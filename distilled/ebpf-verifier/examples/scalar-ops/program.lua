local dir = arg[0]:match("^(.*)/[^/]*$") or "."
dofile(dir .. "/../../oracle.lua")

run(16, [[
r0 = 0
r2 = *(u32 *)(r1 + 0)
r3 = *(u8 *)(r1 + 4)
r7 = *(u64 *)(r1 + 8)
r4 = r2
r4 *= r4
r5 = r2
r5 += r5
r5 *= r5
r6 = r3
r6 <<= 3
r6 >>= 1
r2 <<= 32
r2 <<= r3
r4 |= r3
r4 ^= r3
r4 -= r3
exit
]])
