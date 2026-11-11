local dir = arg[0]:match("^(.*)/[^/]*$") or "."
dofile(dir .. "/../../oracle.lua")

run(16, [[
r0 = 0
r2 = *(u8 *)(r1 + 0)
if r2 > 7 goto +2
r3 = 1
goto +1
r3 = 2
r0 = r3
exit
]])
