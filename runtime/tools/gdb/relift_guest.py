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

# Frames per thread for the current stop: {thread number: [(pc, sp, fp), ...]}.
_cache = {}


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
        if reply and reply[0] != "E":
            frames = [tuple(int(v, 16) for v in f.split(",")) for f in reply.split(";")]
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
        if pending.architecture().name() != "aarch64":
            return None
        thread = gdb.selected_thread()
        frames = _cache.get(thread.num) if thread else None
        if not frames:
            return None
        pcv = pending.read_register("pc")
        spv = pending.read_register("sp")
        fpv = pending.read_register("x29")
        pc, sp = int(pcv), int(spv)
        for i, (fpc, fsp, _) in enumerate(frames):
            if fpc != pc or fsp != sp:
                continue
            info = pending.create_unwind_info(_FrameId(sp, pc))
            if i + 1 < len(frames):
                npc, nsp, nfp = frames[i + 1]
            else:
                # The outermost guest frame: a zero return address ends the backtrace.
                npc, nsp, nfp = 0, sp, 0
            info.add_saved_register("pc", gdb.Value(npc).cast(pcv.type))
            info.add_saved_register("sp", gdb.Value(nsp).cast(spv.type))
            info.add_saved_register("x29", gdb.Value(nfp).cast(fpv.type))
            return info
        return None


gdb.unwinder.register_unwinder(None, ReliftGuestUnwinder(), replace=True)
_prefetch()
