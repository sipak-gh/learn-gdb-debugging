# Chapter 8 examples: automating gdb (inventory with a pretty-printer, csvsum).
CFLAGS := -std=c11 -Wall -Wextra -g -O0
PROGS  := inventory csvsum

all: $(PROGS)

clean:
	rm -f $(PROGS) core core.* trace.txt gdb.txt

.PHONY: all clean
