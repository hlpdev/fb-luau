--!strict
-- Opens the front door a moment after the game starts, and says so.
local words = require("@lib/words")
local util = require("@self/util")

task.spawn(function()
	task.wait(0.25)
	front.Open = true
	log(words.shout(util.describe(front)))
end)
