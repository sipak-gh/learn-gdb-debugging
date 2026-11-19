/* lines.c - the DWARF line table (.debug_line).
 *
 * The compiler does not store the table itself: that would be one row per
 * instruction. It stores a program for a small state machine whose
 * registers are an address, a file, a line and a few flags. Each opcode
 * changes the registers; some also append a row (the registers as they are
 * now) to the table. Running the program gives the table back.
 *
 * .debug_line holds one such unit per compiled .c file: a header (the
 * machine's parameters, the directory and file tables), then the program.
 * This reads DWARF version 5, the default of gcc 11. Names in its tables
 * are mostly offsets into another section, .debug_line_str.
 */
#include "lines.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "elf.h"

/* Numbers from the DWARF 5 standard, section 6.2 and 7.22. (elfutils has
 * them in <dwarf.h>; tdb needs only these.) */
enum {
    DW_LNS_copy = 1, DW_LNS_advance_pc, DW_LNS_advance_line, DW_LNS_set_file,
    DW_LNS_set_column, DW_LNS_negate_stmt, DW_LNS_set_basic_block, DW_LNS_const_add_pc,
    DW_LNS_fixed_advance_pc, DW_LNS_set_prologue_end, DW_LNS_set_epilogue_begin,
    DW_LNS_set_isa,
};
enum { DW_LNE_end_sequence = 1, DW_LNE_set_address, DW_LNE_define_file,
       DW_LNE_set_discriminator };
enum { DW_LNCT_path = 1, DW_LNCT_directory_index };
enum {
    DW_FORM_data2 = 0x05, DW_FORM_data4 = 0x06, DW_FORM_data8 = 0x07, DW_FORM_string = 0x08,
    DW_FORM_block = 0x09, DW_FORM_data1 = 0x0b, DW_FORM_strp = 0x0e, DW_FORM_udata = 0x0f,
    DW_FORM_data16 = 0x1e, DW_FORM_line_strp = 0x1f,
};

struct file {
    char *name;          /* as the compiler wrote it: "targets/deep.c" */
    char *path;          /* to open: "/home/you/.../targets/deep.c" */
};

static struct line_row *rows;
static int nrows, rows_cap;
static struct file *files;
static int nfiles, files_cap;

void lines_free(void)
{
    for (int i = 0; i < nfiles; i++) {
        free(files[i].name);
        free(files[i].path);
    }
    free(files);
    free(rows);
    files = NULL;
    rows = NULL;
    nfiles = files_cap = nrows = rows_cap = 0;
}

/* Reading the section: a position that never goes past the end. Reading
 * too far sets bad and gives 0. */
struct cursor {
    const unsigned char *p, *end;
    bool bad;
};

static uint64_t fixed(struct cursor *c, int n)
{
    uint64_t v = 0;
    if (c->end - c->p < n) {
        c->bad = true;
        c->p = c->end;
        return 0;
    }
    for (int i = 0; i < n; i++)         /* little-endian */
        v |= (uint64_t)c->p[i] << (8 * i);
    c->p += n;
    return v;
}

/* LEB128: 7 bits per byte, low bits first; the top bit says "more". */
static uint64_t uleb(struct cursor *c)
{
    uint64_t v = 0;
    for (int shift = 0;; shift += 7) {
        uint64_t byte = fixed(c, 1);
        if (shift < 64)
            v |= (byte & 0x7f) << shift;
        if (!(byte & 0x80) || c->bad)
            return v;
    }
}

static int64_t sleb(struct cursor *c)
{
    uint64_t v = 0, byte;
    int shift = 0;
    do {
        byte = fixed(c, 1);
        if (shift < 64)
            v |= (byte & 0x7f) << shift;
        shift += 7;
    } while ((byte & 0x80) && !c->bad);
    if (shift < 64 && (byte & 0x40))    /* negative: extend the sign bit */
        v |= ~(uint64_t)0 << shift;
    return (int64_t)v;
}

