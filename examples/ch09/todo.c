/* todo.c - run a to-do script. Each line of the file is a command:
 *
 *     add TEXT    add an item
 *     done N      mark item N as done
 *     list        print the items
 *
 * All commands are parsed first, then run, then echoed as a log.
 *
 *     ./todo plan.txt
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CMDS 32
#define MAX_ITEMS 16

struct cmd {
    char verb[8];
    const char *arg;
};

struct item {
    char text[40];
    int done;
};

static char lines[MAX_CMDS][64];
static struct cmd cmds[MAX_CMDS];
static int ncmds;
static struct item items[MAX_ITEMS];
static int nitems;

/* Split "verb rest" into c->verb and c->arg (the rest). */
static void parse(char *line, struct cmd *c)
{
    line[strcspn(line, "\n")] = '\0';
    char *space = strchr(line, ' ');
    c->arg = space + 1;
    if (space != NULL)
        *space = '\0';
    snprintf(c->verb, sizeof c->verb, "%s", line);
}

static void run(const struct cmd *c)
{
    if (strcmp(c->verb, "add") == 0 && nitems < MAX_ITEMS) {
        snprintf(items[nitems].text, sizeof items[nitems].text, "%s", c->arg);
        items[nitems++].done = 0;
    } else if (strcmp(c->verb, "done") == 0) {
        int n = atoi(c->arg);
        if (n >= 1 && n <= nitems)
            items[n - 1].done = 1;
    } else if (strcmp(c->verb, "list") == 0) {
        for (int i = 0; i < nitems; i++)
            printf("%2d [%c] %s\n", i + 1, items[i].done ? 'x' : ' ',
                   items[i].text);
    }
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "usage: todo FILE\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    if (f == NULL) {
        perror(argv[1]);
        return 1;
    }
    while (ncmds < MAX_CMDS && fgets(lines[ncmds], sizeof lines[0], f) != NULL) {
        parse(lines[ncmds], &cmds[ncmds]);
        ncmds++;
    }
    fclose(f);
    for (int i = 0; i < ncmds; i++)
        run(&cmds[i]);
    printf("log:\n");
    for (int i = 0; i < ncmds; i++)
        printf("  %s %s\n", cmds[i].verb, cmds[i].arg);
    return 0;
}
