/* test_regs.c - tests for regs.c. */
#include "regs.h"
#include "test.h"
#include "tests.h"

void test_reg_find(void)
{
    CHECK_STR(reg_find("rax")->name, "rax");
    CHECK_STR(reg_find("$rip")->name, "rip");
    CHECK_STR(reg_find("r15")->name, "r15");
    CHECK(reg_find("eax") == NULL);       /* only the full 64-bit registers */
    CHECK(reg_find("RAX") == NULL);
    CHECK(reg_find("") == NULL);
}

void test_reg_get_set(void)
{
    struct user_regs_struct regs = { 0 };

    regs.rip = 0x555555555149;
    CHECK_INT(reg_get(&regs, reg_find("rip")), 0x555555555149);
    reg_set(&regs, reg_find("rsp"), 0x7fffffffe5e8);
    CHECK_INT(regs.rsp, 0x7fffffffe5e8);
    CHECK_INT(regs.rbp, 0);               /* its neighbours are untouched */
    CHECK_INT(regs.r8, 0);
    for (int i = 0; i < reg_count; i++)   /* every entry finds itself */
        CHECK(reg_find(reg_table[i].name) == &reg_table[i]);
}

void test_reg_flags(void)
{
    char buf[80];

    CHECK_STR(reg_flags(0x246, buf, sizeof buf), "[ PF ZF IF ]");
    CHECK_STR(reg_flags(0x297, buf, sizeof buf), "[ CF PF AF SF IF ]");
    CHECK_STR(reg_flags(0x2, buf, sizeof buf), "[ ]");      /* bit 1 is always 1 */
    CHECK_STR(reg_flags(0x10346, buf, sizeof buf), "[ PF ZF TF IF RF ]");
}
