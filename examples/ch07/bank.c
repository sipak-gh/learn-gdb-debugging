/* bank.c - two tellers move money back and forth between two accounts at
 * the same time. Each account has a lock. Sometimes it never finishes.
 *
 *     ./bank
 */
#include <pthread.h>
#include <stdio.h>

struct account {
    int id;
    long balance;
    pthread_mutex_t lock;
};

struct teller {
    struct account *from, *to;
    int transfers;
};

struct account alice = { 1, 1000, PTHREAD_MUTEX_INITIALIZER };
struct account bob = { 2, 1000, PTHREAD_MUTEX_INITIALIZER };

/* Move amount from one account to the other, holding both locks so that
 * nobody sees the money in neither (or both) accounts. */
void transfer(struct account *from, struct account *to, long amount)
{
    pthread_mutex_lock(&from->lock);
    pthread_mutex_lock(&to->lock);
    from->balance -= amount;
    to->balance += amount;
    pthread_mutex_unlock(&to->lock);
    pthread_mutex_unlock(&from->lock);
}

void *teller(void *arg)
{
    struct teller *t = arg;
    for (int i = 0; i < t->transfers; i++)
        transfer(t->from, t->to, 1);
    return NULL;
}

int main(void)
{
    struct teller t1 = { &alice, &bob, 100000 };
    struct teller t2 = { &bob, &alice, 100000 };
    pthread_t a, b;

    pthread_create(&a, NULL, teller, &t1);
    pthread_create(&b, NULL, teller, &t2);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    printf("alice: %ld, bob: %ld, total: %ld\n", alice.balance, bob.balance,
           alice.balance + bob.balance);
    return 0;
}
