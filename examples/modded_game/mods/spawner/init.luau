--!strict
-- Spawns a rat every third tick, three in all.
local spawned = 0
on_tick(function(tick: number)
	if tick % 3 == 0 and spawned < 3 then
		spawned += 1
		spawn_item("rat", spawned)
	end
end)
