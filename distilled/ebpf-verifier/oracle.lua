-- The oracle of the ebpf-verifier entry: a concrete interpreter, run in real Lua 5.4.
--
-- It does not verify anything. It runs the program on every input it can have and says `reject`
-- if some input makes it misbehave, and `accept` if none does. The verifier is sound when it
-- never accepts a program this oracle rejects, and the examples are chosen so that the two also
-- agree in the other direction.
--
-- What counts as misbehaving:
--   * reading a register that was never written (r0 included, at exit),
--   * arithmetic on a pointer other than adding a number to it, or comparing a pointer,
--   * a load or store through a number,
--   * an access outside the buffer r1 points to, or outside fp-64..fp-8 on the stack (stack
--     accesses are 8 bytes wide and 8 aligned), or reading a stack slot that was never written,
--   * a path that revisits a state it is still in (it can run forever), or one that executes
--     more than LIMIT instructions.
--
-- Every load from the buffer returns a value the oracle chooses: all 256 values for a byte, and
-- two extremes for a wider load. Two loads of the same address are independent, as they are for
-- the verifier. Lua integers are 64 bit and wrap, which is what the instruction set does.
--
-- Usage, from the program.lua of an example:  run(<buffer size>, <assembly text>)

local LIMIT = 256

local ALU = {
    ["="] = "mov", ["+="] = "add", ["-="] = "sub", ["*="] = "mul", ["&="] = "and",
    ["|="] = "or", ["^="] = "xor", ["<<="] = "lsh", [">>="] = "rsh",
}
local JMP = {
    ["=="] = "eq", ["!="] = "ne", [">"] = "gt", [">="] = "ge", ["<"] = "lt", ["<="] = "le",
    ["s>"] = "sgt", ["s>="] = "sge", ["s<"] = "slt", ["s<="] = "sle", ["&"] = "set",
}
local WIDE = { [1] = nil, [2] = { 0, 0xffff }, [4] = { 0, 0xffffffff }, [8] = { 0, -1 } }

local function signed(sign, digits)
    local n = math.tointeger(tonumber(digits))
    if sign == "-" then
        return -n
    end
    return n
end

