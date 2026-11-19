/* ledger.c - read deposits ("NAME AMOUNT" lines), keep a balance per
 * account, and print the accounts. The money in the accounts should add
 * up to the money deposited. It does not.
 *
 *     ./ledger deposits.txt
 */
#include <stdio.h>

#define MAX_ACCOUNTS 8

struct account {
    long balance;
    char name[8];
};

struct account accounts[MAX_ACCOUNTS];
int naccounts;

/* Copy a name into an account. Names are short. */
void set_name(struct account *a, const char *name)
{
    int i;

    for (i = 0; name[i] != '\0'; i++)
        a->name[i] = name[i];
    a->name[i] = '\0';
}

int same_name(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* The account called name; a new one if there is none yet. */
struct account *find(const char *name)
{
    for (int i = 0; i < naccounts; i++)
        if (same_name(accounts[i].name, name))
            return &accounts[i];
    if (naccounts == MAX_ACCOUNTS)
        return NULL;
    struct account *a = &accounts[naccounts++];
    set_name(a, name);
    return a;
}

int main(int argc, char *argv[])
{
    char name[64];
    long amount, deposited = 0, held = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: ledger FILE\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    if (f == NULL) {
        perror(argv[1]);
        return 1;
    }
    while (fscanf(f, "%63s %ld", name, &amount) == 2) {
        struct account *a = find(name);
        if (a != NULL) {
            a->balance += amount;
            deposited += amount;
        }
    }
    fclose(f);
    for (int i = 0; i < naccounts; i++) {
        printf("%-8.8s %6ld\n", accounts[i].name, accounts[i].balance);
        held += accounts[i].balance;
    }
    printf("deposited %ld, held %ld\n", deposited, held);
    return held == deposited ? 0 : 1;
}
