local function cons(h, t) return {h, t} end
local list = cons(1, cons(2, cons(3, nil)))
local function show(l)
  local out = {}
  while l do out[#out + 1] = tostring(l[1]); l = l[2] end
  return "[" .. table.concat(out, ",") .. "]"
end
local function uncons(l)
  if l == nil then return {0, nil} end
  return {l[1], l[2]}
end
local p = uncons(list)
print("(" .. p[1] .. ", " .. show(p[2]) .. ")")
