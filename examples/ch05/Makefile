# Chapter 5 examples, built for debugging.
CFLAGS := -std=c11 -Wall -Wextra -g -O0
PROGS  := ages greet progress queue scores scores-asan

all: $(PROGS)

scores-asan: scores.c
	$(CC) $(CFLAGS) -fsanitize=address -o $@ $<

clean:
	rm -f $(PROGS) core.*
