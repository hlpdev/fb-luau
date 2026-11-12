--!strict
-- What every mod can require as "@lib/words": the modules are shared, and frozen.
return {
	shout = function(text: string): string
		return string.upper(text) .. "!"
	end,
}
