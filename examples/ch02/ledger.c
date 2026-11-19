/* ledger.c - replay a day of account transactions and print the balance.
 *
 * The bank says the final balance is 1756312.29; this program prints more.
 * Somewhere among the 1000 transactions it goes wrong.
 */
#include <stdio.h>

#define NTXN 1000

struct txn {
    int id;
    char kind;       /* 'D' deposit, 'W' withdrawal, 'F' fee in basis points */
    int amount;      /* cents, or basis points (1/100 of a percent) for 'F' */
};

static struct txn txns[NTXN];

/* Make a reproducible list of transactions (a tiny LCG). */
static void generate(void)
{
    unsigned seed = 12345;

    for (int i = 0; i < NTXN; i++) {
        seed = seed * 1103515245 + 12345;
        unsigned r = (seed >> 16) & 0x7fff;
        txns[i].id = i + 1;
        if (r % 10 == 0) {
            txns[i].kind = 'F';
            txns[i].amount = 1 + r % 25;
        } else if (r % 3 == 0) {
            txns[i].kind = 'W';
            txns[i].amount = (int)(r % 5000) * 100;
        } else {
            txns[i].kind = 'D';
            txns[i].amount = (int)(r % 9000) * 100;
        }
    }
}

static int fee(int balance, int basis_points)
{
    return balance * basis_points / 10000;
}

static int apply(int balance, const struct txn *t)
{
    switch (t->kind) {
    case 'D':
        return balance + t->amount;
    case 'W':
        if (t->amount > balance)
            return balance;            /* refused: not enough money */
        return balance - t->amount;
    case 'F':
        return balance - fee(balance, t->amount);
    }
    return balance;
}

int main(void)
{
    int balance = 0;

    generate();
    for (int i = 0; i < NTXN; i++)
        balance = apply(balance, &txns[i]);
    printf("final balance: %d.%02d\n", balance / 100, balance % 100);
    return 0;
}
