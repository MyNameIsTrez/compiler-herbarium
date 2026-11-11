local dir = arg[0]:match("^(.*)/[^/]*$") or "."
dofile(dir .. "/../../oracle.lua")

run(16, [[
r0 = 0
r2 = *(u8 *)(r1 + 0)
r2 &= 7
r4 = r10
r4 += r2
*(u64 *)(r4 - 8) = r0
exit
]])
