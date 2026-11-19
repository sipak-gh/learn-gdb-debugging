# Chapter 6 examples. stop-O2 is stop.c built with -O2. report is built
# from two files, report.c and mean.c.
CFLAGS := -std=c11 -Wall -Wextra -g -O0
PROGS  := stop stop-O2 checked report

all: $(PROGS)

stop-O2: stop.c
	$(CC) -std=c11 -Wall -Wextra -g -O2 -o $@ $<
report: report.c mean.c
	$(CC) $(CFLAGS) -o $@ report.c mean.c

clean:
	rm -f $(PROGS) core core.*

.PHONY: all clean
