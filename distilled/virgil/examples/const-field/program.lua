-- The same program in Lua.
local cfg = {scale = 7, count = 0, log = 0}

local function bump(c, by)
  c.log = by
  c.count = c.count + by
end

bump(cfg, 1)
bump(cfg, 1)
print(cfg.scale * cfg.count)
