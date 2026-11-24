"""Tests offcpu.py on a known stack: a small C library built with debug info, whose hidden inline function a
simpleperf report names only by the nearest exported symbol.

    python3 -I runtime/tools/test_offcpu.py

Needs cc and llvm-addr2line (else it says it skipped).
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import offcpu  # noqa: E402

SRC = r"""
__attribute__((visibility("default"))) void exported_wait(void);
static void spin(volatile int *p) { while (*p) ; }
__attribute__((noinline, visibility("hidden"))) void hidden_wait(volatile int *p) { spin(p); __asm__ volatile(""); }
__attribute__((visibility("default"))) void exported_wait(void) { volatile int x = 0; hidden_wait(&x); }
"""


def symbol_addr(lib, name):
    out = subprocess.run(["nm", lib], capture_output=True, text=True, check=True).stdout
    for ln in out.splitlines():
        p = ln.split()
        if len(p) == 3 and p[2] == name:
            return int(p[0], 16)
    raise AssertionError(f"{name} not in {lib}")


def report(lib, leaf, caller_ret):
    """A report-sample text: thread 7 runs, switches out with a sched_switch sample at leaf <- caller_ret, then is
    off CPU for 5 ms with no sample; thread 8 has one cpu-clock sample."""
    base = os.path.basename(lib)
    frames = (f"  vaddr_in_file: {leaf:x}\n  file: /data/app/x/{base}\n  symbol: exported_wait\n"
              f"  callchain:\n    vaddr_in_file: {caller_ret:x}\n    file: /data/app/x/{base}\n"
              f"    symbol: exported_wait\n    vaddr_in_file: 1234\n    file: /system/lib64/libc.so\n"
              f"    symbol: __start_thread\n")
    return (
        "meta_info:\n  trace_offcpu: true\n"
        "context_switch:\n  switch_on: true\n  time: 1000\n  thread_id: 7\n"
        "sample:\n  event_type: sched:sched_switch\n  time: 2000000\n  event_count: 1\n  thread_id: 7\n"
        "  thread_name: render\n" + frames +
        "context_switch:\n  switch_on: false\n  time: 2000000\n  thread_id: 7\n"
        "context_switch:\n  switch_on: true\n  time: 7000000\n  thread_id: 7\n"
        # out again without a sched_switch sample: 3 ms with no stack
        "context_switch:\n  switch_on: false\n  time: 8000000\n  thread_id: 7\n"
        "context_switch:\n  switch_on: true\n  time: 11000000\n  thread_id: 7\n"
        "sample:\n  event_type: cpu-clock\n  time: 12000000\n  event_count: 1\n  thread_id: 8\n  thread_name: other\n"
        + frames)


def main():
    cc = shutil.which("cc") or shutil.which("clang")
    a2l = shutil.which("llvm-addr2line")
    if not cc or not a2l or not shutil.which("nm"):
        print("test_offcpu: no cc, nm or llvm-addr2line, skipped")
        return 0
    d = tempfile.mkdtemp(prefix="offcpu-test-", dir=os.environ.get("TMPDIR"))
    try:
        src = os.path.join(d, "w.c")
        lib = os.path.join(d, "libw.so")
        with open(src, "w") as f:
            f.write(SRC)
        subprocess.run([cc, "-O1", "-g", "-shared", "-fPIC", "-o", lib, src], check=True)
        hidden = symbol_addr(lib, "hidden_wait")
        exported = symbol_addr(lib, "exported_wait")
        # the leaf inside hidden_wait's inlined spin loop, the caller frame a return address in exported_wait
        names = offcpu.name_addrs(a2l, lib, [hidden + 4])
        leaf_name = names[hidden + 4]
        assert leaf_name.split("@")[0] in ("spin", "hidden_wait"), leaf_name
        ret = exported + 0x10
        path = os.path.join(d, "rs.txt")
        with open(path, "w") as f:
            f.write(report(lib, hidden + 4, ret))
        spans, oncpu, tname = offcpu.offcpu(offcpu.parse(path, "libw.so"))
        mine = [s for s in spans if s[0] == 7]
        assert [s[1] for s in mine] == [5000000, 3000000], mine
        assert mine[0][2] == (hidden + 4, ret - 4), mine[0][2]
        assert mine[1][2] == ("(no stack sampled)",), mine[1][2]
        assert oncpu[8] == 1 and tname[7] == "render"
        n = offcpu.name_addrs(a2l, lib, [hidden + 4, ret - 4])
        assert "hidden_wait" in n[hidden + 4], n  # the hidden function, never the nearest exported one
        assert n[ret - 4].startswith("exported_wait@"), n
        if "/" in n[hidden + 4]:
            assert n[hidden + 4].startswith("spin@"), n  # the inlined call comes first
        other = [v for v in offcpu.offcpu(offcpu.parse(path, "libw.so", others=True))[0][0][2] if isinstance(v, str)]
        assert other == ["__start_thread(libc.so)"], other
        print("test_offcpu: ok")
        return 0
    finally:
        shutil.rmtree(d)


if __name__ == "__main__":
    sys.exit(main())
