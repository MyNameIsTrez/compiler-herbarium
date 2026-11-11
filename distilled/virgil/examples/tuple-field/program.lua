-- The same program in Lua.
local s = {a = {1, 2}, b = {4, 6}, origin = {0, 0}}

local function dist(o)
  local dx = o.b[1] - o.a[1]
  local dy = o.b[2] - o.a[2]
  return dx * dx + dy * dy
end

print(dist(s))