static void skip(struct cursor *c, uint64_t n)
{
    if ((uint64_t)(c->end - c->p) < n) {
        c->bad = true;
        c->p = c->end;
    } else {
        c->p += n;
    }
}

/* The string at offset in a string section, or NULL. */
static const char *string_at(const unsigned char *sec, size_t len, uint64_t offset)
{
    if (sec == NULL || offset >= len || memchr(sec + offset, '\0', len - offset) == NULL)
        return NULL;
    return (const char *)sec + offset;
}

/* The sections a unit's strings may live in. */
struct strings {
    const unsigned char *line_str, *str;
    size_t line_str_len, str_len;
};

/* Read one attribute value of the given form: a string (*s) or a number
 * (*n). false if the form is one tdb does not know. */
static bool read_form(struct cursor *c, uint64_t form, int offset_size,
                      const struct strings *st, const char **s, uint64_t *n)
{
    *s = NULL;
    *n = 0;
    switch (form) {
    case DW_FORM_string:
        *s = (const char *)c->p;
        if (memchr(c->p, '\0', (size_t)(c->end - c->p)) == NULL)
            return false;
        c->p += strlen(*s) + 1;
        return true;
    case DW_FORM_line_strp:
        *s = string_at(st->line_str, st->line_str_len, fixed(c, offset_size));
        return *s != NULL;
    case DW_FORM_strp:
        *s = string_at(st->str, st->str_len, fixed(c, offset_size));
        return *s != NULL;
    case DW_FORM_udata: *n = uleb(c); return true;
    case DW_FORM_data1: *n = fixed(c, 1); return true;
    case DW_FORM_data2: *n = fixed(c, 2); return true;
    case DW_FORM_data4: *n = fixed(c, 4); return true;
    case DW_FORM_data8: *n = fixed(c, 8); return true;
    case DW_FORM_data16: skip(c, 16); return true;      /* an MD5 checksum */
    case DW_FORM_block: skip(c, uleb(c)); return true;
    default: return false;
    }
}

static char *joined(const char *dir, const char *name)
{
    size_t len = strlen(dir) + 1 + strlen(name) + 1;
    char *s = malloc(len);
    if (s != NULL)
        snprintf(s, len, "%s/%s", dir, name);
    return s;
}

/* Add a file named name in directory dirs[dir] (dirs[0] is the directory
 * the compiler ran in). Returns its index, or -1. */
static int add_file(const char *name, const char *const *dirs, uint64_t ndirs, uint64_t dir)
{
    if (nfiles == files_cap) {
        int cap = files_cap ? files_cap * 2 : 16;
        struct file *bigger = realloc(files, (size_t)cap * sizeof *files);
        if (bigger == NULL)
            return -1;
        files = bigger;
        files_cap = cap;
    }
    const char *d = dir < ndirs ? dirs[dir] : NULL;
    struct file *f = &files[nfiles];
    if (name[0] == '/' || d == NULL || dir == 0)
        f->name = strdup(name);
    else
        f->name = joined(d, name);
    if (f->name == NULL)
        return -1;
    if (f->name[0] == '/' || ndirs == 0)
        f->path = strdup(f->name);
    else
        f->path = joined(dirs[0], f->name);
    if (f->path == NULL) {
        free(f->name);
        return -1;
    }
    return nfiles++;
}

/* A directory or file table is a list of formats (what each entry holds,
 * in which form: "a path, as an offset into .debug_line_str"), then the
 * entries. */
struct table_reader {
    struct cursor *c;
    int offset_size;
    const struct strings *st;
};

/* Read the list of formats: up to 8 (content type, form) pairs. */
static bool read_entry_formats(struct table_reader *r, uint64_t formats[][2], int *count)
{
    *count = (int)fixed(r->c, 1);
    if (*count > 8)
        return false;
    for (int i = 0; i < *count; i++) {
        formats[i][0] = uleb(r->c);
        formats[i][1] = uleb(r->c);
    }
    return !r->c->bad;
}

