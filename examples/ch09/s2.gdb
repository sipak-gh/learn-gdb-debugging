set environment GLIBC_TUNABLES=glibc.cpu.hwcaps=-AVX2,-AVX_Fast_Unaligned_Load
break 72
run plan.txt
delete 1
record
continue
bt
frame 3
print i
print cmds[i]
set can-use-hw-watchpoints 0
watch -l cmds[i].arg
reverse-continue
print space
print line
bt
info record
