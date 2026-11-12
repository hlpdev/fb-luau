--!strict
-- A mod that takes more memory than its State has as it loads, in one step: the limit stops it,
-- and the game goes on without it.
local hoard = string.rep("x", 32 * 1024 * 1024)
print(#hoard)
