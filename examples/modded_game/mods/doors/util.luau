--!strict
return {
	describe = function(door: Door): string
		return door.Name .. (if door.Open then " is open" else " is shut")
	end,
}
