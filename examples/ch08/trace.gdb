# trace.gdb - trace split()'s state machine on the line that starts "Acme.
#
#     gdb -q -batch -x trace.gdb --args ./csvsum orders.csv
#
# split() overwrites the line as it goes, so decide once, on entry.
break split
commands
  silent
  set $trace = line[1] == 'A'
  continue
end
break 24 if $trace
commands
  silent
  printf "%c  ", *p
  output st
  echo \n
  continue
end
run
