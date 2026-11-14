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

The demangling is gdb's own `demangle -l c++`. Standard library only.
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


GuestFrameFilter()
GuestNameCommand()
