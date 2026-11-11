local function fact(n)
  if n < 2 then
    return 1
  end
  return n * fact(n - 1)
end
print(fact(5))
