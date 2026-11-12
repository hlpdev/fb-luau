--!strict
-- A mod that fails as it loads, having hooked the tick first: it's dropped, its hook too.
on_tick(function(tick: number)
	log("thrower ticks " .. tick)
end)
error("the thrower gives up")
