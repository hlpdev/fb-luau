--!strict
local util = require("@self/util")
return {
	count = 2,
	open = function(name: string): boolean
		return util.known(name)
	end,
}
