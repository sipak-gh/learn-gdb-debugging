/* elf.h - what tdb needs from the program's ELF file. */
#ifndef TDB_ELF_H
#define TDB_ELF_H

#include <stdint.h>
#include <sys/types.h>

/* Where the kernel loaded the program: 0 for a fixed-address executable
 * (ET_EXEC), otherwise the distance between the addresses in the file and
 * the addresses in memory (a PIE, ET_DYN). Reads the ELF header of path and
 * the process's auxiliary vector. 0 on success, -1 on error. */
int elf_load_base(const char *path, pid_t pid, uint64_t *base);

#endif
