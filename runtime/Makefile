# Builds the relift C runtime per layer and links the layers: core (no guest
# OS, no guest CPU), cpus/a64 (the ARM64 lifted-code ABI), guests/switch
# (Horizon) and guests/gb (the Game Boy memory map, over the core only). Each layer compiles with only its own include paths and those of
# the layers below it, so core cannot include a CPU or guest header. relift
# drives real builds later; this is for development and CI.
#
#   make                       clang, host target, into build/host
#   make test                  build and run every layer's tests
#   make charter               check that core names no guest and no CPU
#   make AUDIO=real BUILD=build/audio   also build runtime/host (SDL3 output and libopus decoding;
#                              needs pkg-config packages sdl3 and opus). The default is the null
#                              output and the stub decoder, with no extra dependency.
#   make VIDEO=sdl3 BUILD=build/video   also build the SDL3 window backend (runtime/host/window_sdl3.c;
#                              needs pkg-config package sdl3). The default has no window backend:
#                              runs are headless and Vulkan presents to headless surfaces.
#   make vk-present            run the synthetic Vulkan program (a window with VIDEO=sdl3 and a display)
#   make gdb-test              check the gdb frame filter (skips without gdb)
#   make sched-bench           run the device scheduler microbenchmark
#   make portable              only crash_test, fiber_tests and sched_bench: they link just the
#                              host-portable part of the core, so they also build for Windows
#                              (TARGET=x86_64-windows-gnu or aarch64-windows-gnu)
#   make CC="zig cc" AR="zig ar" TARGET=aarch64-macos BUILD=build/mac
#
# TARGET is passed as -target; leave it empty for the compiler's default.

ifeq ($(origin CC),default)
CC := clang
endif
AR ?= ar
PYTHON ?= python3
BUILD ?= build/host
TARGET ?=
OPT ?= -O2
AUDIO ?= null
VIDEO ?= none

TARGET_FLAGS := $(if $(TARGET),-target $(TARGET),)
MACHINE := $(if $(TARGET),$(TARGET),$(shell $(CC) -dumpmachine))

ifneq ($(findstring x86_64,$(MACHINE)),)
ARCH_FLAGS := -mcx16
endif
ifneq ($(findstring windows,$(MACHINE))$(findstring mingw,$(MACHINE)),)
WINDOWS := 1
EXE := .exe
endif
ifneq ($(or $(findstring macos,$(MACHINE)),$(findstring darwin,$(MACHINE)),$(findstring apple,$(MACHINE))),)
SHLIB_EXT := dylib
SHLIB_LDFLAGS := -dynamiclib -Wl,-install_name,@rpath/librelift_rt.dylib
else
SHLIB_EXT := so
SHLIB_LDFLAGS := -shared -Wl,--no-undefined -Wl,-soname,librelift_rt.so
endif
ifneq ($(findstring zig,$(CC)),)
ARCH_FLAGS += -fno-sanitize=undefined
endif

WARN := -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes -Wmissing-prototypes
CFLAGS_BASE := -std=c11 $(OPT) -g $(WARN) $(TARGET_FLAGS) $(ARCH_FLAGS) $(if $(WINDOWS),-DRL_STATIC_RUNTIME,-fPIC) \
	-fvisibility=hidden -fno-omit-frame-pointer -D_GNU_SOURCE -D_DARWIN_C_SOURCE -DRL_BUILDING_RUNTIME -DRL_CHECKS
LDLIBS := $(if $(WINDOWS),,-lpthread)
HOST_SRCS :=
ifeq ($(AUDIO),real)
HOST_SRCS += host/audio_sdl3.c host/opus_libopus.c
CFLAGS_BASE += -DRL_HOST_AUDIO $(shell pkg-config --cflags opus sdl3)
LDLIBS += $(shell pkg-config --libs opus sdl3)
endif
ifeq ($(VIDEO),sdl3)
HOST_SRCS += host/window_sdl3.c
CFLAGS_BASE += -DRL_HOST_VIDEO $(shell pkg-config --cflags sdl3)
LDLIBS += $(shell pkg-config --libs sdl3)
else ifneq ($(VIDEO),none)
$(error VIDEO must be none or sdl3)
endif

