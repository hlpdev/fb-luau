--!strict
-- A mod that takes all the memory it can as it loads: the State's limit stops it, and the game
-- goes on without it.
local kept = {}
for i = 1, 1e7 do
	kept[i] = string.rep("x", 1024) .. i
end
