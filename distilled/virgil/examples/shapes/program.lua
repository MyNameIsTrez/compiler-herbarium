-- The same program in Lua: classes are tables with a shared method table.
local Shape = {}
Shape.__index = Shape
function Shape.area() return 0 end
local Square = setmetatable({}, {__index = Shape})
Square.__index = Square
function Square.area(s) return s.side * s.side end
local Circle = setmetatable({}, {__index = Shape})
Circle.__index = Circle
function Circle.area(c) return c.r * c.r * 3 end
local Tri = setmetatable({}, {__index = Shape})
Tri.__index = Tri
function Tri.area(t) return t.b * t.h end
local Dot = setmetatable({}, {__index = Shape})
Dot.__index = Dot

local function new(class, fields) return setmetatable(fields, class) end

local sq = new(Square, {side = 3, tag = 7})
local head = {
  shape = sq,
  next = {
    shape = new(Square, {side = 4, tag = 7}),
    next = {
      shape = new(Dot, {}),
      next = {shape = sq},
    },
    extra = new(Tri, {b = 1, h = 2}),
  },
  extra = new(Tri, {b = 3, h = 4}),
}

local function sum(n)
  if n then return n.shape:area() + sum(n.next) end
  return 0
end

local function mkc() return new(Circle, {r = 5}) end

print(sum(head) + mkc():area() + new(Circle, {r = 5}):area())
