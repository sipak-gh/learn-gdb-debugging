/* ages.c - read "name,age" lines and print the average age.
 *
 *     ./ages people.txt
 *
 * Bug: it crashes with a segmentation fault, inside the C library.
 * Find which line of people.txt it was reading, and why.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct person {
    char name[32];
    int age;
};

/* Split "name,age" into p. */
static void parse_line(char *line, struct person *p)
{
    char *name = strtok(line, ",\n");
    char *age = strtok(NULL, ",\n");

    snprintf(p->name, sizeof p->name, "%s", name);
    p->age = atoi(age);
}

int main(int argc, char **argv)
{
    char line[128];
    struct person p;
    int total = 0, count = 0;
    FILE *f;

    if (argc != 2) {
        fprintf(stderr, "usage: %s FILE\n", argv[0]);
        return 2;
    }
    f = fopen(argv[1], "r");
    if (f == NULL) {
        perror(argv[1]);
        return 1;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        parse_line(line, &p);
        total += p.age;
        count++;
    }
    fclose(f);
    printf("%d people, average age %.1f\n", count, (double)total / count);
    return 0;
}
