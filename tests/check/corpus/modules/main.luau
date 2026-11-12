--!strict
local doors = require("./mods/doors")
local math2 = require("@lib/math")
local n: string = math2.twice(doors.count)
print(n, doors.open("front", 3))
