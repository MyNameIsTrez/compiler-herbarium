local dir = arg[0]:match("^(.*)/[^/]*$") or "."
dofile(dir .. "/../../oracle.lua")

run(16, [[
r0 = 0
r2 = *(u8 *)(r1 + 0)
if r2 > 7 goto +3
r3 = 1
*(u64 *)(r10 - 8) = r3
goto +1
r3 = 2
exit
]])
