/* source.h - showing lines of the program's source files. */
#ifndef TDB_SOURCE_H
#define TDB_SOURCE_H

/* Print lines first to last of the file at path as gdb does, "12\t...".
 * Lines past the end are left out. Returns the number of lines printed,
 * or -1 if the file cannot be opened. */
int src_print(const char *path, int first, int last);

/* The number of lines in the file, or -1 if it cannot be opened. */
int src_count(const char *path);

#endif
