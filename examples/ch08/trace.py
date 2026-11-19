# trace.py - print each change of state in csvsum's split(), for every
# line, without stopping:
#
#     gdb -q -batch -x trace.py --args ./csvsum orders.csv
import gdb


class StateTrace(gdb.Breakpoint):
    """A breakpoint at the top of split()'s loop that never stops: stop()
    looks at the state and returns False, so the program goes on."""

    def __init__(self, location):
        super().__init__(location)
        self.silent = True
        self.last = None

    def stop(self):
        frame = gdb.selected_frame()
        st = str(frame.read_var("st"))
        p = frame.read_var("p")
        if self.last is None:
            print("line: %s" % p.string().rstrip("\n"))
        if st != self.last:
            # st is the state that will handle the character at p.
            print("  %-6s from %r" % (st, chr(p.dereference())))
        self.last = st
        return False


class LineStart(gdb.Breakpoint):
    """split() is called once per line: start a new trace."""

    def __init__(self, tracer):
        super().__init__("split")
        self.silent = True
        self.tracer = tracer

    def stop(self):
        self.tracer.last = None
        return False


LineStart(StateTrace("csvsum.c:24"))
gdb.execute("run")
