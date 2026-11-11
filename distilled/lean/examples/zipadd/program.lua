local function cons(h, t) return {h, t} end
local list = cons(1, cons(2, cons(3, nil)))
local function show(l)
  local out = {}
  while l do out[#out + 1] = tostring(l[1]); l = l[2] end
  return "[" .. table.concat(out, ",") .. "]"
end
local function zipadd(xs, ys)
  if xs == nil or ys == nil then return nil end
  return cons(xs[1] + ys[1], zipadd(xs[2], ys[2]))
end
print(show(zipadd(list, list)))
