/* test_lines.c - tests for lines.c, on the demo target deep. Addresses
 * change with the compiler; these tests use the symbol table to find them
 * and check the lines, which come from deep.c. */
#include <string.h>

#include "lines.h"
#include "symbols.h"
#include "test.h"
#include "tests.h"

#ifndef TARGETS_DIR
#define TARGETS_DIR "build/targets"
#endif

static const struct symbol *load(const char *name)
{
    CHECK(sym_load(TARGETS_DIR "/deep") > 0);
    CHECK(lines_load(TARGETS_DIR "/deep") > 0);
    return sym_by_name(name);
}

void test_line_at(void)
{
    const struct symbol *inner = load("inner"), *m = sym_by_name("main");

    /* inner's first instruction is the line of its "{". */
    CHECK(line_at(inner->addr) != NULL);
    CHECK_INT(line_at(inner->addr)->line, 11);
    CHECK_STR(line_file_name(line_at(inner->addr)->file), "targets/deep.c");
    CHECK(line_is_start(inner->addr));
    CHECK(!line_is_start(inner->addr + 1));
    CHECK_INT(line_at(inner->addr + 1)->line, 11);
    /* The last byte of main is still line 31; the byte after it is no line. */
    CHECK_INT(line_at(m->addr + m->size - 1)->line, 31);
    CHECK(line_at(m->addr + m->size) == NULL);
    CHECK(line_at(0) == NULL);

    const char *path = line_file_path(line_at(inner->addr)->file);
    CHECK(path[0] == '/');
    CHECK(strlen(path) > 15 && strcmp(path + strlen(path) - 15, "/targets/deep.c") == 0);
    uint64_t addr = inner->addr;        /* inner points into the table */
    lines_free();
    sym_free();
    CHECK(line_at(addr) == NULL);
}

void test_line_find(void)
{
    const struct symbol *inner = load("inner"), *outer = sym_by_name("outer");
    uint64_t addr;
    int line;

    CHECK_INT(line_find("deep.c", 11, &addr, &line), 0);
    CHECK_INT(addr, inner->addr);
    CHECK_INT(line, 11);
    /* Line 19 is empty: the next line with code is outer's "{". */
    CHECK_INT(line_find("targets/deep.c", 19, &addr, &line), 0);
    CHECK_INT(line, 21);
    CHECK_INT(addr, outer->addr);
    /* The name must match whole path components. */
    CHECK_INT(line_find("eep.c", 11, &addr, &line), -1);
    CHECK_INT(line_find("deep.c", 99, &addr, &line), -1);
    CHECK_INT(line_find("nosuch.c", 1, &addr, &line), -1);
    lines_free();
    sym_free();
}

void test_line_after_prologue(void)
{
    const struct symbol *inner = load("inner"), *m = sym_by_name("main");

    uint64_t body = line_after_prologue(inner->addr, inner->size);
    CHECK(body > inner->addr && body < inner->addr + inner->size);
    CHECK_INT(line_at(body)->line, 12);
    CHECK_INT(line_at(line_after_prologue(m->addr, m->size))->line, 27);
    /* No second statement within 1 byte: the function's own address. */
    CHECK_INT(line_after_prologue(inner->addr, 1), inner->addr);
    lines_free();
    sym_free();
}

void test_lines_load_errors(void)
{
    CHECK_INT(lines_load("Makefile"), 0);               /* not ELF: no line table */
    CHECK_INT(lines_load(TARGETS_DIR "/nosuch"), -1);
    CHECK(line_at(0x1000) == NULL);
}
