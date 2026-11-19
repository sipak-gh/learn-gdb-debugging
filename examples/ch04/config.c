/* config.c - read "key=value" settings from settings.conf; use defaults for
 * anything the file does not set.
 *
 *     ./config
 *
 * Bug: settings.conf says port=8080, but the program uses port 80 and says
 * nothing. Find out which file it really opens with catch syscall.
 */
#include <stdio.h>
#include <string.h>

struct config {
    int port;
    int workers;
};

static void load(struct config *c, const char *path)
{
    FILE *fp = fopen(path, "r");
    char line[128];

    if (fp == NULL)
        return;                 /* no file: keep the defaults */
    while (fgets(line, sizeof line, fp) != NULL) {
        sscanf(line, "port=%d", &c->port);
        sscanf(line, "workers=%d", &c->workers);
    }
    fclose(fp);
}

int main(void)
{
    struct config c = { .port = 80, .workers = 1 };

    load(&c, "setings.conf");
    printf("port %d, %d worker%s\n", c.port, c.workers, c.workers == 1 ? "" : "s");
    return 0;
}
