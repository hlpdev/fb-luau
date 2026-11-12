-- A game's own script, which the build step compiles.
local door = { open = false }

function door.toggle()
    door.open = not door.open
    return door.open and "door opened" or "door closed"
end

return door
