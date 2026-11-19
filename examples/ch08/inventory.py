# inventory.py - gdb helpers for inventory.c. Load them in gdb with
#
#     (gdb) source inventory.py
#
# print shows a struct table as its items, check-table looks for items in
# the wrong bucket, and $misplaced(TABLE) counts them (for conditions).
import gdb
import gdb.printing


def fnv1a(name):
    """The same hash as hash() in inventory.c, on a Python bytes object."""
    h = 14695981039346656037
    for byte in name:
        h ^= byte
        h = (h * 1099511628211) % 2**64
    return h


def items(table):
    """Yield (bucket number, struct item value) for each item in a table."""
    buckets = table["buckets"]
    for i in range(int(table["nbuckets"])):
        it = buckets[i]
        while it != 0:
            yield i, it.dereference()
            it = it["next"]


def misplaced(table):
    """The items of a table that are not in the bucket their hash says."""
    n = int(table["nbuckets"])
    wrong = []
    for i, item in items(table):
        name = item["name"].string()
        want = fnv1a(name.encode()) % n
        if want != i:
            wrong.append((name, i, want))
    return wrong


class ItemPrinter:
    """struct item: its name and quantity (not the next pointer)."""

    def __init__(self, val):
        self.val = val

    def to_string(self):
        return "item \"%s\", qty %d" % (self.val["name"].string(), int(self.val["qty"]))


class TablePrinter:
    """struct table: a map from item names to quantities."""

    def __init__(self, val):
        self.val = val

    def to_string(self):
        return "table of %d items in %d buckets" % (int(self.val["count"]),
                                                    int(self.val["nbuckets"]))

    def children(self):
        for _, item in items(self.val):
            yield "name", item["name"].string()
            yield "qty", item["qty"]

    def display_hint(self):
        return "map"


def build_printer():
    pp = gdb.printing.RegexpCollectionPrettyPrinter("inventory")
    pp.add_printer("item", "^item$", ItemPrinter)
    pp.add_printer("table", "^table$", TablePrinter)
    return pp


class CheckTable(gdb.Command):
    """check-table EXPR: list the items of the struct table EXPR that are
not in the bucket their hash says (table_get cannot find those)."""

    def __init__(self):
        super().__init__("check-table", gdb.COMMAND_DATA, gdb.COMPLETE_EXPRESSION)

    def invoke(self, arg, from_tty):
        if not arg:
            raise gdb.GdbError("Usage: check-table EXPR")
        table = gdb.parse_and_eval(arg)
        if table.type.code == gdb.TYPE_CODE_PTR:
            table = table.dereference()
        wrong = misplaced(table)
        for name, i, want in wrong:
            print("%-8s in bucket %2d, belongs in bucket %2d" % (name, i, want))
        count = sum(1 for _ in items(table))
        print("%d items, %d in the wrong bucket" % (count, len(wrong)))


class Misplaced(gdb.Function):
    """$misplaced(TABLE): how many items of TABLE are in the wrong bucket."""

    def __init__(self):
        super().__init__("misplaced")

    def invoke(self, table):
        if table.type.code == gdb.TYPE_CODE_PTR:
            table = table.dereference()
        return len(misplaced(table))


gdb.printing.register_pretty_printer(gdb.current_objfile(), build_printer(), replace=True)
CheckTable()
Misplaced()