# Include paths per layer: a layer sees itself and the layers below it.
CORE_INC := -Icore/include -Icore
A64_INC := $(CORE_INC) -Icpus/a64/include -Icpus/a64
SWITCH_INC := $(A64_INC) -Iguests/switch
GB_INC := $(CORE_INC) -Iguests/gb
VK_INC := $(SWITCH_INC) -Iguests/switch/vk -Iguests/switch/vk/gen
RUNNER_INC := -Icore/tests

CORE_SRCS := $(wildcard core/*.c) $(HOST_SRCS)
CORE_ASM := $(wildcard core/*.S)
CORE_HDRS := $(wildcard core/include/relift/*.h core/*.h)
A64_SRCS := $(wildcard cpus/a64/*.c)
A64_HDRS := $(CORE_HDRS) $(wildcard cpus/a64/include/relift/a64/*.h cpus/a64/*.h)
SWITCH_SRCS := $(wildcard guests/switch/*.c)
SWITCH_HDRS := $(A64_HDRS) $(wildcard guests/switch/*.h)
GB_SRCS := $(wildcard guests/gb/*.c)
GB_HDRS := $(CORE_HDRS) $(wildcard guests/gb/*.h)
VK_SRCS := $(wildcard guests/switch/vk/*.c guests/switch/vk/gen/*.c)
VK_HDRS := $(SWITCH_HDRS) $(wildcard guests/switch/vk/*.h guests/switch/vk/gen/*.h)

FIBER_SRCS := core/fiber.c
FIBER_ASM := $(CORE_ASM)
SCHED_SRCS := core/sched.c

RUNNER_SRCS := core/tests/runner.c
RUNNER_HDRS := core/tests/runner.h
CORE_TEST_SRCS := $(filter-out $(RUNNER_SRCS),$(wildcard core/tests/*.c))
A64_TEST_SRCS := $(wildcard cpus/a64/tests/*.c)
SWITCH_TEST_SRCS := $(wildcard guests/switch/tests/*.c)
SWITCH_TEST_HDRS := $(wildcard guests/switch/tests/*.h)
GB_TEST_SRCS := $(wildcard guests/gb/tests/*.c)
GB_TEST_HDRS := $(wildcard guests/gb/tests/*.h)
VK_TEST_SRCS := $(wildcard guests/switch/vk/tests/*.c)
VK_DEMO_SRCS := guests/switch/vk/demo/vk_present.c

obj = $(patsubst %.S,$(BUILD)/obj/%.o,$(patsubst %.c,$(BUILD)/obj/%.o,$(1)))
CORE_ASM_OBJS := $(call obj,$(CORE_ASM))
CORE_OBJS := $(call obj,$(CORE_SRCS)) $(CORE_ASM_OBJS)
FIBER_OBJS := $(call obj,$(FIBER_SRCS) $(FIBER_ASM))
SCHED_OBJS := $(call obj,$(SCHED_SRCS)) $(FIBER_OBJS)
A64_OBJS := $(call obj,$(A64_SRCS))
SWITCH_OBJS := $(call obj,$(SWITCH_SRCS))
VK_OBJS := $(call obj,$(VK_SRCS))
GB_OBJS := $(call obj,$(GB_SRCS))
GB_TEST_OBJS := $(call obj,$(GB_TEST_SRCS))
RUNNER_OBJS := $(call obj,$(RUNNER_SRCS))
CORE_TEST_OBJS := $(call obj,$(CORE_TEST_SRCS))
A64_TEST_OBJS := $(call obj,$(A64_TEST_SRCS))
SWITCH_TEST_OBJS := $(call obj,$(SWITCH_TEST_SRCS))
VK_TEST_OBJS := $(call obj,$(VK_TEST_SRCS))
VK_DEMO_OBJS := $(call obj,$(VK_DEMO_SRCS))
TC_OBJS := $(call obj,guests/switch/tests/tc.c)
RT_OBJS := $(CORE_OBJS) $(A64_OBJS) $(SWITCH_OBJS) $(VK_OBJS)

CORE_LIB := $(BUILD)/librelift_core.a
A64_LIB := $(BUILD)/librelift_a64.a
SWITCH_LIB := $(BUILD)/librelift_switch.a
VK_LIB := $(BUILD)/librelift_switch_vk.a
GB_LIB := $(BUILD)/librelift_gb.a
STATIC := $(BUILD)/librelift_rt.a
SHARED := $(BUILD)/librelift_rt.$(SHLIB_EXT)
CORE_TESTS := $(BUILD)/core_tests
A64_TESTS := $(BUILD)/a64_tests
SWITCH_TESTS := $(BUILD)/switch_tests
VK_TESTS := $(BUILD)/switch_vk_tests
GB_TESTS := $(BUILD)/gb_tests
VK_PRESENT := $(BUILD)/vk_present
TESTS := $(CORE_TESTS) $(A64_TESTS) $(SWITCH_TESTS) $(VK_TESTS) $(GB_TESTS) $(VK_PRESENT)
CRASH_OBJS := $(call obj,core/crash.c core/crash_posix.c core/crash_win.c) $(FIBER_OBJS)
CRASH_TEST_OBJS := $(call obj,core/tests/crash/crash_test.c)
CRASH_TEST := $(BUILD)/crash_test$(EXE)
FIBER_TEST_OBJS := $(call obj,core/tests/fiber/fiber_tests.c)
FIBER_TESTS := $(BUILD)/fiber_tests$(EXE)
SCHED_BENCH_OBJS := $(call obj,core/tests/bench/sched_bench.c)
SCHED_BENCH := $(BUILD)/sched_bench$(EXE)
PORTABLE := $(CRASH_TEST) $(FIBER_TESTS) $(SCHED_BENCH)

.PHONY: all test portable test-portable charter gdb-test sched-bench vk-present clean
all: $(CORE_LIB) $(A64_LIB) $(SWITCH_LIB) $(VK_LIB) $(GB_LIB) $(STATIC) $(SHARED) $(TESTS) $(PORTABLE)
portable: $(PORTABLE)

$(filter-out $(CORE_ASM_OBJS),$(CORE_OBJS)): $(BUILD)/obj/%.o: %.c $(CORE_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_BASE) $(CORE_INC) $(CFLAGS) -c $< -o $@

$(CORE_ASM_OBJS): $(BUILD)/obj/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(TARGET_FLAGS) -g -c $< -o $@

$(A64_OBJS): $(BUILD)/obj/%.o: %.c $(A64_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_BASE) $(A64_INC) $(CFLAGS) -c $< -o $@

$(SWITCH_OBJS): $(BUILD)/obj/%.o: %.c $(SWITCH_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_BASE) $(SWITCH_INC) $(CFLAGS) -c $< -o $@

$(VK_OBJS): $(BUILD)/obj/%.o: %.c $(VK_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_BASE) $(VK_INC) $(CFLAGS) -c $< -o $@

$(GB_OBJS): $(BUILD)/obj/%.o: %.c $(GB_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_BASE) $(GB_INC) $(CFLAGS) -c $< -o $@

$(GB_TEST_OBJS): $(BUILD)/obj/%.o: %.c $(GB_HDRS) $(RUNNER_HDRS) $(GB_TEST_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_BASE) $(GB_INC) $(RUNNER_INC) $(CFLAGS) -Iguests/gb/tests -c $< -o $@

$(VK_TEST_OBJS): $(BUILD)/obj/%.o: %.c $(VK_HDRS) $(RUNNER_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_BASE) $(VK_INC) $(RUNNER_INC) $(CFLAGS) -c $< -o $@

$(VK_DEMO_OBJS): $(BUILD)/obj/%.o: %.c $(VK_HDRS) $(RUNNER_HDRS) $(SWITCH_TEST_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_BASE) $(VK_INC) $(RUNNER_INC) -Iguests/switch/tests $(CFLAGS) -c $< -o $@

$(RUNNER_OBJS) $(CORE_TEST_OBJS): $(BUILD)/obj/%.o: %.c $(CORE_HDRS) $(RUNNER_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_BASE) $(CORE_INC) $(RUNNER_INC) $(CFLAGS) -c $< -o $@

$(A64_TEST_OBJS): $(BUILD)/obj/%.o: %.c $(A64_HDRS) $(RUNNER_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_BASE) $(A64_INC) $(RUNNER_INC) $(CFLAGS) -c $< -o $@

$(SWITCH_TEST_OBJS): $(BUILD)/obj/%.o: %.c $(SWITCH_HDRS) $(RUNNER_HDRS) $(SWITCH_TEST_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_BASE) $(SWITCH_INC) $(RUNNER_INC) $(CFLAGS) -c $< -o $@

$(CORE_LIB): $(CORE_OBJS)
$(A64_LIB): $(A64_OBJS)
$(SWITCH_LIB): $(SWITCH_OBJS)
$(VK_LIB): $(VK_OBJS)
$(GB_LIB): $(GB_OBJS)
$(STATIC): $(RT_OBJS)
$(CORE_LIB) $(A64_LIB) $(SWITCH_LIB) $(VK_LIB) $(GB_LIB) $(STATIC):
	@rm -f $@
	$(AR) rcs $@ $^

$(SHARED): $(RT_OBJS)
	$(CC) $(TARGET_FLAGS) $(SHLIB_LDFLAGS) $^ -o $@ $(LDLIBS)

# Each layer's tests link that layer and the ones below it, nothing above.
$(CORE_TESTS): $(CORE_TEST_OBJS) $(RUNNER_OBJS) $(CORE_OBJS)
$(A64_TESTS): $(A64_TEST_OBJS) $(RUNNER_OBJS) $(CORE_OBJS) $(A64_OBJS)
$(SWITCH_TESTS): $(SWITCH_TEST_OBJS) $(RUNNER_OBJS) $(RT_OBJS)
$(VK_TESTS): $(VK_TEST_OBJS) $(RUNNER_OBJS) $(RT_OBJS)
$(GB_TESTS): $(GB_TEST_OBJS) $(RUNNER_OBJS) $(CORE_OBJS) $(GB_OBJS)
$(VK_PRESENT): $(VK_DEMO_OBJS) $(TC_OBJS) $(RT_OBJS)
$(TESTS):
	$(CC) $(TARGET_FLAGS) $^ -o $@ $(LDLIBS)

# The crash test links only the crash handler and the fibers, the fiber
# tests the fibers and the scheduler, so both also build on Windows; so does
# the scheduler microbenchmark, which links the whole core elsewhere.
$(CRASH_TEST_OBJS) $(FIBER_TEST_OBJS): $(BUILD)/obj/%.o: %.c $(CORE_HDRS) core/tests/child.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_BASE) -mno-omit-leaf-frame-pointer $(CORE_INC) $(RUNNER_INC) $(CFLAGS) -c $< -o $@

$(CRASH_TEST): $(CRASH_TEST_OBJS) $(CRASH_OBJS)
$(FIBER_TESTS): $(FIBER_TEST_OBJS) $(SCHED_OBJS) $(CRASH_OBJS)
$(CRASH_TEST) $(FIBER_TESTS):
	$(CC) $(TARGET_FLAGS) $(sort $^) -o $@ $(LDLIBS) $(if $(WINDOWS),,-lm)

$(SCHED_BENCH_OBJS): $(BUILD)/obj/%.o: %.c $(CORE_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_BASE) $(CORE_INC) $(CFLAGS) -c $< -o $@

$(SCHED_BENCH): $(SCHED_BENCH_OBJS) $(if $(WINDOWS),$(SCHED_OBJS),$(CORE_OBJS))
	$(CC) $(TARGET_FLAGS) $^ -o $@ $(LDLIBS)

sched-bench: $(SCHED_BENCH)
	$(abspath $(SCHED_BENCH))

test-portable: $(PORTABLE)
	$(abspath $(CRASH_TEST))
	$(abspath $(FIBER_TESTS))

test: export RELIFT_VIDEO := headless
test: $(TESTS) $(CRASH_TEST) $(FIBER_TESTS)
	$(abspath $(CRASH_TEST))
	$(abspath $(FIBER_TESTS))
	$(abspath $(CORE_TESTS))
	$(abspath $(A64_TESTS))
	$(abspath $(SWITCH_TESTS))
	$(abspath $(VK_TESTS))
	$(abspath $(GB_TESTS))
	$(MAKE) --no-print-directory vk-present

# The synthetic Vulkan program: a window with VIDEO=sdl3 and a display,
# headless surfaces otherwise (make test always runs headless, RELIFT_VIDEO=headless).
# Exit 77 means no host Vulkan (skipped).
vk-present: $(VK_PRESENT)
	@mkdir -p $(BUILD)/vk_present-sdmc
	RELIFT_SDMC=$(abspath $(BUILD))/vk_present-sdmc $(abspath $(VK_PRESENT)); rc=$$?; \
		if [ $$rc -eq 77 ]; then echo "vk_present skipped: no host Vulkan"; else exit $$rc; fi

charter:
	$(PYTHON) charter/check.py

gdb-test:
	CC="$(CC)" sh tools/gdb/test.sh $(BUILD)

clean:
	rm -rf $(BUILD)
