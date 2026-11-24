"""Off-CPU time by stack from a simpleperf profile, with frames named by addr2line.

Record on the device with `simpleperf record -p <pid> -e cpu-clock --trace-offcpu --call-graph fp`, then
`simpleperf report-sample --show-callchain -i perf.data > rs.txt` and pull it with the unstripped library
(the build's own `libfe.so`, not the APK's stripped copy; `--dso libfe.so` when the copy is renamed).

    python3 -I offcpu.py rs.txt --lib libfe.so [--a2l llvm-addr2line] [--threads]
                         [--thread TID | --pick REGEX] [--depth N] [--top N]

`--threads` lists every thread's off-CPU and on-CPU time with its most common named frame. `--pick` chooses the
thread whose off-CPU stacks most often hold a frame matching REGEX (`nvn_vk_run` picks the render thread);
`--thread` names one. The report groups that thread's off-CPU time by its first `--depth` frames in the library.

Frames are named with addr2line, including inlined calls (`inner/outer`, innermost first), because the library's
runtime functions are hidden and the nearest exported symbol names the wrong function. A caller frame is a return
address, so it is looked up at the address before it; the sample's own frame is looked up as is.
"""
import argparse
import os
import re
import subprocess
import sys
from collections import Counter, defaultdict


def parse(path, dso, others=False, no_kernel=False):
    """Yields ('sample', tid, name, event, time, frames), ('out', tid, time) and ('on', tid, time).

    frames, innermost first, are the library's vaddrs, plus 'symbol(file)' strings for other files with others.
    """
    rec = None
    sw = None
    leaf = True
    va = 0
    cur = ""
    with open(path, errors="replace") as fh:
        for line in fh:
            if not line.startswith(" "):
                if rec is not None:
                    yield ("sample",) + tuple(rec)
                rec = None
                if line.startswith("sample:"):
                    rec = [0, "", "", 0, []]
                    leaf = True
                    sw = None
                elif line.startswith("context_switch:"):
                    sw = {}
                else:
                    sw = None
                continue
            t = line.strip()
            if sw is not None:
                k, _, v = t.partition(": ")
                sw[k] = v
                if k == "thread_id":
                    yield ("on" if sw.get("switch_on") == "true" else "out", int(v), int(sw["time"]))
                continue
            if rec is None:
                continue
            k, _, v = t.partition(": ")
            if k == "vaddr_in_file":
                va = int(v, 16)
            elif k == "file":
                cur = v
            elif k == "symbol":
                # simpleperf prints each frame as vaddr_in_file, file, symbol
                if cur.endswith("/" + dso) or cur == dso:
                    rec[4].append(va if leaf else va - 4)
                elif others and not (no_kernel and cur.startswith("[kernel")):
                    rec[4].append(f"{v}({os.path.basename(cur)})")
                leaf = False
            elif k == "thread_id":
                rec[0] = int(v)
            elif k == "thread_name":
                rec[1] = v
            elif k == "event_type":
                rec[2] = v
            elif k == "time":
                rec[3] = int(v)
    if rec is not None:
        yield ("sample",) + tuple(rec)


def name_addrs(a2l, lib, addrs):
    """Maps each address to its inline chain, 'fn@file:line/caller@file:line', innermost first."""
    addrs = sorted(addrs)
    if not addrs:
        return {}
    inp = "".join(f"0x{a:x}\n" for a in addrs)
    out = subprocess.run([a2l, "-a", "-f", "-i", "-C", "-e", lib], input=inp, capture_output=True, text=True,
                         check=True).stdout.split("\n")
    names = {}
    cur = None
    chain = []
    i = 0
    while i < len(out):
        ln = out[i]
        if ln.startswith("0x"):
            if cur is not None:
                names[cur] = "/".join(chain)
            cur, chain = int(ln, 16), []
            i += 1
            continue
        if cur is not None and ln and i + 1 < len(out):
            where = out[i + 1]
            where = re.sub(r"^.*?/(runtime|guests|core)/", r"\1/", where)
            where = re.sub(r" \(discriminator \d+\)", "", where)
            chain.append(f"{ln}@{where}")
            i += 2
            continue
        i += 1
    if cur is not None:
        names[cur] = "/".join(chain)
    return names


