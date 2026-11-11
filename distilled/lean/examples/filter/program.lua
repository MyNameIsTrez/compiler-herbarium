local function cons(h, t) return {h, t} end
local list = cons(1, cons(2, cons(3, nil)))
local function show(l)
  local out = {}
  while l do out[#out + 1] = tostring(l[1]); l = l[2] end
  return "[" .. table.concat(out, ",") .. "]"
end
local function length(l) if l == nil then return 0 end return 1 + length(l[2]) end
local function filter(l)
  if l == nil then return nil end
  if 1 < l[1] then return cons(l[1], filter(l[2])) end
  return filter(l[2])
end
print(show(filter(list)))
