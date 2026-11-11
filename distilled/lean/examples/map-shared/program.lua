local function cons(h, t) return {h, t} end
local list = cons(1, cons(2, cons(3, nil)))
local function show(l)
  local out = {}
  while l do out[#out + 1] = tostring(l[1]); l = l[2] end
  return "[" .. table.concat(out, ",") .. "]"
end
local function length(l) if l == nil then return 0 end return 1 + length(l[2]) end
local function map_inc(l) if l == nil then return nil end return cons(l[1] + 1, map_inc(l[2])) end
print("(" .. show(map_inc(list)) .. ", " .. show(list) .. ")")
