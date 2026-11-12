local t = setmetatable({}, { __index = function(_, k) return call(echo, k) end })
print(t.x, call(call, call, echo, 1))
local co = coroutine.wrap(function() for i = 1, 5 do yield(i) coroutine.yield(i) end end)
for i = 1, 5 do print(pcall(co)) end
print(pcall(function() front.Open = "no" end))
print(pcall(setmetatable, front, {}))
print(getmetatable(front))
