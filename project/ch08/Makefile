# Makefile for tdb, a tiny debugger (GNU make).
#
#   make           build build/tdb and the demo targets in build/targets/
#   make test      build, then run the unit tests and the integration tests
#   make clean     remove build/

BUILD := build

# -D_GNU_SOURCE: a debugger is Linux-specific; ask glibc for everything.
# -MMD -MP: write build/*.d files listing the headers each .c includes.
CPPFLAGS += -D_GNU_SOURCE -MMD -MP
CFLAGS   ?= -O2 -g
CFLAGS   += -std=c11 -Wall -Wextra -pedantic -Werror

# GNU readline, if its header is there (Debian/Ubuntu: libreadline-dev):
# line editing and history at the (tdb) prompt. "make READLINE=no" to
# build without it.
READLINE ?= $(if $(wildcard /usr/include/readline/readline.h),yes,no)
ifeq ($(READLINE),yes)
CPPFLAGS += -DTDB_READLINE
LDLIBS   += -lreadline
endif

# The programs we debug: no optimisation, frame pointers kept, debug info.
TARGET_CFLAGS := -std=c11 -g -O0 -fno-omit-frame-pointer -pthread

LIB_OBJS := $(addprefix $(BUILD)/, proc.o run.o breakpoint.o elf.o symbols.o regs.o mem.o \
                                  debugreg.o syscalls.o signals.o frames.o lines.o source.o \
                                  step.o cli.o)
TDB_OBJS := $(BUILD)/tdb.o $(BUILD)/input.o $(LIB_OBJS)
TEST_OBJS := $(addprefix $(BUILD)/, unit.o test_symbols.o test_regs.o test_mem.o \
                                   test_debugreg.o test_signals.o test_lines.o test_cli.o) $(LIB_OBJS)
TARGETS  := $(addprefix $(BUILD)/targets/, hello crash exit3 loop spin data watched files deep ticker waiter fact \
                                           workers deadlock forker sum)

.PHONY: all test clean

all: $(BUILD)/tdb $(TARGETS)

$(BUILD)/tdb: $(TDB_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD)/unit: $(TEST_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# The tests include headers from src/ and read the demo targets.
$(BUILD)/%.o: tests/%.c | $(BUILD)
	$(CC) $(CPPFLAGS) -Isrc -DTARGETS_DIR='"$(BUILD)/targets"' $(CFLAGS) -c -o $@ $<

$(BUILD)/targets/%: targets/%.c | $(BUILD)/targets
	$(CC) $(TARGET_CFLAGS) -o $@ $<

$(BUILD) $(BUILD)/targets:
	mkdir -p $@

test: all $(BUILD)/unit
	$(BUILD)/unit
	tests/itest.sh $(BUILD)

clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/*.d)
