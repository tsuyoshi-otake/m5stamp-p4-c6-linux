# After: target extended-remote :3333
#        file <matching-vmlinux>
set pagination off
echo === JTAG halt registers ===\n
info registers pc ra sp mstatus mcause
echo === mstatus.MIE (bit 3) ===\n
p/x ($mstatus >> 3) & 1
echo === backtrace ===\n
bt
