local dir = arg[0]:match("^(.*)/[^/]*$") or "."
dofile(dir .. "/../../oracle.lua")

run(16, [[
r0 = 0
r2 = 0
r2 += 1
if r2 != 1000 goto -2
exit
]])
