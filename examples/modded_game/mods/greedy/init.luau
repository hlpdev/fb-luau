--!strict
-- A mod that never lets go: its tick runs past the time budget, and the game goes on without it.
on_tick(function(tick: number)
	if tick == 5 then
		while true do
		end
	end
end)
