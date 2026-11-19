/* elf.h - what tdb needs from the program's ELF file. */
#ifndef TDB_ELF_H
#define TDB_ELF_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Where the kernel loaded the program: 0 for a fixed-address executable
 * (ET_EXEC), otherwise the distance between the addresses in the file and
 * the addresses in memory (a PIE, ET_DYN). Reads the ELF header of path and
 * the process's auxiliary vector. 0 on success, -1 on error. */
int elf_load_base(const char *path, pid_t pid, uint64_t *base);

/* Read the whole file into memory (free() it). NULL on error. */
unsigned char *elf_read_file(const char *path, size_t *size);

/* The contents of the section called name (".debug_line") in an ELF file
 * of size bytes read into memory: a pointer into file, its length in *len.
 * NULL if the file is not a 64-bit ELF file or has no such section. */
const unsigned char *elf_section(const unsigned char *file, size_t size, const char *name,
                                 size_t *len);

#endif
