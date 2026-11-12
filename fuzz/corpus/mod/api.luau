print(front.Open, back.Open)
front.Open = true
print(tostring(front), front:Knock(1, 2))
print(echo(1, "two", { 3 }))
print(pcall(fail))
print(where())
print(call(function(a, b) return a + b end, 1, 2))
print(pcall(call, error, "raised"))
for i = 1, 3 do print(yield(i)) end
