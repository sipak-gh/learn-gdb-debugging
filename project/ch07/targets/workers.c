/* workers: three threads each add their share to a total. A target for
 * the thread commands: break in work(), info threads, thread N. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>

#define NWORKERS 3

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long total;

static void work(int id)
{
    long share = 100 * id;

    pthread_mutex_lock(&lock);
    total += share;
    pthread_mutex_unlock(&lock);
}

static void *worker(void *arg)
{
    int id = (int)(long)arg;
    char name[16];

    snprintf(name, sizeof name, "worker-%d", id);
    pthread_setname_np(pthread_self(), name);
    work(id);
    return NULL;
}

int main(void)
{
    pthread_t threads[NWORKERS];

    for (long i = 0; i < NWORKERS; i++)
        pthread_create(&threads[i], NULL, worker, (void *)(i + 1));
    for (int i = 0; i < NWORKERS; i++)
        pthread_join(threads[i], NULL);
    printf("total: %ld\n", total);
    return 0;
}
