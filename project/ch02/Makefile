# Makefile for tdb, a tiny debugger (GNU make).
#
#   make           build build/tdb and the demo targets in build/targets/
#   make test      build, then run the tests
#   make clean     remove build/

BUILD := build

# -D_GNU_SOURCE: a debugger is Linux-specific; ask glibc for everything.
# -MMD -MP: write build/*.d files listing the headers each .c includes.
CPPFLAGS += -D_GNU_SOURCE -MMD -MP
CFLAGS   ?= -O2 -g
CFLAGS   += -std=c11 -Wall -Wextra -pedantic -Werror

# The programs we debug: no optimisation, frame pointers kept, debug info.
TARGET_CFLAGS := -std=c11 -g -O0 -fno-omit-frame-pointer

TDB_OBJS := $(addprefix $(BUILD)/, tdb.o proc.o run.o breakpoint.o elf.o)
TARGETS  := $(addprefix $(BUILD)/targets/, hello crash exit3 loop spin)

.PHONY: all test clean

all: $(BUILD)/tdb $(TARGETS)

$(BUILD)/tdb: $(TDB_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD)/targets/%: targets/%.c | $(BUILD)/targets
	$(CC) $(TARGET_CFLAGS) -o $@ $<

$(BUILD) $(BUILD)/targets:
	mkdir -p $@

test: all
	tests/itest.sh $(BUILD)

clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/*.d)
