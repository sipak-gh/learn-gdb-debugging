break 65
run deposits.txt
delete 1
record
break 72
continue
print accounts[4]
print sizeof(struct account)
set can-use-hw-watchpoints 0
watch -l accounts[4].balance
reverse-continue
reverse-continue
reverse-continue
bt
print i
print name
print *a
reverse-finish
info record
continue
