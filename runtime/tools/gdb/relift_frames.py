"""Show relift's guest names in gdb backtraces.

relift names each lifted function with a C identifier that is also an
Itanium mangled name inside the guest namespace, for example
_ZN5guest4main3fooEv for guest::main::foo(). lldb, perf and addr2line -C
demangle those names, but gdb does not demangle the name of a function from
a C compilation unit. This file adds a frame filter that does it, so `bt`
shows guest::main::foo() instead of the raw identifier, and a command:

    (gdb) source runtime/tools/gdb/relift_frames.py
    (gdb) bt
    (gdb) relift-guest-name _ZN5guest4main3fooEv
    guest::main::foo()

The demangling is gdb's own `demangle -l c++`.

The device scheduler runs each guest CPU on a fiber (runtime/core/fiber.h),
and `bt` shows only the fiber that is running. Two commands show the others:

    (gdb) relift-fibers
    #0  guest CPU 0  running      stack 0x7ffff5e00000-0x7ffff6e00000
    #1  guest CPU 1  parked       stack 0x7ffff4c00000-0x7ffff5c00000
    (gdb) relift-fiber-bt 1

relift-fiber-bt loads the registers the switch saved on the fiber's stack
into the innermost frame, prints the backtrace and puts the registers back.
x86-64 System V and AArch64 hosts. Standard library only.
"""

import re

import gdb
from gdb.FrameDecorator import FrameDecorator

GUEST_PREFIX = "_ZN5guest"
HOST_NAME = re.compile(r"^_ZN5guest[A-Za-z0-9_]+$")

_names = {}


def guest_name(host):
    """The demangled guest name for a relift host name, or None."""
    if not isinstance(host, str) or not HOST_NAME.match(host):
        return None
    if host in _names:
        return _names[host]
    try:
        out = gdb.execute("demangle -l c++ -- " + host, to_string=True).strip()
    except gdb.error:
        out = ""
    name = out if out.startswith("guest::") else None
    _names[host] = name
    return name


class GuestFrame(FrameDecorator):
    """A frame whose function name is shown as its guest name."""

    def function(self):
        name = super().function()
        return guest_name(name) or name


class GuestFrameFilter:
    """Wraps every frame whose function is a relift host name."""

    def __init__(self):
        self.name = "relift-guest-names"
        self.priority = 100
        self.enabled = True
        gdb.frame_filters[self.name] = self

    def filter(self, frames):
        return map(GuestFrame, frames)


class GuestNameCommand(gdb.Command):
    """Print the guest name of a relift host name: relift-guest-name NAME."""

    def __init__(self):
        super().__init__("relift-guest-name", gdb.COMMAND_DATA)

    def invoke(self, arg, from_tty):
        host = arg.strip()
        name = guest_name(host)
        if name is None:
            raise gdb.GdbError("not a relift guest name: " + host)
        gdb.write(name + "\n")


# Registers rl_fiber_swap saves, as (name, byte offset from the saved sp),
# the register that gets the return address, and the frame size.
SWITCH_FRAMES = {
    "x86-64": ([("r15", 0), ("r14", 8), ("r13", 16), ("r12", 24), ("rbx", 32), ("rbp", 40)], "rip", 48, 56),
    "aarch64": ([("x%d" % (19 + i), 8 * i) for i in range(10)] + [("x29", 80)], "pc", 88, 160),
}


def switch_frame():
    arch = gdb.newest_frame().architecture().name()
    if "x86-64" in arch:
        return SWITCH_FRAMES["x86-64"]
    if "aarch64" in arch:
        return SWITCH_FRAMES["aarch64"]
    raise gdb.GdbError("relift fibers: no switch frame layout for " + arch)


def live_fibers():
    """(index, rl_fiber value) for every listed fiber."""
    try:
        live = gdb.parse_and_eval("rl_fiber_live")
    except gdb.error:
        raise gdb.GdbError("relift fibers: rl_fiber_live not found (no fibers linked, or no debug info)")
    lo, hi = live.type.range()
    out = []
    for i in range(lo, hi + 1):
        p = live[i]
        if int(p) != 0:
            out.append((i, p.dereference()))
    return out


def fiber_state(f, sp):
    lo, hi = int(f["alloc"]), int(f["top"])
    if lo <= sp < hi:
        return "running"
    if int(f["sp"]) == int(f["first_sp"]):
        return "not started"
    return "parked"


def current_sp():
    return int(gdb.newest_frame().read_register("sp"))


class FibersCommand(gdb.Command):
    """List the live relift fibers (guest CPUs): relift-fibers."""

    def __init__(self):
        super().__init__("relift-fibers", gdb.COMMAND_STACK)

    def invoke(self, arg, from_tty):
        sp = current_sp()
        for i, f in live_fibers():
            label = f["label"].string() or "fiber"
            gdb.write("#%-2d %-14s %-12s stack 0x%x-0x%x\n" % (i, label, fiber_state(f, sp), int(f["limit"]),
                                                                 int(f["top"])))


class FiberBacktraceCommand(gdb.Command):
    """Backtrace a parked relift fiber: relift-fiber-bt N [bt arguments]."""

    def __init__(self):
        super().__init__("relift-fiber-bt", gdb.COMMAND_STACK)

    def invoke(self, arg, from_tty):
        args = gdb.string_to_argv(arg)
        if not args:
            raise gdb.GdbError("usage: relift-fiber-bt N (N from relift-fibers)")
        want = int(args[0], 0)
        fibers = dict(live_fibers())
        if want not in fibers:
            raise gdb.GdbError("relift-fiber-bt: no live fiber #%d" % want)
        f = fibers[want]
        state = fiber_state(f, current_sp())
        if state == "running":
            gdb.execute("bt " + " ".join(args[1:]))
            return
        if state == "not started":
            gdb.write("fiber #%d has not run yet\n" % want)
            return
        saved, pc_reg, ret_off, size = switch_frame()
        base = int(f["sp"])
        inferior = gdb.selected_inferior()

        def word(off):
            return int.from_bytes(bytes(inferior.read_memory(base + off, 8)), "little")

        gdb.newest_frame().select()
        frame = gdb.newest_frame()
        names = [n for n, _ in saved] + [pc_reg, "sp"]
        old = {n: int(frame.read_register(n)) for n in names}
        new = {n: word(off) for n, off in saved}
        new[pc_reg] = word(ret_off)
        new["sp"] = base + size
        try:
            for n in names:
                gdb.execute("set $%s = %d" % (n, new[n]), to_string=True)
            gdb.execute("bt " + " ".join(args[1:]))
        finally:
            for n in names:
                gdb.execute("set $%s = %d" % (n, old[n]), to_string=True)


GuestFrameFilter()
GuestNameCommand()
FibersCommand()
FiberBacktraceCommand()