/* Read one entry: *path and *dir get its DW_LNCT_path and
 * DW_LNCT_directory_index values. false if it cannot be read. */
static bool read_entry(struct table_reader *r, uint64_t formats[][2], int count,
                       const char **path, uint64_t *dir)
{
    *path = NULL;
    *dir = 0;
    for (int i = 0; i < count; i++) {
        const char *s;
        uint64_t n;
        if (!read_form(r->c, formats[i][1], r->offset_size, r->st, &s, &n))
            return false;
        if (formats[i][0] == DW_LNCT_path)
            *path = s;
        else if (formats[i][0] == DW_LNCT_directory_index)
            *dir = n;
    }
    return *path != NULL && !r->c->bad;
}

static bool add_row(const struct line_row *row)
{
    if (nrows == rows_cap) {
        int cap = rows_cap ? rows_cap * 2 : 256;
        struct line_row *bigger = realloc(rows, (size_t)cap * sizeof *rows);
        if (bigger == NULL)
            return false;
        rows = bigger;
        rows_cap = cap;
    }
    rows[nrows++] = *row;
    return true;
}

/* The header fields the line program needs. */
struct header {
    unsigned min_inst_length;
    bool default_is_stmt;
    int line_base;
    unsigned line_range;
    unsigned opcode_base;
    const unsigned char *opcode_lengths;   /* arguments of opcodes 1 .. opcode_base - 1 */
    int first_file;                        /* files[] index of this unit's file 0 */
    int nfiles;
};

/* Run the line program in c, appending rows. false on a bad program. */
static bool run_program(struct cursor *c, const struct header *h)
{
    struct line_row r;
    /* The registers' starting values (DWARF 5, 6.2.2). */
#define RESET() (r = (struct line_row){ .file = 1, .line = 1, .is_stmt = h->default_is_stmt })
    RESET();

    while (c->p < c->end && !c->bad) {
        unsigned op = (unsigned)fixed(c, 1);
        if (op >= h->opcode_base) {
            /* A special opcode: one byte that advances the address and the
             * line together, then appends a row. */
            unsigned adjusted = op - h->opcode_base;
            r.addr += (adjusted / h->line_range) * h->min_inst_length;
            r.line += h->line_base + (int)(adjusted % h->line_range);
            if (!add_row(&r))
                return false;
            continue;
        }
        switch (op) {
        case 0: {
            /* An extended opcode: its length, then the opcode itself. */
            uint64_t len = uleb(c);
            if (len == 0 || len > (uint64_t)(c->end - c->p))
                return false;
            const unsigned char *next = c->p + len;
            unsigned sub = (unsigned)fixed(c, 1);
            if (sub == DW_LNE_end_sequence) {
                r.end_seq = true;
                if (!add_row(&r))
                    return false;
                RESET();
            } else if (sub == DW_LNE_set_address) {
                r.addr = fixed(c, 8);
            }
            /* DW_LNE_set_discriminator and others: nothing we keep. */
            c->p = next;
            break;
        }
        case DW_LNS_copy:
            if (!add_row(&r))
                return false;
            break;
        case DW_LNS_advance_pc:
            r.addr += uleb(c) * h->min_inst_length;
            break;
        case DW_LNS_advance_line:
            r.line += (int)sleb(c);
            break;
        case DW_LNS_set_file:
            r.file = (int)uleb(c);
            break;
        case DW_LNS_set_column:
            uleb(c);
            break;
        case DW_LNS_negate_stmt:
            r.is_stmt = !r.is_stmt;
            break;
        case DW_LNS_const_add_pc:
            /* The address advance of special opcode 255. */
            r.addr += ((255 - h->opcode_base) / h->line_range) * h->min_inst_length;
            break;
        case DW_LNS_fixed_advance_pc:
            r.addr += fixed(c, 2);
            break;
        case DW_LNS_set_basic_block:
        case DW_LNS_set_prologue_end:
        case DW_LNS_set_epilogue_begin:
            break;
        default:
            /* DW_LNS_set_isa, or an opcode newer than tdb: the header says
             * how many LEB128 arguments to skip. */
            for (unsigned i = 0; i < h->opcode_lengths[op - 1]; i++)
                uleb(c);
            break;
        }
    }
#undef RESET
    return !c->bad;
}

