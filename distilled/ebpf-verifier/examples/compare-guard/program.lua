local dir = arg[0]:match("^(.*)/[^/]*$") or "."
dofile(dir .. "/../../oracle.lua")

run(16, [[
r0 = 0
r2 = *(u8 *)(r1 + 0)
if r2 > 15 goto +4
r4 = r1
r4 += r2
r0 = *(u8 *)(r4 + 0)
exit
exit
]])
