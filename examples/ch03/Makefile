# Chapter 3 examples, built for debugging. stats.c draws a warning on
# purpose: read it. roster-nodebug is roster without -g: symbols only.
CFLAGS := -std=c11 -Wall -Wextra -g -O0
PROGS  := roster stats roster-nodebug

all: $(PROGS)

roster-nodebug: roster.c
	$(CC) -std=c11 -Wall -Wextra -O0 -o $@ $<

clean:
	rm -f $(PROGS)
