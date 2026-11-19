/* roster.c - read "name score" lines, keep the students in a list sorted
 * by score, and print the list.
 *
 *     ./roster < class.txt
 *
 * Bug: one name comes out wrong. Find out why with gdb's print and x.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct student {
    char name[8];
    int score;
    struct student *next;
};

/* Insert s into the list (highest score first); return the new head. */
static struct student *insert(struct student *head, struct student *s)
{
    if (head == NULL || s->score > head->score) {
        s->next = head;
        return s;
    }
    struct student *p = head;
    while (p->next != NULL && p->next->score >= s->score)
        p = p->next;
    s->next = p->next;
    p->next = s;
    return head;
}

static struct student *new_student(const char *name, int score)
{
    struct student *s = malloc(sizeof *s);
    if (s == NULL) {
        perror("malloc");
        exit(1);
    }
    /* Names longer than the field are cut short. */
    strncpy(s->name, name, sizeof s->name);
    s->score = score;
    s->next = NULL;
    return s;
}

int main(void)
{
    struct student *head = NULL;
    char name[64];
    int score;

    while (scanf("%63s %d", name, &score) == 2)
        head = insert(head, new_student(name, score));

    int rank = 1;
    for (struct student *s = head; s != NULL; s = s->next)
        printf("%d. %-8s %3d\n", rank++, s->name, s->score);

    while (head != NULL) {
        struct student *next = head->next;
        free(head);
        head = next;
    }
    return 0;
}
