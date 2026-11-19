# Chapter 7 examples: threads (bank, hits) and processes (linecount).
CFLAGS := -std=c11 -Wall -Wextra -g -O0 -pthread
PROGS  := bank hits linecount

all: $(PROGS)

clean:
	rm -f $(PROGS) core core.*

.PHONY: all clean
