# Builds the relift C runtime per layer and links the layers: core (no guest
# OS, no guest CPU), cpus/a64 (the ARM64 lifted-code ABI) and guests/switch
# (Horizon). Each layer compiles with only its own include paths and those of
# the layers below it, so core cannot include a CPU or guest header. relift
# drives real builds later; this is for development and CI.
#
#   make                       clang, host target, into build/host
#   make test                  build and run every layer's tests
#   make charter               check that core names no guest and no CPU
#   make AUDIO=real BUILD=build/audio   also build runtime/host (SDL3 output and libopus decoding;
#                              needs pkg-config packages sdl3 and opus). The default is the null
#                              output and the stub decoder, with no extra dependency.
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

TARGET_FLAGS := $(if $(TARGET),-target $(TARGET),)
MACHINE := $(if $(TARGET),$(TARGET),$(shell $(CC) -dumpmachine))

ifneq ($(findstring x86_64,$(MACHINE)),)
ARCH_FLAGS := -mcx16
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
CFLAGS_BASE := -std=c11 $(OPT) -g $(WARN) $(TARGET_FLAGS) $(ARCH_FLAGS) -fPIC -fvisibility=hidden \
	-fno-omit-frame-pointer -D_GNU_SOURCE -D_DARWIN_C_SOURCE -DRL_BUILDING_RUNTIME
LDLIBS := -lpthread
ifeq ($(AUDIO),real)
HOST_SRCS := $(wildcard host/*.c)
CFLAGS_BASE += -DRL_HOST_AUDIO $(shell pkg-config --cflags opus sdl3)
LDLIBS += $(shell pkg-config --libs opus sdl3)
endif

# Include paths per layer: a layer sees itself and the layers below it.
CORE_INC := -Icore/include -Icore
A64_INC := $(CORE_INC) -Icpus/a64/include -Icpus/a64
SWITCH_INC := $(A64_INC) -Iguests/switch
RUNNER_INC := -Icore/tests

CORE_SRCS := $(wildcard core/*.c) $(HOST_SRCS)
CORE_HDRS := $(wildcard core/include/relift/*.h core/*.h)
A64_SRCS := $(wildcard cpus/a64/*.c)
A64_HDRS := $(CORE_HDRS) $(wildcard cpus/a64/include/relift/a64/*.h cpus/a64/*.h)
SWITCH_SRCS := $(wildcard guests/switch/*.c)
SWITCH_HDRS := $(A64_HDRS) $(wildcard guests/switch/*.h)

RUNNER_SRCS := core/tests/runner.c
RUNNER_HDRS := core/tests/runner.h
CORE_TEST_SRCS := $(filter-out $(RUNNER_SRCS),$(wildcard core/tests/*.c))
A64_TEST_SRCS := $(wildcard cpus/a64/tests/*.c)
SWITCH_TEST_SRCS := $(wildcard guests/switch/tests/*.c)
SWITCH_TEST_HDRS := $(wildcard guests/switch/tests/*.h)

obj = $(patsubst %.c,$(BUILD)/obj/%.o,$(1))
CORE_OBJS := $(call obj,$(CORE_SRCS))
A64_OBJS := $(call obj,$(A64_SRCS))
SWITCH_OBJS := $(call obj,$(SWITCH_SRCS))
RUNNER_OBJS := $(call obj,$(RUNNER_SRCS))
CORE_TEST_OBJS := $(call obj,$(CORE_TEST_SRCS))
A64_TEST_OBJS := $(call obj,$(A64_TEST_SRCS))
SWITCH_TEST_OBJS := $(call obj,$(SWITCH_TEST_SRCS))
RT_OBJS := $(CORE_OBJS) $(A64_OBJS) $(SWITCH_OBJS)

CORE_LIB := $(BUILD)/librelift_core.a
A64_LIB := $(BUILD)/librelift_a64.a
SWITCH_LIB := $(BUILD)/librelift_switch.a
STATIC := $(BUILD)/librelift_rt.a
SHARED := $(BUILD)/librelift_rt.$(SHLIB_EXT)
CORE_TESTS := $(BUILD)/core_tests
A64_TESTS := $(BUILD)/a64_tests
SWITCH_TESTS := $(BUILD)/switch_tests
TESTS := $(CORE_TESTS) $(A64_TESTS) $(SWITCH_TESTS)

.PHONY: all test charter clean
all: $(CORE_LIB) $(A64_LIB) $(SWITCH_LIB) $(STATIC) $(SHARED) $(TESTS)

$(CORE_OBJS): $(BUILD)/obj/%.o: %.c $(CORE_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_BASE) $(CORE_INC) $(CFLAGS) -c $< -o $@

$(A64_OBJS): $(BUILD)/obj/%.o: %.c $(A64_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_BASE) $(A64_INC) $(CFLAGS) -c $< -o $@

$(SWITCH_OBJS): $(BUILD)/obj/%.o: %.c $(SWITCH_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_BASE) $(SWITCH_INC) $(CFLAGS) -c $< -o $@

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
$(STATIC): $(RT_OBJS)
$(CORE_LIB) $(A64_LIB) $(SWITCH_LIB) $(STATIC):
	@rm -f $@
	$(AR) rcs $@ $^

$(SHARED): $(RT_OBJS)
	$(CC) $(TARGET_FLAGS) $(SHLIB_LDFLAGS) $^ -o $@ $(LDLIBS)

# Each layer's tests link that layer and the ones below it, nothing above.
$(CORE_TESTS): $(CORE_TEST_OBJS) $(RUNNER_OBJS) $(CORE_OBJS)
$(A64_TESTS): $(A64_TEST_OBJS) $(RUNNER_OBJS) $(CORE_OBJS) $(A64_OBJS)
$(SWITCH_TESTS): $(SWITCH_TEST_OBJS) $(RUNNER_OBJS) $(RT_OBJS)
$(TESTS):
	$(CC) $(TARGET_FLAGS) $^ -o $@ $(LDLIBS)

test: $(TESTS)
	$(abspath $(CORE_TESTS))
	$(abspath $(A64_TESTS))
	$(abspath $(SWITCH_TESTS))

charter:
	$(PYTHON) charter/check.py

clean:
	rm -rf $(BUILD)
