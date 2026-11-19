/* inventory.c - load a stock list into a hash table, then look every item
 * up again. With a short list all is well; with a longer one some items
 * cannot be found.
 *
 *     ./inventory stock.txt
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct item {
    char *name;
    int qty;
    struct item *next;          /* the next item in the same bucket */
};

struct table {
    struct item **buckets;
    size_t nbuckets;
    size_t count;
};

/* FNV-1a: a simple, good string hash. */
unsigned long hash(const char *s)
{
    unsigned long h = 14695981039346656037UL;

    for (; *s != '\0'; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211UL;
    }
    return h;
}

/* Twice as many buckets: every item moves to its bucket in the new array. */
void grow(struct table *t)
{
    size_t n = t->nbuckets * 2;
    struct item **b = calloc(n, sizeof *b);

    for (size_t i = 0; i < t->nbuckets; i++) {
        struct item *it = t->buckets[i];
        while (it != NULL) {
            struct item *next = it->next;
            size_t h = hash(it->name) % t->nbuckets;
            it->next = b[h];
            b[h] = it;
            it = next;
        }
    }
    free(t->buckets);
    t->buckets = b;
    t->nbuckets = n;
}

void table_put(struct table *t, const char *name, int qty)
{
    if (t->count >= 2 * t->nbuckets)
        grow(t);
    size_t h = hash(name) % t->nbuckets;
    struct item *it = malloc(sizeof *it);
    it->name = strdup(name);
    it->qty = qty;
    it->next = t->buckets[h];
    t->buckets[h] = it;
    t->count++;
}

struct item *table_get(const struct table *t, const char *name)
{
    size_t h = hash(name) % t->nbuckets;

    for (struct item *it = t->buckets[h]; it != NULL; it = it->next)
        if (strcmp(it->name, name) == 0)
            return it;
    return NULL;
}

int main(int argc, char *argv[])
{
    struct table stock = { calloc(4, sizeof(struct item *)), 4, 0 };
    char name[64];
    int qty, missing = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: inventory FILE\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    if (f == NULL) {
        perror(argv[1]);
        return 1;
    }
    while (fscanf(f, "%63s %d", name, &qty) == 2)
        table_put(&stock, name, qty);
    rewind(f);
    while (fscanf(f, "%63s %d", name, &qty) == 2) {
        if (table_get(&stock, name) == NULL) {
            printf("not found: %s\n", name);
            missing++;
        }
    }
    fclose(f);
    printf("%zu items, %d not found\n", stock.count, missing);
    return missing ? 1 : 0;
}
