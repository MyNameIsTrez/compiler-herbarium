-- The same program in Lua: a generic function is one function, and a tuple is a table.
local function id(x) return x end
local function dup(x) return {x, x} end
local function first(x) return dup(x)[1] end
local function last(x) return dup(x)[2] end
local function rt(x) return first(id(x)) end

local A = {}
A.__index = A
local B = {}
B.__index = B

local function fresh(class) return setmetatable({v = 0, w = 0}, class) end

local pr = rt({2, 3})
print(rt(3) + rt(setmetatable({v = 4}, A)).v + rt(setmetatable({w = 5}, B)).w + pr[1] + pr[2]
      + last(7) + fresh(A).v + fresh(B).w)
