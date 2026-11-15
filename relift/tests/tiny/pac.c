// A guest program built with return-address signing under the B key
// (-mbranch-protection=pac-ret+b-key, and +leaf in pac_leaf.bin): calls
// through signed frames (PACIBSP, AUTIBSP, RETAB), signs and authenticates
// a pointer with PACIB1716 and AUTIB1716, strips one with XPACLRI and
// checks that PACGA is a function of its inputs. It prints one line per
// check. When the byte at data+0x10 is 1, a signed function overwrites its
// own saved return address before it returns, so the authenticated return
// fails and the process must crash before it prints "returned".
// Rebuild pac.bin and pac_leaf.bin with `sh pac.sh`.

__asm__(".section .text.start, \"ax\"\n"
        "        .globl _start\n"
        "_start: b start\n"
        "        .long mod0 - _start\n"
        "mod0:   .ascii \"MOD0\"\n"
        "        .long 0x2000 - 8\n"
        "        .long 0x3000 - 8\n"
        "        .long 0x4000 - 8\n"
        "        .long 0\n"
        "        .long 0\n"
        "        .long 0x2000 - 8\n"
        "start:  adr x9, _start\n"
        "        add x9, x9, #4, lsl #12\n"
        "        mov sp, x9\n"
        "        bl main\n"
        "        svc #0x7\n"
        "        b .\n");

typedef unsigned long u64;

extern volatile unsigned char data_start[];

static void print(const char *s) {
    u64 n = 0;
    while (s[n])
        n++;
    register const char *x0 __asm__("x0") = s;
    register u64 x1 __asm__("x1") = n;
    __asm__ volatile("svc #0x27" : : "r"(x0), "r"(x1) : "memory");
}

static void check(const char *what, int ok) {
    print(ok ? what : "FAIL");
}

__attribute__((noinline)) static u64 leaf(u64 x) {
    __asm__ volatile("" : "+r"(x));
    return x * 7 + 3;
}

__attribute__((noinline)) static u64 deep(u64 x) {
    return leaf(x) + leaf(x + 1);
}

__attribute__((noinline)) static u64 middle(u64 x) {
    u64 (*through)(u64) = deep;
    __asm__ volatile("" : "+r"(through));
    return through(x) * 2 + leaf(x);
}

__attribute__((noinline)) static u64 outer(u64 x) {
    u64 a = middle(x);
    u64 b = middle(a & 0xff);
    return a ^ b;
}

static u64 pacib1716(u64 p, u64 m) {
    register u64 x17 __asm__("x17") = p;
    register u64 x16 __asm__("x16") = m;
    __asm__ volatile("pacib1716" : "+r"(x17) : "r"(x16));
    return x17;
}

static u64 autib1716(u64 p, u64 m) {
    register u64 x17 __asm__("x17") = p;
    register u64 x16 __asm__("x16") = m;
    __asm__ volatile("autib1716" : "+r"(x17) : "r"(x16));
    return x17;
}

static u64 xpaclri(u64 p) {
    register u64 x30 __asm__("x30") = p;
    __asm__ volatile("xpaclri" : "+r"(x30));
    return x30;
}

static u64 pacga(u64 a, u64 b) {
    u64 r;
    __asm__ volatile("pacga %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}

__attribute__((noinline)) static u64 victim(u64 x) {
    u64 *frame = __builtin_frame_address(0);
    frame[1] += 4;
    u64 r = leaf(x);
    __asm__ volatile("" : "+r"(r));
    return r + 1;
}

int main(void) {
    u64 want = 0;
    for (u64 x = 5; x < 9; x++) {
        u64 m = (x * 7 + 3 + (x + 1) * 7 + 3) * 2 + x * 7 + 3;
        u64 n = ((m & 0xff) * 7 + 3 + ((m & 0xff) + 1) * 7 + 3) * 2 + (m & 0xff) * 7 + 3;
        want += m ^ n;
    }
    u64 got = 0;
    for (u64 x = 5; x < 9; x++)
        got += outer(x);
    check("signed frames return", got == want);

    u64 p = (u64)&main;
    u64 s = pacib1716(p, 0x1234);
    check("pacib1716 signs", s != p && (s & 0x7fffffffffULL) == p);
    check("autib1716 authenticates", autib1716(s, 0x1234) == p);
    u64 bad = autib1716(s, 0x1235);
    check("autib1716 with another modifier fails", bad != p && (bad >> 39) != 0);
    check("xpaclri strips", xpaclri(s) == p);

    u64 g = pacga(0x1111, 0x2222);
    check("pacga repeats", g == pacga(0x1111, 0x2222));
    check("pacga keeps the low half zero", (g & 0xffffffff) == 0);
    check("pacga depends on its inputs",
          g != pacga(0x1112, 0x2222) && g != pacga(0x1111, 0x2223));

    if (data_start[0x10] == 1) {
        print("corrupting");
        victim(1);
        print("returned");
    }
    print("done");
    return 0;
}
