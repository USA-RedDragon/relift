# A64 RCpc litmus tests

`litmus.c` holds three two-thread tests (store-buffering with LDAPR and with
LDAR, message passing with LDAPR). Build and run natively on an arm64 host with
FEAT_LRCPC:

    zig cc -target aarch64-linux-musl -static -O2 -mcpu=generic+rcpc -o litmus.arm64 litmus.c

The outcome counts are the hardware's allowed set. On a Cortex-X3 (Thor) SB with
LDAPR shows both loads reading 0 (allowed by RCpc); SB with LDAR never does, and
MP never reads flag=1 with data=0. A lifted run must show no outcome marked
FORBIDDEN; the thread bodies are plain functions so a lifted build of the
binary can call them from a host harness.
