local dir = arg[0]:match("^(.*)/[^/]*$") or "."
dofile(dir .. "/../../oracle.lua")

run(8, [[
r0 = 0
r2 = *(u8 *)(r1 + 0)
r3 = r2
r3 -= 128
if r2 == 7 goto +13
if r2 > 200 goto +12
if r2 < 3 goto +11
if r2 >= 190 goto +10
if r2 <= 4 goto +9
if r3 s> 100 goto +8
if r3 s< -100 goto +7
if r3 s>= 90 goto +6
if r3 s<= -90 goto +5
if r3 != 50 goto +4
if r2 > r3 goto +3
if r2 == r3 goto +2
if r2 & 1 goto +1
if r2 & 6 goto +0
exit
]])
