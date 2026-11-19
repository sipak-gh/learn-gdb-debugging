/* greet.c - print a greeting card for each name on the command line.
 *
 *     ./greet Ann Maximilian-Alexander
 *
 * Bug: a long name ends the program with "*** stack smashing detected ***".
 * The crash comes when format_card returns; find the write that did it.
 */
#include <stdio.h>
#include <string.h>

static void format_card(const char *name)
{
    char card[24];

    strcpy(card, "Dear ");
    strcat(card, name);
    strcat(card, ",");
    puts(card);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        format_card(argv[i]);
    return 0;
}
