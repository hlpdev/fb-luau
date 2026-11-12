--!strict
local function add(a: number, b: number): number
	return a + b
end

local s: string = add(1, 2)
local t = { x = 1 }
t.y = 2
print(s, t, add("1", 2))
