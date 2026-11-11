local dir = arg[0]:match("^(.*)/[^/]*$") or "."
dofile(dir .. "/../../oracle.lua")

run(16, [[
r0 = 0
r2 = *(u8 *)(r1 + 0)
r2 &= 7
*(u64 *)(r10 - 8) = r2
r3 = *(u8 *)(r1 + 1)
r3 &= 3
*(u64 *)(r10 - 16) = r3
if r3 > 1 goto +1
r0 = 1
r5 = *(u64 *)(r10 - 8)
r4 = r1
r4 += r5
r0 = *(u8 *)(r4 + 0)
exit
]])
