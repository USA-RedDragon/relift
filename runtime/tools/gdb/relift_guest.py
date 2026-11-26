"""gdb helper for a session on relift's GDB server (runtime/core/gdbstub.c).

The server walks a stopped guest thread's frame records itself and answers
the walk in the qRelift.frames packet; this unwinder hands gdb those frames,
so a plain `bt` shows every guest call. gdb cannot do it alone: without
symbols its aarch64 unwinder reads the frame record 16 bytes past x29 and
stops after the first frame.

    gdb -ex 'source runtime/tools/gdb/relift_guest.py' -ex 'target remote :2345'

It does nothing on a connection that is not relift's server: the packet then
gets an empty reply.
"""

import gdb
import gdb.unwinder
from gdb.FrameDecorator import FrameDecorator

# Frames per thread for the current stop: {thread number: [(pc, sp, fp, name), ...]}.
_cache = {}
# Thread numbers whose connection answered qRelift.frames at all (an error reply counts): relift's server.
_server = set()


def _forget(*_):
    _cache.clear()


for _ev in (gdb.events.cont, gdb.events.memory_changed, gdb.events.register_changed,
            gdb.events.exited):
    _ev.connect(_forget)


def _frames(thread):
    """The server's walk for thread, or None off a relift server."""
    if thread.num in _cache:
        return _cache[thread.num]
    frames = None
    conn = gdb.selected_inferior().connection
    if isinstance(conn, gdb.RemoteTargetConnection):
        tid = thread.ptid[1] or thread.ptid[2]
        try:
            reply = conn.send_packet("qRelift.frames:%x" % tid)
        except gdb.error:
            reply = b""
        if isinstance(reply, bytes):
            reply = reply.decode("ascii", "replace")
        if reply:
            _server.add(thread.num)
        if reply and reply[0] != "E":
            frames = []
            for f in reply.split(";"):
                pc, sp, fp, name = f.split(",")
                frames.append((int(pc, 16), int(sp, 16), int(fp, 16), bytes.fromhex(name).decode("utf-8", "replace")))
    _cache[thread.num] = frames
    return frames


def _prefetch(*_):
    """Asks the server for every thread's walk while gdb is between commands, never inside an unwinder."""
    try:
        inf = gdb.selected_inferior()
        if inf.pid == 0:
            return
        for t in inf.threads():
            _frames(t)
    except gdb.error:
        pass


# A stop and each prompt fetch the walks; gdb cannot send a packet from inside an unwinder.
gdb.events.stop.connect(_prefetch)
gdb.events.before_prompt.connect(_prefetch)


class _FrameId:
    def __init__(self, sp, pc):
        self.sp = sp
        self.pc = pc


class ReliftGuestUnwinder(gdb.unwinder.Unwinder):
    """Unwinds a guest thread on relift's GDB server from the server's frame walk."""

    def __init__(self):
        super().__init__("relift guest")

    def __call__(self, pending):
        arch = pending.architecture().name()
        if arch == "gbz80":
            return self._outermost(pending)
        if arch != "aarch64":
            return None
        thread = gdb.selected_thread()
        frames = _cache.get(thread.num) if thread else None
        if not frames:
            return None
        pcv = pending.read_register("pc")
        spv = pending.read_register("sp")
        fpv = pending.read_register("x29")
        pc, sp = int(pcv), int(spv)
        for i, (fpc, fsp, _, _) in enumerate(frames):
            if fpc != pc or fsp != sp:
                continue
            info = pending.create_unwind_info(_FrameId(sp, pc))
            if i + 1 < len(frames):
                npc, nsp, nfp, _ = frames[i + 1]
            else:
                # The outermost guest frame: a zero return address ends the backtrace.
                npc, nsp, nfp = 0, sp, 0
            # The caller's link register is the return address of the frame after next (0 past the outermost).
            nlr = frames[i + 2][0] if i + 2 < len(frames) else 0
            info.add_saved_register("pc", gdb.Value(npc).cast(pcv.type))
            info.add_saved_register("sp", gdb.Value(nsp).cast(spv.type))
            info.add_saved_register("x29", gdb.Value(nfp).cast(fpv.type))
            info.add_saved_register("x30", gdb.Value(nlr).cast(fpv.type))
            # The callee-saved registers a guest call keeps: the server's walk does not track them, so the caller
            # has the values this frame has. Every register the next frame's unwind may read is then saved, which
            # gdb versions that read x19 to x28 or the link register of a frame need.
            for n in range(19, 29):
                name = "x%d" % n
                try:
                    info.add_saved_register(name, pending.read_register(name))
                except gdb.error:
                    pass
            return info
        return None


    def _outermost(self, pending):
        """A Game Boy thread is its own outermost frame: the program keeps no frame records, and gdb 15's gbz80
        unwinder asserts (trad-frame.h, 'addr') when it computes a frame's id or caller after a register or memory
        write; gdb 16 and later fixed it. Skipped off relift's server, which gdb's own unwinder then handles."""
        thread = gdb.selected_thread()
        if not thread or thread.num not in _server:
            return None
        pcv = pending.read_register("pc")
        spv = pending.read_register("sp")
        info = pending.create_unwind_info(_FrameId(int(spv), int(pcv)))
        # A zero return address ends the backtrace; the registers the cart keeps pass through.
        info.add_saved_register("pc", gdb.Value(0).cast(pcv.type))
        info.add_saved_register("sp", spv)
        for name in ("af", "bc", "de", "hl"):
            try:
                info.add_saved_register(name, pending.read_register(name))
            except gdb.error:
                pass
        return info


gdb.unwinder.register_unwinder(None, ReliftGuestUnwinder(), replace=True)


class ReliftGuestEnd:
    """Drops the zero-pc frame that the outermost guest frame's zero return address leaves after it, so bt ends at
    the outermost guest call, and names each guest frame."""

    def __init__(self):
        self.name = "relift guest end"
        self.priority = 100
        self.enabled = True
        gdb.frame_filters[self.name] = self

    def filter(self, frames):
        return (_Named(f) for f in frames if f.inferior_frame().level() == 0 or f.inferior_frame().pc() != 0)


class _Named(FrameDecorator):
    """A guest frame named for its lifted function (`module+0xoff name`), as the server's walk names it."""

    def function(self):
        frame = self.inferior_frame()
        thread = gdb.selected_thread()
        for f in _cache.get(thread.num) or () if thread else ():
            if f[0] == frame.pc() and f[3]:
                return f[3]
        return super().function()


ReliftGuestEnd()
_prefetch()
