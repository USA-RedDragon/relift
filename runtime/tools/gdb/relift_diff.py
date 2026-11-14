"""Run lifted guest functions against a reference engine, from gdb.

    (gdb) source runtime/tools/gdb/relift_diff.py
    (gdb) relift-diff _ZN5guest4main3fooEv 3

At each of the next 3 calls of the lifted function, the command runs the
same guest function from the same rl_ctx and live guest memory in the
reference engine (`relift diff --serve`, the IR interpreter), lets the lifted
code run with every other thread frozen, and compares the registers and every
page the engine touched. The engine stops, and says so, when the function
reaches a system call or a runtime-provided function: nothing is compared
then.

    relift-diff SYMBOL [COUNT [SKIP]]

SYMBOL is the host name of a lifted function. COUNT calls are checked (default
1) after SKIP calls pass (default 0). The environment names the engine:
RELIFT_DIFF_ENGINE is its command line (default `relift diff --serve`).
Most people run this through `relift diff`, which builds, starts the program
under gdb and sets the environment.

Needs x86-64 System V or AArch64 hosts, gdb with Python, and a lifted
function that is not inlined or tail-called into. The guest address of the
function is found by reading the dispatch table through the context
(docs/abi.md, section 3). Standard library only.
"""

import os
import shlex
import struct
import subprocess

import gdb

PAGE = 4096
CTX_SIZE = 896
X_OFF, SP_OFF, V_OFF, NZCV_OFF, FPCR_OFF, TPIDR_OFF, TPIDRRO_OFF = 0, 248, 256, 776, 780, 792, 800
DISP_TABLE_OFF, DISP_LO_OFF, DISP_COUNT_OFF = 872, 880, 888
GUEST_LIMIT = 1 << 40

_guest_pc = {}
_totals = {}


def host_args():
    arch = gdb.selected_frame().architecture().name()
    if "x86-64" in arch:
        return "$rdi", "$rsi"
    if "aarch64" in arch:
        return "$x0", "$x1"
    raise gdb.GdbError("relift-diff: no argument registers for " + arch)


def read(addr, n):
    return bytes(gdb.selected_inferior().read_memory(addr, n))


def hex_of(v):
    return "%x" % v


def context(c):
    b = read(c, CTX_SIZE)
    regs = [("x%d" % i, struct.unpack_from("<Q", b, X_OFF + 8 * i)[0]) for i in range(31)]
    regs.append(("sp", struct.unpack_from("<Q", b, SP_OFF)[0]))
    nzcv, fpcr = struct.unpack_from("<II", b, NZCV_OFF)
    tp, tpro = struct.unpack_from("<QQ", b, TPIDR_OFF)
    regs += [("nzcv", nzcv), ("fpcr", fpcr), ("tpidr_el0", tp), ("tpidrro_el0", tpro)]
    for i in range(32):
        lo, hi = struct.unpack_from("<QQ", b, V_OFF + 16 * i)
        regs.append(("v%d" % i, (hi << 64) | lo))
    return dict(regs), [n for n, _ in regs]


def after_line(c):
    values, order = context(c)
    skip = ("nzcv", "tpidr_el0", "tpidrro_el0")
    return "AFTER " + " ".join("%s=%x" % (n, values[n]) for n in order if n not in skip)


