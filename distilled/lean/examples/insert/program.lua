local function node(l, k, r) return {l, k, r} end
local function insert(t, x)
  if t == nil then return node(nil, x, nil) end
  if x < t[2] then return node(insert(t[1], x), t[2], t[3]) end
  return node(t[1], t[2], insert(t[3], x))
end
local function show(t)
  if t == nil then return "." end
  return "(" .. show(t[1]) .. " " .. t[2] .. " " .. show(t[3]) .. ")"
end
print(show(insert(insert(insert(nil, 2), 1), 3)))