/* Turn the unit-local file numbers of the rows added since first_row into
 * indexes into files[]. */
static void fix_files(int first_row, const struct header *h)
{
    for (int i = first_row; i < nrows; i++) {
        if (rows[i].file >= 0 && rows[i].file < h->nfiles)
            rows[i].file += h->first_file;
        else
            rows[i].file = -1;
    }
}

/* Read the unit at the start of c (whose end is the unit's end). 1 if it
 * was read, 0 if it is a version tdb does not read, -1 if it is broken. */
static int read_unit(struct cursor *c, int offset_size, const struct strings *st)
{
    struct header h = { 0 };
    int version = (int)fixed(c, 2);
    if (version != 5)
        return c->bad ? -1 : 0;
    if (fixed(c, 1) != 8)               /* address size */
        return -1;
    fixed(c, 1);                        /* segment selector size */
    uint64_t header_length = fixed(c, offset_size);
    if (c->bad || header_length > (uint64_t)(c->end - c->p))
        return -1;
    struct cursor program = { c->p + header_length, c->end, false };

    h.min_inst_length = (unsigned)fixed(c, 1);
    fixed(c, 1);                        /* maximum operations per instruction */
    h.default_is_stmt = fixed(c, 1) != 0;
    h.line_base = (int8_t)fixed(c, 1);
    h.line_range = (unsigned)fixed(c, 1);
    h.opcode_base = (unsigned)fixed(c, 1);
    if (c->bad || h.line_range == 0 || h.opcode_base == 0)
        return -1;
    h.opcode_lengths = c->p;
    skip(c, h.opcode_base - 1);

    struct table_reader tr = { c, offset_size, st };
    uint64_t formats[8][2];
    int nformats;
    const char *path;
    uint64_t dir;

    /* The directory table. Entry 0 is the directory the compiler ran in. */
    const char *dirs[64];
    if (!read_entry_formats(&tr, formats, &nformats))
        return -1;
    uint64_t ndirs = uleb(c);
    for (uint64_t i = 0; i < ndirs; i++) {
        if (!read_entry(&tr, formats, nformats, &path, &dir))
            return -1;
        if (i < 64)
            dirs[i] = path;
    }
    if (ndirs > 64)
        ndirs = 64;

    /* The file table. The line program's file register indexes it. */
    if (!read_entry_formats(&tr, formats, &nformats))
        return -1;
    uint64_t count = uleb(c);
    h.first_file = nfiles;
    for (uint64_t i = 0; i < count; i++) {
        if (!read_entry(&tr, formats, nformats, &path, &dir) ||
            add_file(path, dirs, ndirs, dir) < 0)
            return -1;
        h.nfiles++;
    }
    if (c->bad)
        return -1;

    int first_row = nrows;
    if (!run_program(&program, &h))
        return -1;
    fix_files(first_row, &h);
    return 1;
}

