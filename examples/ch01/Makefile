# Chapter 1 examples. Built for debugging: -g (debug info), -O0 (no
# optimisation). avg-O2 and avg-Og show what optimisation does to a debug
# session; avg-nodebug shows gdb without -g.
CFLAGS := -std=c11 -Wall -Wextra -g -O0
PROGS  := avg reverse avg-O2 avg-Og avg-nodebug

all: $(PROGS)

avg-O2: avg.c
	$(CC) -std=c11 -Wall -Wextra -g -O2 -o $@ $<
avg-Og: avg.c
	$(CC) -std=c11 -Wall -Wextra -g -Og -o $@ $<
avg-nodebug: avg.c
	$(CC) -std=c11 -Wall -Wextra -O0 -o $@ $<

clean:
	rm -f $(PROGS) core core.*

.PHONY: all clean
