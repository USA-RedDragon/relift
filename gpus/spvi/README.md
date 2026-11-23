# gpus/spvi

A small SPIR-V interpreter (`relift-gpu-spvi`) for the compute programs
relift's GPU translator (`gpus/nv`) emits: one invocation at a time, logical
variables, uniform and storage buffers at fixed addresses, physical storage
buffer pointers, 32-bit floats and 32/64-bit integers, under each denormal
behavior a host may have. Anything outside that subset panics, so a test
never passes by skipping an operation.

It runs the known-answer tests (`gpus/sm86/tests/kat.rs`) and GPU replay
(`relift gpu-replay`), which runs one dumped dispatch on the CPU in trace mode.
