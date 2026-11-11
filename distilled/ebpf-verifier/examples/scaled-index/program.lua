local dir = arg[0]:match("^(.*)/[^/]*$") or "."
dofile(dir .. "/../../oracle.lua")

run(16, [[
r0 = 0
r2 = *(u8 *)(r1 + 0)
r2 &= 3
r3 = 2
r3 *= r2
r5 = *(u8 *)(r1 + 1)
r5 &= 3
r5 |= 1
r5 *= r3
r4 = r1
r4 += r3
r0 = *(u8 *)(r4 + 0)
exit
]])
