import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
GUARD = os.path.join(HERE, "release_guard.py")


def guard(*args):
    return subprocess.run([sys.executable, "-I", GUARD, *args], capture_output=True, text=True)


class ReleaseGuard(unittest.TestCase):
    def source(self, text):
        d = tempfile.mkdtemp(dir=os.environ.get("TMPDIR"))
        with open(os.path.join(d, "x.c"), "w") as f:
            f.write(text)
        return d

    def test_a_clean_source_passes(self):
        self.assertEqual(guard("--sources", self.source("int f(void) { return 0; }\n")).returncode, 0)

    def test_dlopen_in_a_runtime_source_fails(self):
        r = guard("--sources", self.source('void f(void) { dlopen("x", 0); }\n'))
        self.assertEqual(r.returncode, 1)
        self.assertIn("dlopen outside hostlib.c", r.stderr)

    def test_load_library_in_a_runtime_source_fails(self):
        self.assertEqual(guard("--sources", self.source('void f(void) { LoadLibraryW(L"x"); }\n')).returncode, 1)

    def test_a_driver_load_outside_hostlib_fails(self):
        r = guard("--sources", self.source("void f(void) { adrenotools_open_libvulkan(0, 0, 0, 0, 0, 0, 0, 0); }\n"))
        self.assertEqual(r.returncode, 1)
        self.assertIn("adrenotools_open_libvulkan outside hostlib.c", r.stderr)

    def test_a_driver_load_in_hostlib_passes(self):
        d = tempfile.mkdtemp(dir=os.environ.get("TMPDIR"))
        with open(os.path.join(d, "hostlib.c"), "w") as f:
            f.write("void f(void) { adrenotools_open_libvulkan(0, 0, 0, 0, 0, 0, 0, 0); }\n")
        self.assertEqual(guard("--sources", d).returncode, 0)

    def test_an_object_that_loads_a_driver_fails(self):
        d = tempfile.mkdtemp(dir=os.environ.get("TMPDIR"))
        with open(os.path.join(d, "window.o"), "wb") as f:
            f.write(b"\x7fELF....\0adrenotools_open_libvulkan\0")
        r = guard("--objects", d)
        self.assertEqual(r.returncode, 1)
        self.assertIn("imports a library loader", r.stderr)

    def test_executable_memory_fails(self):
        r = guard("--sources", self.source("int p = PROT_READ | PROT_EXEC;\n"))
        self.assertEqual(r.returncode, 1)
        self.assertIn("PROT_EXEC", r.stderr)

    def test_a_hot_load_object_fails(self):
        d = tempfile.mkdtemp(dir=os.environ.get("TMPDIR"))
        with open(os.path.join(d, "devload.o"), "wb") as f:
            f.write(b"\x7fELF....RELIFT_DEV_SOCKET\0")
        r = guard("--objects", d)
        self.assertEqual(r.returncode, 1)
        self.assertIn("hot-load", r.stderr)

    def elf(self, extra):
        elf = bytearray(0x200)
        elf[0:4] = b"\x7fELF"
        elf[4] = 2
        d = tempfile.mkdtemp(dir=os.environ.get("TMPDIR"))
        path = os.path.join(d, "program")
        with open(path, "wb") as f:
            f.write(bytes(elf) + extra)
        return path

    def test_a_binary_with_the_cpu_interpreter_fails_the_interpreter_check(self):
        path = self.elf(b"\0rlgbd_exec_native\0")
        self.assertEqual(guard("--binary", path).returncode, 0)
        r = guard("--binary", path, "--no-interpreter")
        self.assertEqual(r.returncode, 1)
        self.assertIn("CPU interpreter (rlgbd_exec_native)", r.stderr)

    def test_a_binary_without_the_cpu_interpreter_passes_the_interpreter_check(self):
        self.assertEqual(guard("--binary", self.elf(b"\0rlgbd_control\0"), "--no-interpreter").returncode, 0)

    def test_a_writable_executable_segment_fails(self):
        import struct

        elf = bytearray(0x200)
        elf[0:4] = b"\x7fELF"
        elf[4] = 2
        struct.pack_into("<Q", elf, 0x20, 0x40)
        struct.pack_into("<HH", elf, 0x36, 56, 1)
        struct.pack_into("<II", elf, 0x40, 1, 7)
        d = tempfile.mkdtemp(dir=os.environ.get("TMPDIR"))
        path = os.path.join(d, "program")
        with open(path, "wb") as f:
            f.write(elf)
        r = guard("--binary", path)
        self.assertEqual(r.returncode, 1)
        self.assertIn("writable and executable", r.stderr)


if __name__ == "__main__":
    unittest.main()