-- The assembly text, one instruction per line, as `main.c` prints it.
local function parse(text)
    local prog = {}
    for line in text:gmatch("[^\n]+") do
        local d, size, s, sign, off = line:match("^r(%d+) = %*%(u(%d+) %*%)%(r(%d+) ([+-]) (%d+)%)$")
        if d then
            prog[#prog + 1] = { k = "ldx", dst = tonumber(d), src = tonumber(s), size = tonumber(size) // 8,
                                off = signed(sign, off) }
        end
        if not d then
            size, d, sign, off, s = line:match("^%*%(u(%d+) %*%)%(r(%d+) ([+-]) (%d+)%) = r(%d+)$")
            if d then
                prog[#prog + 1] = { k = "stx", dst = tonumber(d), src = tonumber(s), size = tonumber(size) // 8,
                                    off = signed(sign, off) }
            end
        end
        if not d then
            sign, off = line:match("^goto ([+-])(%d+)$")
            if sign then
                prog[#prog + 1] = { k = "ja", off = signed(sign, off) }
                d = true
            end
        end
        if not d then
            local r, op, rhs
            r, op, rhs, sign, off = line:match("^if r(%d+) (%S+) (%S+) goto ([+-])(%d+)$")
            if r then
                local insn = { k = "jmp", op = JMP[op], dst = tonumber(r), off = signed(sign, off) }
                local reg = rhs:match("^r(%d+)$")
                if reg then
                    insn.src = tonumber(reg)
                else
                    insn.imm = math.tointeger(tonumber(rhs))
                end
                prog[#prog + 1] = insn
                d = true
            end
        end
        if not d then
            local r, op, rhs = line:match("^r(%d+) (%S+) (%S+)$")
            if r and ALU[op] then
                local insn = { k = "alu", op = ALU[op], dst = tonumber(r) }
                local reg = rhs:match("^r(%d+)$")
                if reg then
                    insn.src = tonumber(reg)
                else
                    insn.imm = math.tointeger(tonumber(rhs))
                end
                prog[#prog + 1] = insn
                d = true
            end
        end
        if not d then
            assert(line == "exit", "cannot parse: " .. line)
            prog[#prog + 1] = { k = "exit" }
        end
    end
    return prog
end

local function show(v)
    if v == nil then
        return "n"
    elseif type(v) == "table" then
        return v.k .. v.off
    end
    return tostring(v)
end

local function copy(t, n)
    local c = {}
    for i = 0, n do
        c[i] = t[i]
    end
    return c
end

local function cond(op, a, b)
    if op == "eq" then return a == b
    elseif op == "ne" then return a ~= b
    elseif op == "gt" then return math.ult(b, a)
    elseif op == "ge" then return not math.ult(a, b)
    elseif op == "lt" then return math.ult(a, b)
    elseif op == "le" then return not math.ult(b, a)
    elseif op == "sgt" then return a > b
    elseif op == "sge" then return a >= b
    elseif op == "slt" then return a < b
    elseif op == "sle" then return a <= b
    end
    return a & b ~= 0
end

local function alu(op, a, b)
    if op == "mov" then
        return b
    end
    if type(a) == "table" or type(b) == "table" then
        assert(op == "add" and type(a) == "table" and type(b) ~= "table", "pointer arithmetic")
        return { k = a.k, off = a.off + b }
    end
    if op == "add" then return a + b
    elseif op == "sub" then return a - b
    elseif op == "mul" then return a * b
    elseif op == "and" then return a & b
    elseif op == "or" then return a | b
    elseif op == "xor" then return a ~ b
    elseif op == "lsh" then return a << (b & 63)
    end
    return a >> (b & 63)
end

local function check(mem_size, prog)
    local onpath, done = {}, {}

    local function key(pc, regs, stack)
        local t = { pc }
        for i = 0, 10 do t[#t + 1] = show(regs[i]) end
        for i = 0, 7 do t[#t + 1] = show(stack[i]) end
        return table.concat(t, ",")
    end

    local function reads(insn)
        if insn.k == "alu" then
            return { insn.op ~= "mov" and insn.dst or nil, insn.src }
        elseif insn.k == "ldx" then
            return { insn.src }
        elseif insn.k == "stx" then
            return { insn.dst, insn.src }
        elseif insn.k == "jmp" then
            return { insn.dst, insn.src }
        elseif insn.k == "exit" then
            return { 0 }
        end
        return {}
    end

    local exec
    function exec(pc, regs, stack, depth)
        assert(depth <= LIMIT, "too many instructions on one path")
        local insn = prog[pc]
        assert(insn, "fell off the end of the program")
        local k = key(pc, regs, stack)
        assert(not onpath[k], "the program can run forever")
        if done[k] then
            return
        end
        onpath[k] = true
        for _, r in pairs(reads(insn)) do
            assert(regs[r] ~= nil, "read of an unwritten register r" .. r)
        end
        local next = pc + 1
        if insn.k == "alu" then
            local rhs = insn.src and regs[insn.src] or insn.imm
            local nr = copy(regs, 10)
            nr[insn.dst] = alu(insn.op, regs[insn.dst], rhs)
            exec(next, nr, stack, depth + 1)
        elseif insn.k == "ldx" or insn.k == "stx" then
            local p = regs[insn.k == "ldx" and insn.src or insn.dst]
            assert(type(p) == "table", "access through a number")
            local addr = p.off + insn.off
            local slot
            if p.k == "mem" then
                assert(addr >= 0 and addr + insn.size <= mem_size, "access outside the buffer")
            else
                assert(insn.size == 8 and addr % 8 == 0 and addr >= -64 and addr <= -8, "bad stack access")
                slot = (addr + 64) // 8
            end
            if insn.k == "stx" then
                local ns = copy(stack, 7)
                if slot then
                    ns[slot] = regs[insn.src]
                end
                exec(next, regs, ns, depth + 1)
            elseif slot then
                assert(stack[slot] ~= nil, "read of a stack slot that was never written")
                local nr = copy(regs, 10)
                nr[insn.dst] = stack[slot]
                exec(next, nr, stack, depth + 1)
            else
                local values = WIDE[insn.size]
                if not values then
                    values = {}
                    for v = 0, 255 do values[#values + 1] = v end
                end
                for _, v in ipairs(values) do
                    local nr = copy(regs, 10)
                    nr[insn.dst] = v
                    exec(next, nr, stack, depth + 1)
                end
            end
        elseif insn.k == "ja" then
            exec(next + insn.off, regs, stack, depth + 1)
        elseif insn.k == "jmp" then
            local a = regs[insn.dst]
            local b = insn.src and regs[insn.src] or insn.imm
            assert(type(a) ~= "table" and type(b) ~= "table", "a pointer was compared")
            if cond(insn.op, a, b) then
                next = next + insn.off
            end
            exec(next, regs, stack, depth + 1)
        end
        onpath[k] = nil
        done[k] = true
    end

    local regs = { [1] = { k = "mem", off = 0 }, [10] = { k = "stack", off = 0 } }
    exec(1, regs, {}, 1)
end

function run(mem_size, text)
    local ok, err = pcall(check, mem_size, parse(text))
    if not ok then
        io.stderr:write(tostring(err), "\n")
    end
    print(ok and "accept" or "reject")
end
