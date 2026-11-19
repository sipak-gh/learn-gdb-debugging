/* test_symbols.c - tests for symbols.c, on the demo target data. */
#include "symbols.h"
#include "test.h"
#include "tests.h"

/* Set by the Makefile: where the demo targets are built. */
#ifndef TARGETS_DIR
#define TARGETS_DIR "build/targets"
#endif

static const struct symbol *load(const char *name)
{
    CHECK(sym_load(TARGETS_DIR "/data") > 0);
    return sym_by_name(name);
}

void test_sym_load(void)
{
    const struct symbol *s = load("counter");

    CHECK(s != NULL);
    CHECK_INT(s->kind, SYM_OBJECT);
    CHECK_INT(s->size, 4);                 /* an int */
    CHECK_INT(sym_by_name("big")->size, 8);
    CHECK_INT(sym_by_name("greeting")->size, 11);
    CHECK_INT(sym_by_name("main")->kind, SYM_FUNC);
    CHECK(sym_by_name("nosuch") == NULL);
    sym_free();
    CHECK(sym_by_name("main") == NULL);
}

void test_sym_load_errors(void)
{
    CHECK_INT(sym_load("Makefile"), -1);              /* not ELF */
    CHECK_INT(sym_load(TARGETS_DIR "/nosuch"), -1);
    CHECK(sym_by_name("main") == NULL);
}

void test_sym_by_addr(void)
{
    const struct symbol *bump = load("bump");

    CHECK(sym_by_addr(bump->addr) == bump);
    CHECK(sym_by_addr(bump->addr + 5) == bump);
    CHECK(sym_by_addr(bump->addr + bump->size - 1) == bump);
    CHECK(sym_by_addr(bump->addr + bump->size) != bump);
    CHECK(sym_by_addr(0) == NULL);
    CHECK(sym_by_addr(UINT64_MAX) == NULL);
    sym_free();
}

void test_sym_parse(void)
{
    const struct symbol *main_ = load("main");
    uint64_t addr;

    CHECK_INT(sym_parse("0x1149", 0x1000, &addr), 0);
    CHECK_INT(addr, 0x1149);                      /* numbers are not moved */
    CHECK_INT(sym_parse("4425", 0, &addr), 0);
    CHECK_INT(addr, 4425);
    CHECK_INT(sym_parse("main", 0, &addr), 0);
    CHECK_INT(addr, main_->addr);
    CHECK_INT(sym_parse("main", 0x555555554000, &addr), 0);
    CHECK_INT(addr, 0x555555554000 + main_->addr);
    CHECK_INT(sym_parse("main+4", 0, &addr), 0);
    CHECK_INT(addr, main_->addr + 4);
    CHECK_INT(sym_parse("main+0x10", 0, &addr), 0);
    CHECK_INT(addr, main_->addr + 16);
    sym_free();
}

void test_sym_parse_errors(void)
{
    uint64_t addr = 7;

    load("main");
    CHECK_INT(sym_parse("nosuch", 0, &addr), -1);
    CHECK_INT(sym_parse("main+", 0, &addr), -1);
    CHECK_INT(sym_parse("main+x", 0, &addr), -1);
    CHECK_INT(sym_parse("main+-1", 0, &addr), -1);
    CHECK_INT(sym_parse("0x11zz", 0, &addr), -1);
    CHECK_INT(sym_parse("", 0, &addr), -1);
    CHECK_INT(sym_parse(NULL, 0, &addr), -1);
    CHECK_INT(addr, 7);                           /* untouched on error */
    sym_free();
}

void test_sym_describe(void)
{
    const struct symbol *main_ = load("main");
    char buf[64];

    CHECK_STR(sym_describe(main_->addr, buf, sizeof buf), "main");
    CHECK_STR(sym_describe(main_->addr + 13, buf, sizeof buf), "main+13");
    CHECK(sym_describe(0, buf, sizeof buf) == NULL);
    CHECK_STR(sym_describe(main_->addr + 13, buf, 5), "main");     /* truncated */
    sym_free();
}
