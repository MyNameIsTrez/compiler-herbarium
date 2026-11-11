local dir = arg[0]:match("^(.*)/[^/]*$") or "."
dofile(dir .. "/../../oracle.lua")

run(16, [[
r0 = 0
r2 = *(u8 *)(r1 + 0)
r2 -= 8
if r2 s< 0 goto +4
if r2 s> 7 goto +3
r4 = r1
r4 += r2
r0 = *(u8 *)(r4 + 0)
exit
]])
