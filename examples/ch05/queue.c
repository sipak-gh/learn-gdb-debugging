/* queue.c - a worker that drains a job queue, then waits for more.
 *
 *     ./queue &
 *
 * Bug: it prints "draining 7 jobs" and never "queue empty"; it uses a full
 * CPU core. Attach to it with gdb -p and see where it is stuck.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <sys/prctl.h>
#include <unistd.h>

#define QSIZE 16

/* A ring buffer: jobs[head] is the next job, jobs[tail] the next free slot. */
struct queue {
    int jobs[QSIZE];
    int head;
    int tail;
};

static int done;

static void run_job(int job)
{
    done += job;
}

/* Run jobs two at a time, as the old batch code did. */
static void drain(struct queue *q)
{
    while (q->head != q->tail) {
        run_job(q->jobs[q->head]);
        run_job(q->jobs[(q->head + 1) % QSIZE]);
        q->head = (q->head + 2) % QSIZE;
    }
}

int main(void)
{
    struct queue q = { { 5, 3, 8, 1, 9, 2, 7 }, 0, 7 };

    /* Let any process of ours attach a debugger (see ptrace_scope). */
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY);
    printf("queue: pid %d, draining %d jobs\n", (int)getpid(), q.tail - q.head);
    fflush(stdout);
    drain(&q);
    printf("queue empty, %d done\n", done);
    return 0;
}
