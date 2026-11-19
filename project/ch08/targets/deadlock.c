/* deadlock: two threads take two locks in opposite orders and wait for
 * each other for ever. Stop it with Ctrl-C and ask each thread where it
 * is, or attach to it (it allows that with prctl, as waiter does):
 *
 *     deadlock &
 *     tdb -p $!
 */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t a = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t b = PTHREAD_MUTEX_INITIALIZER;

static void take_both(pthread_mutex_t *first, pthread_mutex_t *second)
{
    struct timespec ten_ms = { 0, 10000000 };

    pthread_mutex_lock(first);
    nanosleep(&ten_ms, NULL);   /* give the other thread time to take its first lock */
    pthread_mutex_lock(second);
    pthread_mutex_unlock(second);
    pthread_mutex_unlock(first);
}

static void *one(void *arg)
{
    (void)arg;
    take_both(&a, &b);
    return NULL;
}

static void *two(void *arg)
{
    (void)arg;
    take_both(&b, &a);
    return NULL;
}

int main(void)
{
    pthread_t t1, t2;

    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    printf("deadlock: pid %d\n", (int)getpid());
    fflush(stdout);
    pthread_create(&t1, NULL, one, NULL);
    pthread_create(&t2, NULL, two, NULL);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    printf("done\n");
    return 0;
}