def guest_pc(c, host):
    """The guest address whose dispatch table slot holds `host`."""
    if host in _guest_pc:
        return _guest_pc[host]
    tab, lo, count = struct.unpack("<QQQ", read(c + DISP_TABLE_OFF, 24))
    want = struct.pack("<Q", host)
    data = read(tab, count * 8)
    at = data.find(want)
    while at >= 0 and at % 8:
        at = data.find(want, at + 1)
    if at < 0:
        raise gdb.GdbError("relift-diff: no dispatch table slot holds %#x" % host)
    _guest_pc[host] = lo + (at // 8) * 4
    return _guest_pc[host]


def serve_pages(engine, mem):
    """Answers page requests until the engine sends RAN; returns its line."""
    while True:
        line = engine.stdout.readline().decode().rstrip("\n")
        if not line:
            return None
        if line.startswith("R "):
            page = int(line[2:], 16)
            data = None
            if page < GUEST_LIMIT:
                try:
                    data = read(mem + page, PAGE)
                except gdb.MemoryError:
                    data = None
            engine.stdin.write(b"NO\n" if data is None else b"OK\n" + data)
            engine.stdin.flush()
        else:
            return line


def read_result(engine, mem):
    """Reads protocol lines after RAN until RESULT; answers page requests."""
    while True:
        line = serve_pages(engine, mem)
        if line is None or line.startswith("RESULT"):
            return line


def run_case(cmd, name, number, c, mem, host_addr, bp):
    values, order = context(c)
    pc = guest_pc(c, host_addr)
    state = "STATE pc=%x stop=%x" % (pc, values["x30"])
    state += "".join(" %s=%x" % (n, values[n]) for n in order)
    engine = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    try:
        engine.stdin.write((state + "\n").encode())
        engine.stdin.flush()
        ran = serve_pages(engine, mem)
        if ran is None:
            return "stopped", "the engine ended without a verdict"
        if ran.startswith("RAN ok"):
            bp.enabled = False
            gdb.execute("set scheduler-locking on")
            try:
                gdb.execute("finish", to_string=True)
            finally:
                gdb.execute("set scheduler-locking off")
                bp.enabled = True
            engine.stdin.write((after_line(c) + "\n").encode())
            engine.stdin.flush()
        result = read_result(engine, mem)
        if result is None:
            return "stopped", "the engine ended without a verdict"
        word, _, detail = result.split(" ", 1)[1].partition(" ")
        return word, detail
    finally:
        engine.stdin.close()
        engine.wait()


class RelDiff(gdb.Command):
    """Run lifted guest functions against the reference engine: relift-diff SYMBOL [COUNT [SKIP]]."""

    def __init__(self):
        super().__init__("relift-diff", gdb.COMMAND_RUNNING)

    def invoke(self, arg, from_tty):
        words = shlex.split(arg)
        if not words:
            raise gdb.GdbError("usage: relift-diff SYMBOL [COUNT [SKIP]]")
        name = words[0]
        count = int(words[1]) if len(words) > 1 else 1
        skip = int(words[2]) if len(words) > 2 else 0
        cmd = shlex.split(os.environ.get("RELIFT_DIFF_ENGINE", "relift diff --serve"))
        bp = gdb.Breakpoint("*" + name, internal=True)
        tally = {}
        done = 0
        seen = 0
        try:
            while done < count:
                gdb.execute("continue", to_string=True)
                if not gdb.selected_inferior().pid:
                    gdb.write("relift-diff: the program exited\n")
                    break
                host_addr = bp.locations[0].address
                if int(gdb.parse_and_eval("$pc")) != host_addr:
                    continue
                seen += 1
                if seen <= skip:
                    continue
                a0, a1 = host_args()
                c = int(gdb.parse_and_eval(a0))
                mem = int(gdb.parse_and_eval(a1))
                word, detail = run_case(cmd, name, done + 1, c, mem, host_addr, bp)
                tally[word] = tally.get(word, 0) + 1
                done += 1
                gdb.write("relift-diff: %s call %d: %s %s\n" % (name, done, word.upper(), detail))
        finally:
            bp.delete()
        total = " ".join("%s=%d" % kv for kv in sorted(tally.items()))
        gdb.write("relift-diff: SUMMARY %s %s\n" % (name, total or "no calls"))
        for k, v in tally.items():
            _totals[k] = _totals.get(k, 0) + v


class Start(gdb.Command):
    """Prepare a batch run and stop at the program's first instruction: relift-diff-start."""

    def __init__(self):
        super().__init__("relift-diff-start", gdb.COMMAND_RUNNING)

    def invoke(self, arg, from_tty):
        gdb.execute("set pagination off")
        gdb.execute("set confirm off")
        gdb.execute("set print thread-events off")
        gdb.execute("set print inferior-events off")
        gdb.execute("handle SIGSEGV SIGBUS SIGUSR1 SIGUSR2 SIG34 SIG35 SIG36 nostop noprint pass")
        gdb.execute("starti", to_string=True)


class Finish(gdb.Command):
    """Kill the program and quit: status 1 after a mismatch, 2 when nothing was compared."""

    def __init__(self):
        super().__init__("relift-diff-finish", gdb.COMMAND_RUNNING)

    def invoke(self, arg, from_tty):
        if gdb.selected_inferior().pid:
            gdb.execute("kill")
        if _totals.get("mismatch", 0):
            status = 1
        elif not _totals.get("match", 0):
            status = 2
        else:
            status = 0
        gdb.execute("quit %d" % status)


RelDiff()
Start()
Finish()
