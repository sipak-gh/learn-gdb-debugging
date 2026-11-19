/* sensor.c - read temperatures, one per line, keep the average of the last
 * five, and raise an alarm when a reading is above the alarm level (30).
 *
 *     ./sensor < readings.txt
 *
 * Bug: the alarm goes off for 24 degrees. Nothing assigns alarm_level after
 * the start, yet it changes. Find the write with a watchpoint.
 */
#include <stdio.h>

#define WINDOW 5

struct filter {
    int window[WINDOW];
    int alarm_level;
    int pos;
    int count;
};

/* The average of the readings in the window. */
static double average(const struct filter *f)
{
    int sum = 0;
    int n = f->count < WINDOW ? f->count : WINDOW;

    for (int i = 0; i < n; i++)
        sum += f->window[i];
    return n > 0 ? (double)sum / n : 0.0;
}

/* Put reading r into the window, replacing the oldest one. */
static void filter_add(struct filter *f, int r)
{
    f->window[f->pos] = r;
    f->pos = (f->pos + 1) % (WINDOW + 1);
    f->count++;
}

int main(void)
{
    struct filter f = { .alarm_level = 30 };
    int r;

    while (scanf("%d", &r) == 1) {
        filter_add(&f, r);
        if (r > f.alarm_level)
            printf("ALARM: %d is above %d\n", r, f.alarm_level);
        printf("reading %2d  average %.1f\n", r, average(&f));
    }
    return 0;
}
