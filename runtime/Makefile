# Builds the relift C runtime: a static library, a shared library and the
# C unit tests. relift drives real builds later; this is for development and CI.
#
#   make                       clang, host target, into build/host
#   make test                  build and run the tests
#   make CC="zig cc" AR="zig ar" TARGET=aarch64-macos BUILD=build/mac
#
# TARGET is passed as -target; leave it empty for the compiler's default.

ifeq ($(origin CC),default)
CC := clang
endif
AR ?= ar
BUILD ?= build/host
TARGET ?=
OPT ?= -O2

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
CFLAGS_ALL := -std=c11 $(OPT) -g $(WARN) $(TARGET_FLAGS) $(ARCH_FLAGS) -fPIC -fvisibility=hidden \
	-fno-omit-frame-pointer -D_GNU_SOURCE -D_DARWIN_C_SOURCE -DRL_BUILDING_RUNTIME \
	-Iinclude -Icore -Iswitch $(CFLAGS)
LDLIBS := -lpthread

RT_SRCS := $(wildcard core/*.c) $(wildcard switch/*.c)
RT_HDRS := $(wildcard include/relift/*.h) $(wildcard core/*.h) $(wildcard switch/*.h)
RT_OBJS := $(patsubst %.c,$(BUILD)/obj/%.o,$(RT_SRCS))
TEST_SRCS := $(wildcard tests/*.c)
TEST_HDRS := $(wildcard tests/*.h)
TEST_OBJS := $(patsubst %.c,$(BUILD)/obj/%.o,$(TEST_SRCS))

STATIC := $(BUILD)/librelift_rt.a
SHARED := $(BUILD)/librelift_rt.$(SHLIB_EXT)
TESTS := $(BUILD)/rt_tests

.PHONY: all test clean
all: $(STATIC) $(SHARED) $(if $(TEST_SRCS),$(TESTS))

$(BUILD)/obj/%.o: %.c $(RT_HDRS) $(TEST_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_ALL) -c $< -o $@

$(STATIC): $(RT_OBJS)
	@rm -f $@
	$(AR) rcs $@ $^

$(SHARED): $(RT_OBJS)
	$(CC) $(TARGET_FLAGS) $(SHLIB_LDFLAGS) $^ -o $@ $(LDLIBS)

$(TESTS): $(TEST_OBJS) $(RT_OBJS)
	$(CC) $(TARGET_FLAGS) $^ -o $@ $(LDLIBS)

test: $(TESTS)
	./$(TESTS)

clean:
	rm -rf $(BUILD)
