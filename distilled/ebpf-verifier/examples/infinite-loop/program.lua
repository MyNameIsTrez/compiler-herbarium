local dir = arg[0]:match("^(.*)/[^/]*$") or "."
dofile(dir .. "/../../oracle.lua")

run(16, [[
r0 = 0
goto -1
]])