int lines_load(const char *path)
{
    size_t size, len, line_str_len = 0, str_len = 0;
    unsigned char *file = elf_read_file(path, &size);

    lines_free();
    if (file == NULL)
        return -1;
    const unsigned char *sec = elf_section(file, size, ".debug_line", &len);
    if (sec == NULL) {
        free(file);
        return 0;
    }
    struct strings st;
    st.line_str = elf_section(file, size, ".debug_line_str", &line_str_len);
    st.str = elf_section(file, size, ".debug_str", &str_len);
    st.line_str_len = line_str_len;
    st.str_len = str_len;

    struct cursor c = { sec, sec + len, false };
    int skipped = 0, broken = 0;
    while (c.p < c.end) {
        /* unit_length: 4 bytes, or 0xffffffff and then 8 (64-bit DWARF,
         * whose section offsets are 8 bytes too). */
        int offset_size = 4;
        uint64_t unit_length = fixed(&c, 4);
        if (unit_length == 0xffffffff) {
            unit_length = fixed(&c, 8);
            offset_size = 8;
        }
        if (c.bad || unit_length > (uint64_t)(c.end - c.p)) {
            broken++;
            break;
        }
        struct cursor unit = { c.p, c.p + unit_length, false };
        int r = read_unit(&unit, offset_size, &st);
        if (r == 0)
            skipped++;
        else if (r < 0)
            broken++;
        c.p += unit_length;
    }
    free(file);
    if (skipped > 0)
        printf("Skipped %d line table unit%s that %s not DWARF 5 (compile with -gdwarf-5).\n",
               skipped, skipped == 1 ? "" : "s", skipped == 1 ? "is" : "are");
    if (broken > 0)
        printf("Could not read %d line table unit%s.\n", broken, broken == 1 ? "" : "s");
    return nrows;
}

const struct line_row *line_at(uint64_t addr)
{
    /* The rows come in sequences, each in address order and closed by an
     * end_seq row: the answer is the last row at or below addr in the
     * sequence whose end is above addr. */
    const struct line_row *best = NULL;
    for (int i = 0; i < nrows; i++) {
        if (rows[i].end_seq) {
            if (best != NULL && addr < rows[i].addr)
                return best;
            best = NULL;
        } else if (rows[i].addr <= addr) {
            best = &rows[i];
        }
    }
    return NULL;
}

bool line_is_start(uint64_t addr)
{
    for (int i = 0; i < nrows; i++)
        if (rows[i].addr == addr && rows[i].is_stmt && !rows[i].end_seq)
            return true;
    return false;
}

const char *line_file_name(int file)
{
    return file >= 0 && file < nfiles ? files[file].name : "??";
}

const char *line_file_path(int file)
{
    return file >= 0 && file < nfiles ? files[file].path : "??";
}

/* Is file called want, or does its path end in "/want"? */
static bool file_matches(int file, const char *want)
{
    if (file < 0 || file >= nfiles)
        return false;
    const char *path = files[file].path;
    size_t lp = strlen(path), lw = strlen(want);
    if (strcmp(files[file].name, want) == 0)
        return true;
    return lp >= lw && strcmp(path + lp - lw, want) == 0 && (lp == lw || path[lp - lw - 1] == '/');
}

int line_find(const char *file, int line, uint64_t *addr, int *found_line)
{
    int best_line = 0;
    /* The nearest line at or after line that has statements... */
    for (int i = 0; i < nrows; i++) {
        const struct line_row *r = &rows[i];
        if (!r->end_seq && r->is_stmt && r->line >= line && file_matches(r->file, file) &&
            (best_line == 0 || r->line < best_line))
            best_line = r->line;
    }
    if (best_line == 0)
        return -1;
    /* ...and the lowest address among them. */
    bool found = false;
    for (int i = 0; i < nrows; i++) {
        const struct line_row *r = &rows[i];
        if (!r->end_seq && r->is_stmt && r->line == best_line && file_matches(r->file, file) &&
            (!found || r->addr < *addr)) {
            *addr = r->addr;
            found = true;
        }
    }
    *found_line = best_line;
    return 0;
}

uint64_t line_after_prologue(uint64_t func, uint64_t size)
{
    uint64_t body = 0;
    for (int i = 0; i < nrows; i++) {
        const struct line_row *r = &rows[i];
        if (!r->end_seq && r->is_stmt && r->addr > func && r->addr < func + size &&
            (body == 0 || r->addr < body))
            body = r->addr;
    }
    return body ? body : func;
}
