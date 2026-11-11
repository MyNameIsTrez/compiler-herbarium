local dir = arg[0]:match("^(.*)/[^/]*$") or "."
dofile(dir .. "/../../oracle.lua")

run(16, [[
r0 = 0
r2 = 7
*(u64 *)(r10 - 72) = r2
exit
]])