def offcpu(events):
    """Off-CPU intervals from the context switch records, from a thread's switch out to its switch on.

    Each interval takes the stack of the thread's sched_switch sample taken since its last switch on. simpleperf does
    not sample every switch out (on a Thor run, 1294 samples for 1461 switches holding over half the off time), so an
    interval without one is kept with no stack instead of being dropped.
    """
    since_on = {}  # tid -> stack of the sched_switch sample since the last switch on
    out_at = {}
    spans = []  # (tid, ns, frames)
    oncpu = Counter()
    tname = {}
    for ev in events:
        if ev[0] == "out":
            out_at[ev[1]] = ev[2]
            continue
        if ev[0] == "on":
            _, tid, time = ev
            t0 = out_at.pop(tid, None)
            stack = since_on.pop(tid, None)
            if t0 is not None and time >= t0:
                spans.append((tid, time - t0, stack if stack is not None else ("(no stack sampled)",)))
            continue
        _, tid, name, event, time, vas = ev
        tname.setdefault(tid, name)
        if event == "cpu-clock":
            oncpu[tid] += 1
        else:
            since_on[tid] = tuple(vas)
    return spans, oncpu, tname


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("report")
    ap.add_argument("--lib", required=True)
    ap.add_argument("--dso", help="library file name in the profile (default: the --lib file name)")
    ap.add_argument("--others", action="store_true", help="keep frames of other files, named by their symbol")
    ap.add_argument("--no-kernel", action="store_true", help="with --others, drop kernel frames")
    ap.add_argument("--a2l", default="llvm-addr2line")
    ap.add_argument("--threads", action="store_true")
    ap.add_argument("--thread", type=int)
    ap.add_argument("--pick")
    ap.add_argument("--depth", type=int, default=4)
    ap.add_argument("--top", type=int, default=15)
    ap.add_argument("--hz", type=float, default=2000.0, help="cpu-clock sample rate, for on-CPU ms")
    a = ap.parse_args(argv)
    spans, oncpu, tname = offcpu(parse(a.report, a.dso or os.path.basename(a.lib), a.others, a.no_kernel))
    addrs = {v for _, _, vas in spans for v in vas if isinstance(v, int)}
    names = name_addrs(a.a2l, a.lib, addrs)

    def fr(v):
        if isinstance(v, str):
            return v
        return names.get(v) or f"0x{v:x}"

    off = Counter()
    for tid, ns, _ in spans:
        off[tid] += ns
    if a.threads:
        top = defaultdict(Counter)
        for tid, ns, vas in spans:
            if vas:
                top[tid][fr(vas[0]).split("@")[0]] += ns
        for tid in sorted(set(off) | set(oncpu), key=lambda t: -off[t]):
            best = top[tid].most_common(1)
            print(f"{tid:7d} {tname.get(tid, ''):20s} off {off[tid] / 1e6:8.0f} ms  on {oncpu[tid] * 1e3 / a.hz:7.0f} ms"
                  f"  {best[0][0] if best else ''}")
        return 0
    tid = a.thread
    if tid is None and a.pick:
        rx = re.compile(a.pick)
        hits = Counter()
        for t, ns, vas in spans:
            if any(rx.search(fr(v)) for v in vas):
                hits[t] += 1
        if not hits:
            print(f"no thread has an off-CPU frame matching {a.pick}", file=sys.stderr)
            return 1
        tid = hits.most_common(1)[0][0]
    if tid is None:
        tid = off.most_common(1)[0][0]
    by = Counter()
    for t, ns, vas in spans:
        if t == tid:
            by[" < ".join(fr(v) for v in vas[:a.depth]) or "(no library frame)"] += ns
    print(f"thread {tid} ({tname.get(tid, '')}): off CPU {off[tid] / 1e6:.0f} ms, on CPU {oncpu[tid] * 1e3 / a.hz:.0f} ms")
    for k, v in by.most_common(a.top):
        print(f"{v / 1e6:8.0f} ms  {k}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
