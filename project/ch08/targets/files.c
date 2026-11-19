/* files.c - opens a file that is not there, then writes a message; a
 * target for catching system calls. */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    const char msg[] = "no settings, using defaults\n";
    int fd = open("settings.conf", O_RDONLY);

    if (fd < 0)
        write(1, msg, strlen(msg));
    else
        close(fd);
    return 0;
}
