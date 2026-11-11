local dir = arg[0]:match("^(.*)/[^/]*$") or "."
dofile(dir .. "/../../oracle.lua")

run(16, [[
r0 = 0
r2 = 0
r3 = 0
r4 = r1
r4 += r2
*(u8 *)(r4 + 0) = r3
r2 += 1
if r2 != 16 goto -5
exit
]])
