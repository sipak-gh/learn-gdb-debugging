/* tests.h - the list of every unit test, as an X-macro.
 *
 * To add a test: write void test_NAME(void) in a test_*.c file and add
 * X(NAME) here. unit.c expands this list into a table, and the line below
 * declares every test function, so a typo is a compile error.
 */
#ifndef TDB_TESTS_H
#define TDB_TESTS_H

#define TESTS(X)                    \
    X(sym_load)                     \
    X(sym_load_errors)              \
    X(sym_by_addr)                  \
    X(sym_parse)                    \
    X(sym_parse_errors)             \
    X(sym_describe)                 \
    X(reg_find)                     \
    X(reg_get_set)                  \
    X(reg_flags)                    \
    X(mem_format_line)              \
    X(mem_format_short_line)        \
    X(dr7_enable)                   \
    X(dr7_disable)                  \
    X(sys_names)

#define TEST_DECLARE(name) void test_##name(void);
TESTS(TEST_DECLARE)

#endif
