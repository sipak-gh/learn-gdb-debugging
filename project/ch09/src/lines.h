/* lines.h - the DWARF line table (.debug_line): which source line each
 * address of the program's code came from, and back. */
#ifndef TDB_LINES_H
#define TDB_LINES_H

#include <stdbool.h>
#include <stdint.h>

/* One row of the table: the code from addr up to the next row's address
 * came from line of file. */
struct line_row {
    uint64_t addr;       /* address in the file, as nm shows it */
    int file;            /* index into the file table: see line_file_name() */
    int line;            /* 1, 2, ... */
    bool is_stmt;        /* a good place to stop: the start of a statement */
    bool end_seq;        /* not code: the first address after a run of code */
};

/* Read the line table of the ELF file path (DWARF version 5, what gcc 11
 * writes for -g). Replaces any table loaded before. Returns the number of
 * rows, 0 if the file has no line table, or -1 (and says why) if it cannot
 * be read. */
int lines_load(const char *path);

/* Forget the table. */
void lines_free(void);

/* The row that covers the file address addr, or NULL if no source line
 * does (code compiled without -g, the PLT, libraries). */
const struct line_row *line_at(uint64_t addr);

/* Is addr the first address of a statement: a row with is_stmt starts
 * there? gdb shows "0x... in" before a location that is not. */
bool line_is_start(uint64_t addr);

/* A file's name as the compiler gave it ("targets/deep.c"), and the path
 * to open it with (the compiler's directory put in front if relative). */
const char *line_file_name(int file);
const char *line_file_path(int file);

/* The lowest address of the statements on line in the file whose name is
 * file or ends in "/file". If no code came from that line, the next line
 * that has some (gdb does the same). 0 with *addr and *found_line set, or
 * -1 if the file has no code at or after line. */
int line_find(const char *file, int line, uint64_t *addr, int *found_line);

/* Where the body of the function at the file address func (size bytes
 * long) starts, past the prologue that sets up its frame: the second
 * statement in the line table. func itself if there is no second. */
uint64_t line_after_prologue(uint64_t func, uint64_t size);

#endif
