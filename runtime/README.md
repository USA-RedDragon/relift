# relift C runtime

The C half of relift: lifted code links against it. The contract is
`cpus/a64/include/relift/a64/abi.h`, explained in `../docs/abi.md`.

| Directory | What it holds |
|---|---|
| `core/` | Guest-neutral: guest memory reservation and the exact 4 KiB map with the host page policy of abi.md section 2, the SIGSEGV/SIGBUS handler, host threads, FPCR, the clock, the dispatch table, logging and configuration |
| `switch/` | The Horizon kernel (handle table, events, threads, memory, shared and transfer memory, mutexes, condition variables, the address arbiter), `rl_svc`, IPC (HIPC, CMIF with domains, TIPC), `sm:` and the service registry |
| `tests/` | C unit tests that drive the runtime through `rl_ctx` and `rl_svc` the way lifted code does |

## Build and test

relift drives real builds. For work on the runtime itself:

```sh
make -C runtime -j2 test                     # clang, host target, into runtime/build/host
make -C runtime -j2 CC="zig cc" AR="zig ar" TARGET=aarch64-macos BUILD=build/mac
```

The build makes `librelift_rt.a`, `librelift_rt.so` (or `.dylib`) built with
`RL_BUILDING_RUNTIME`, and `rt_tests`. `rt_tests [substring...]` runs the
tests whose names contain a substring; each test runs in its own process.
`RELIFT_TEST_NOFORK=1` runs them in one process for a debugger.

## Environment switches

| Variable | Effect |
|---|---|
| `RELIFT_TRACE_SVC=1` | Log every svc and IPC command with arguments, result and time |
| `RELIFT_STRICT=1` | Stop on an unimplemented svc or unknown service command instead of returning an error |
| `RELIFT_LOG=debug\|info\|warn\|error` | Lowest log level printed |
| `RELIFT_HOST_PAGE=16384` | Manage guest memory in 16 KiB host pages on a 4 KiB host, to test the Apple and Android policy |
| `RELIFT_ADDRESS_BITS=36` | Use the 36-bit layout even when the NPDM asks for 39 bits (the fallback when the host cannot reserve 2^39 bytes) |

## Known limits

- Shared memory is mapped by mapping the same host pages again, so on 16 KiB
  host pages its guest address and size must be 16 KiB aligned; other
  mappings fail with an invalid address result and a one-time report.
- 32-bit address spaces are not supported.
- Guest threads run on host threads at host priority; guest priorities order
  the kernel's wait queues only.
