local dir = arg[0]:match("^(.*)/[^/]*$") or "."
dofile(dir .. "/../../oracle.lua")

run(16, [[
r0 = 0
r2 = 5
r3 = 9
if r2 < r3 goto +2
r0 = *(u8 *)(r1 + 100)
exit
exit
]])
