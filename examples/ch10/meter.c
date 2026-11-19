/* meter.c - power use from an electricity meter's log.
 *
 *     ./meter sample.log
 *
 * Each line of the log is a reading: the time (seconds since midnight)
 * and the meter's counter (watt-hours). For each interval between two
 * readings meter prints the average power, in watts; then the peak.
 *
 * Bug: on the device (cd device; ./meter meter.log) it dies with
 * "Floating point exception". Here, with sample.log, it works. Debug it
 * where it fails: gdbserver there, gdb here.
 */
#include <stdio.h>
#include <stdlib.h>

#define MAXREADINGS 1000

struct reading {
    long time;      /* seconds since midnight */
    long wh;        /* the meter's counter, in watt-hours */
};

static struct reading readings[MAXREADINGS];

/* Read the log into readings[]. Returns how many there are. */
static int load(const char *path)
{
    FILE *f = fopen(path, "r");
    int n = 0;

    if (f == NULL) {
        perror(path);
        exit(1);
    }
    while (n < MAXREADINGS &&
           fscanf(f, "%ld %ld", &readings[n].time, &readings[n].wh) == 2)
        n++;
    fclose(f);
    return n;
}

/* The average power between two readings, in watts. */
static long watts(const struct reading *a, const struct reading *b)
{
    long wh = b->wh - a->wh;
    long seconds = b->time - a->time;

    return wh * 3600 / seconds;
}

int main(int argc, char *argv[])
{
    long peak = 0;
    int n;

    if (argc != 2) {
        fprintf(stderr, "usage: meter LOG\n");
        return 2;
    }
    n = load(argv[1]);
    for (int i = 1; i < n; i++) {
        long w = watts(&readings[i - 1], &readings[i]);

        printf("%5ld-%5ld s  %6ld W\n", readings[i - 1].time, readings[i].time, w);
        if (w > peak)
            peak = w;
    }
    printf("%d readings, peak %ld W\n", n, peak);
    return 0;
}
