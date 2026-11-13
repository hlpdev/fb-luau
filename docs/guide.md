# The luau guide

`luau` runs [Luau](https://luau.org) scripts in a FutureBlitz program: scripts a program didn't
write, like a game's mods, sandboxed, with a typed API where Luau's stack is never seen. This
guide goes through it a part at a time, each with a program that runs; `tools/guide_programs.py`
runs every one of them, as ctest does.

Each program is whole: put it in a project whose `project.toml` has

```toml
[dependencies]
luau = "0.1"
```

and `fb run` it. They check what they show with `Expect`, so a program that runs is one that
did what the guide says.

## A State, and running code

A `LuauState` is a Luau VM of its own: its globals, its memory, its scripts. `Run` runs source,
`Evaluate<T>` gives what it returns as a T, and `Get` and `Set` read and write its globals.

```vb
Import std.test
Import luau

Local lua = LuauState()
Try lua.Run("greeting = 'hello, ' .. 'world'")
Expect(Try lua.Get<String>("greeting") == "hello, world")
Expect(Try lua.Evaluate<Int>("return 6 * 7") == 42)

Try lua.Set("speed", 4.5)
Expect(Try lua.Evaluate<Float64>("return speed * 2") == 9.0)

// Load compiles once; the function it gives runs as often as it's called.
Local count = Try lua.Load("calls = (calls or 0) + 1")
Try count.Invoke()
Try count.Invoke()
Expect(Try lua.Get<Int>("calls") == 2)
lua.Close()
```

A State goes when the program drops it, or at `Close()`, which is final: what it gave the
program, a table or a function, can't be used after.

## Values

A value crosses by its type, the one the program names: `Bool` is a boolean, every number type
is a number, `String` is a string, `Vec3` is a vector, and a `Map` is a table, copied. A value of
another type is a `LuauError` of `CONVERSION`, saying what it was. `LuauValue` takes any value,
for when its type is known only as the program runs.

```vb
Import std.collections
Import std.test
Import luau

Local lua = LuauState()
Local scores: Map<String, Int> = { "ana": 3, "bo": 5 }
Try lua.Set("scores", scores)
Expect(Try lua.Evaluate<Int>("return scores.ana + scores.bo") == 8)

Local error = ExpectThrows<LuauError>(lua.Evaluate<Int>("return 'seven'"))
Expect(error != Null && error.Kind == LuauErrorKind.CONVERSION)

Local any = Try lua.Evaluate<LuauValue>("return 'hi'")
Expect(any.Kind == LuauValueKind.STRING && any.AsString() == "hi")
```

A `T?` doesn't cross yet, as FutureBlitz can't give it `LuauType`. A function of the program's
that gives something or nil gives a `LuauValue`, made by `LuauValue.Of(lua, value)` or
`LuauValue.Nil(lua)`, which need the State, so the function captures it `[Weak lua]`, as an
object the State keeps holds it weakly.

```vb
Import std.test
Import luau

Local lua = LuauState()
Local names = ["hall", "vault"]
Try lua.SetFunction("door", Function[Weak lua](number: Int): LuauValue Throws LuauError
    Local held = lua
    If held == Null Then Throw LuauError(LuauErrorKind.CLOSED, "the Luau State is closed")
    If number < 1 || number > names.Count Then Return LuauValue.Nil(held)
    Return Try LuauValue.Of(held, names[number - 1])
End Function)
Expect(Try lua.Evaluate<String>("return door(2)") == "vault")
Expect(Try lua.Evaluate<Bool>("return door(3) == nil"))
```

A Struct or Class that lists `LuauRecord` crosses as a table of its Fields, by name, both ways:

```vb
Import std.test
Import luau

Struct Spawn Implements LuauRecord
    Field Name: String
    Field Count: Int = 1
    Field Tags: String[]
End Struct

Local lua = LuauState()
Local spawn = Try lua.Evaluate<Spawn>(r"return { Name = 'rat', Tags = { 'small' } }")
Expect(spawn.Name == "rat" && spawn.Count == 1 && spawn.Tags == ["small"])
```

## Tables

A `LuauTable` is a handle to a table in the VM, not a copy: what the program sets, the script
sees. `Get` and `Set` go through its metatable, as a script's do; `RawGet` and `RawSet` don't.

```vb
Import std.test
Import luau

Local lua = LuauState()
Local config = Try lua.NewTable()
Try config.Set("volume", 0.8)
Try lua.Set("config", config)
Try lua.Run("config.muted = config.volume == 0")
Expect(!(Try config.Get<Bool, String>("muted")))

Local list = Try lua.Evaluate<LuauTable>(r"return { 'a', 'b', 'c' }")
Expect(list.Length() == 3 && (Try list.ToArray<String>()) == ["a", "b", "c"])
```

## Functions both ways

`SetFunction` gives a script a FutureBlitz function, its arguments read by their types and its
result given back by its type; a script that passes the wrong type gets an error worded as
Luau's own are. An error the function throws is raised in the script, which its `pcall` catches,
and one the script lets go comes back out of the program's call as the `LuauError`'s `Cause`.

```vb
Import std.core
Import std.test
Import std.text
Import luau

Function ReadSave(name: String): String Throws IOError
    Throw IOError(IOErrorKind.NOT_FOUND, "{name} doesn't exist")
End Function

Local lua = LuauState()
Try lua.SetFunction("add", Function(a: Int, b: Int) => a + b)
Try lua.SetFunction("read_save", ReadSave)
Expect(Try lua.Evaluate<Int>("return add(2, 3)") == 5)

Local wrong = ExpectThrows<LuauError>(lua.Run("add(1, 'two')", "=mod"))
Expect(wrong != Null && wrong.Message.Contains("invalid argument #2 to 'add'"))

Expect(!(Try lua.Evaluate<Bool>("return (pcall(read_save, 'slot1'))")))
Local thrown = ExpectThrows<LuauError>(lua.Run("read_save('slot1')"))
Expect(thrown != Null && thrown.Cause Is IOError)
```

A Luau function is a `LuauFunction` the program calls: `Call<R>` gives its first result as an R,
`Invoke` none, `CallAll` every one, and `ToFunc` makes it a Func the program passes around.

```vb
Import std.test
Import luau

Local lua = LuauState()
Try lua.Run("function area(w, h) return w * h, 'square units' end")
Local area = Try lua.Get<LuauFunction>("area")
Expect(Try area.Call<Float64>(2.0, 3.0) == 6.0)
Expect((Try area.CallAll(2.0, 3.0)).Count == 2)

Local f = Try area.ToFunc<Float64, Float64, Float64>()
Expect(Try f(4.0, 5.0) == 20.0)
```

A function given to Luau that needs its State captures it `[Weak lua]`, so the State can still
go: one that holds it strongly keeps it alive through the State's own map of its functions.

## Errors

Every call that runs Luau throws a `LuauError`: its `Kind` (`SYNTAX`, `RUNTIME`, `MEMORY`,
`TIMEOUT`, `CONVERSION`, `YIELD`, `CLOSED`), its `Message` as Luau writes it, its `Traceback`,
and the `Value` a script raised, when it raised one that isn't a string.

```vb
Import std.text
Import std.test
Import luau

Local lua = LuauState()
Local syntax = ExpectThrows<LuauError>(lua.Run("local = 1", "=mod"))
Expect(syntax != Null && syntax.Kind == LuauErrorKind.SYNTAX)

Local raised = ExpectThrows<LuauError>(lua.Run(r"error({ code = 7 })"))
Expect(raised != Null && raised.Kind == LuauErrorKind.RUNTIME)
Local value = raised?.Value
Expect(value != Null && (Try (Try value.Read<LuauTable>()).Get<Int, String>("code")) == 7)
```

## Registering types, and what to expose

A Class that lists `LuauObject` crosses as itself: a script holds the object, not a copy, and
each object has a userdata of its own, whatever its Class's `==` says. `Register<T>()` exposes only what's marked: `@LuauExpose` for a Method that gives a value,
`@LuauAction` for one that gives nothing, `@LuauGet` and `@LuauSet` for a field, each named as
it is or by the name given. What isn't marked can't be reached; a marked Method of a shape that
can't be exposed panics where it's registered, saying why.

```vb
Import std.test
Import luau

Class Player Implements LuauObject
    Field health: Int = 100
    Field secret: String = "not for mods"

    @LuauGet("Health") Method GetHealth(): Int => health
    @LuauExpose Method Damage(amount: Int): Int
        health -= amount
        Return health
    End Method
    @LuauAction("heal") Method Heal()
        health = 100
    End Method
End Class

Local lua = LuauState()
Try lua.Register<Player>()
Local player = Player()
Try lua.Set("player", player)
Expect(Try lua.Evaluate<Int>("return player:Damage(30)") == 70)
Try lua.Run("player:heal()")
Expect(player.GetHealth() == 100)
Expect(Try lua.Evaluate<String>("return typeof(player)") == "Player")
Expect(ExpectThrows<LuauError>(lua.Run("return player.secret")) != Null)
```

`Register` gives a `LuauTypeBuilder`, whose `AddMethod`, `AddGetter`, `AddSetter`, and
`AddOperator` add what reflection can't, written in Luau.

## Lifetimes, and the cycle to avoid

An object a script holds stays alive while the script holds it, and goes when neither side
does; a handle, like a `LuauTable`, holds its value in the VM until the handle goes. Two
collectors see each side, and neither sees across: an object that holds a `LuauTable` that holds
the object is a cycle neither frees, which lives until its State closes. Hold a `Weak<T>`, or a
key, where the cycle would be.

```vb
Import std.test
Import luau

Class Door Implements LuauObject
    // Not a LuauTable of the door's: the door's own state, which the script reads by Methods.
    Field openedBy: String = ""
    @LuauAction Method Open(who: String)
        openedBy = who
    End Method
End Class

Local before = LuauStatesOpen(here: True)
Local lua = LuauState()
Try lua.Register<Door>()
Try lua.Set("door", Door())
Try lua.Run("door:Open('ana')")
lua.Close()
Expect(LuauStatesOpen(here: True) == before)
```

`LuauStatesOpen()` gives how many States the process has open and the bytes they hold, which a
program's tests and soaks watch come back to what they were.

## Vectors and buffers

Luau's vector is std.math's `Vec3`, crossing as a value; a `LuauBuffer` is a handle to a buffer
the program reads and writes in place.

```vb
Import std.math
Import std.test
Import luau

Local lua = LuauState()
Try lua.Set("up", Vec3(0, 1, 0))
Local moved = Try lua.Evaluate<Vec3>("return up * 2 + vector.create(1, 0, 0)")
Expect(moved == Vec3(1, 2, 0))

Local bytes = Try lua.NewBuffer(4)
Try lua.Set("bytes", bytes)
Try lua.Run("buffer.writeu32(bytes, 0, 0xDEADBEEF)")
Expect(bytes.ReadUInt32(0) == 0xDEADBEEF)
```

## Threads on the scheduler

A script's `task.wait(2)` waits as an `Await Wait(2.Seconds)` does, while the program goes on.
Something has to resume it: call `lua.Start()` once, before a script's first wait, and it runs
every wait of the State's, `task.spawn`'s and `task.delay`'s too, at each tick of the scheduler
it's on, until the State closes. A `LuauThread`'s `Start()` runs its own thread the same way, as
an Async Function, whose Task is done when the thread ends. A Scheduler of the program's own,
which a pause stops and whose `Tick` scales time, runs them where it's given them. `SetAsync`
gives a script an Async Function: its thread waits for the Task. A String its argument takes is
a copy of the script's, held for as long as the Function awaits, once for every thread that
calls it; where a script's string may be large, take a `LuauValue` or a `LuauTable`, which stay
the State's.

```vb
Import std.core
Import std.test
Import luau

Async Function Fetched(name: String): String
    Await NextTick()
    Return "{name}'s save"
End Function

Local lua = LuauState()
Local world = Scheduler()
// Once, before any wait: the State's waits run on the world's ticks from here.
world.Run(lua.Start())
Try lua.SetAsync("fetch", Function(name: String) => Fetched(name))
Try lua.Run(r"task.delay(0.5, function() delayed = true end)")
Try lua.Run(r"function main() task.wait(1) result = fetch('ana') end")
Local done = (Try lua.Spawn(Try lua.Get<LuauFunction>("main"))).Start()
world.Run(done)
Local time = 0.0
While !done.IsDone()
    world.Tick(0.25)
    time += 0.25
End While
Expect(time >= 1.0 && Try lua.Get<String>("result") == "ana's save")
Expect(Try lua.Get<Bool>("delayed"))
// The State's driver runs until the State closes, which ends it.
lua.Close()
```

A wait before `lua.Start()`, with nothing to run it, is a `LuauError` of `YIELD` that says
`lua.Start()` runs them; so is one inside a metamethod, or on a State made on a thread of the
program's other than the main one, where nothing Async runs. A thread that waits is `WAITING`: the program follows it by its Task, not
by `Resume`. A State closed while its threads wait ends every wait, and each Start's Task throws
`CLOSED`.

## Sandboxing a game's mods

A State is sandboxed as Luau's own sandbox makes it: `Freeze()` makes the globals and every
library read-only, and each `LuauScript` has globals of its own over them, so one script can't
change what another sees; a State's first `NewScript` freezes it, if the program hasn't.

A State is what keeps one mod from another. Its memory limit, its waits, its threads, and its
modules are the State's: scripts in one State share them, and a thread or a function one script
hands to another's code is theirs to resume, close, or call. So the scripts of one State are one
author's files, and each mod, a stranger's code, gets a State of its own.
`LuauOptions` sets a memory limit for the whole State, 256 MB unless the program sets one, and a
time budget for each call into it, 10 seconds unless it sets one; 0 is none, by the program's
choice. `LuauOptions.Sandbox()` is a mod's, tighter: 64 MB, 50 ms a call, and 1,000 waits.

Past the time budget, the script meets a `LuauError` of `TIMEOUT` at every safe point, which its
own `pcall` can't keep it from, and the call ends. Past the memory limit, the allocation that
went past fails with "not enough memory", which a script's `pcall` does catch, as Luau's does, so
a script can go on; the call gives a `MEMORY` error only when the script doesn't catch it. And
the limit is the State's, not a script's: a script that hoards makes every other script's calls
in its State fail for want of memory until it lets go, which a State for each mod keeps to the
mod.

```vb
Import std.test
Import std.text
Import std.time
Import luau

Local options = LuauOptions()
options.MemoryLimit = 8 * 1024 * 1024
options.TimeBudget = 20.Milliseconds
Local lua = Try LuauState(options)
Try lua.SetFunction("say", Function(text: String): Int => text.Length)
Try lua.Freeze()

Local first = Try lua.NewScript("=first")
Local second = Try lua.NewScript("=second")
Try first.Run("name = 'first'")
Expect(Try second.Evaluate<Bool>("return name == nil"))
// A script's own global hides the shared one only for itself; the shared ones are read-only.
Try first.Run("say = nil")
Expect(Try second.Evaluate<Int>("return say('hi')") == 2)
Expect(ExpectThrows<LuauError>(first.Run("string.upper = nil")) != Null)

Local spin = ExpectThrows<LuauError>(first.Run("while true do end"))
Expect(spin != Null && spin.Kind == LuauErrorKind.TIMEOUT)
Local hog = ExpectThrows<LuauError>(first.Run("local big = string.rep('x', 16 * 1024 * 1024)"))
Expect(hog != Null && hog.Kind == LuauErrorKind.MEMORY)
Expect(Try second.Evaluate<Int>("return say('still here')") == 10)
```

The libraries a State opens are Luau's safe ones; `loadstring`, `getfenv`, `setfenv`, and all
of `collectgarbage` are off unless the options turn them on. Nothing reaches a file or the OS
unless the program gives a function that does.

## Require

`require` is Luau's own, with its rules for paths: `./` and `../` from the requiring module,
`@self`, and the aliases of a `.luaurc`; a module that's a folder's `init.luau` is the
folder, so its `./` is beside the folder, and `@self/` is inside it. The program gives the files
through
`LuauOptions.Modules`, by their paths from the modules' root, so a mod requires only what the
program gives it, from wherever it keeps them. Each module runs once per State, and its table is
frozen, as every script that requires it shares it.

```vb
Import std.collections
Import std.test
Import luau

Local files: Map<String, String> = {
    "lib/words.luau": r"return { shout = function(s) return string.upper(s) .. '!' end }",
    "mods/doors/init.luau": r"local words = require('../lib/words') return words.shout('open')"
}
Local options = LuauOptions()
options.Modules = Function(path: String): String? => files.Get(path)
Local lua = Try LuauState(options)
Local doors = Try lua.NewScript("@mods/doors/main.luau")
Expect(Try doors.Evaluate<String>(files["mods/doors/init.luau"] ?? "", "@mods/doors/init.luau") ==
       "OPEN!")
```

## Bytecode

A game's own scripts can be compiled ahead, as its build does, into bytecode a State runs
without compiling. Luau runs bytecode as it's given, checking nothing, so bytecode crosses a file
system only in `Save`'s container, signed by the build with an Ed25519 private key that never
ships, which `Load` checks in the game with the public key before Luau sees a byte. The key that
checks is compiled into the game, an `Embed` or a `Const`, never read from where the containers
are, as a mod that can replace a container could replace a key beside it. The key that signs
never leaves the build machine: it's never in the repository, nor written beside what ships. The
public key is no secret; the private key is the whole of it. A mod is source, always, and scripts
embedded in the executable can as well be compiled from source as it starts.

```vb
Import std.crypto
Import std.test
Import luau

// The build's: Ed25519.NewPrivateKey() made it once, and the build keeps it.
Local privateKey = Ed25519.NewPrivateKey()
Local shipped = (Try LuauBytecode.Compile("return 6 * 7", "@rooms/answer.luau")).Save(privateKey)

// The game's, which ships with the public key only.
Local publicKey = Ed25519.PublicKeyOf(privateKey)
Local loaded = Try LuauBytecode.Load(shipped, publicKey)
Expect(Try LuauState().Evaluate<Int>(loaded) == 42)
Local another = Ed25519.PublicKeyOf(Ed25519.NewPrivateKey())
Expect(ExpectThrows<LuauError>(LuauBytecode.Load(shipped, another)) != Null)
```

examples/bytecode is a build step and its game, as two modes of one program, the game with
its public key embedded, and the program that makes its key pair once, as a game makes its own:
the pair is never committed.

## Native code

Where Luau's code generator is supported, x64 and arm64, a State can compile code to native code
as it loads. It's off by default, as a script shouldn't turn it on for itself; with
`LuauNativeCode.MARKED`, a chunk that starts with `--!native` and a function marked `@native`
are; with `LuauNativeCode.ALL`, everything. Its code takes memory of its own, outside the memory
limit, up to `LuauOptions.NativeMemory`, 16 MB by default, past which code runs in the
interpreter. Where the generator isn't supported, the interpreter runs it, with the same results.

```vb
Import std.test
Import luau

Local options = LuauOptions()
options.NativeCode = LuauNativeCode.ALL
Local lua = Try LuauState(options)
Expect(Try lua.Evaluate<Int>("local s = 0 for i = 1, 100 do s += i end return s") == 5050)
Expect(!LuauNativeCodeSupported() || lua.NativeFunctions > 0)
```

## Checking mods

`LuauChecker` is Luau's type checker and lint, as `luau-analyze` runs them, over files the
program reads. `lua.Definitions()` writes what a State gives its scripts, its registered types
and the globals it set, and what its options took away, like loadstring, as a definitions file,
so a mod is checked against the game's real API before it's loaded, and a mod's editor can know
it too.

```vb
Import std.collections
Import std.test
Import luau

Local lua = LuauState()
Try lua.SetFunction("spawn_item", Function(name: String, count: Int) => count)
Local files: Map<String, String> = {
    "good.luau": "--!strict\nspawn_item('rat', 2)\n",
    "bad.luau": "--!strict\nspawn_item(2, 'rat')\n"
}
Local checker = LuauChecker(Function(path: String): String? => files.Get(path))
Try checker.AddDefinitions(lua.Definitions(), "game")
Expect((Try checker.Check("good")).Count == 0)
Local found = Try checker.Check("bad")
Expect(found.Count == 2 && found[0].Name == "TypeError" && found[0].Line == 2)
```

Each `LuauDiagnostic` has its module, line and column, its severity, Luau's code and name for
it, and its message, as luau-analyze gives them.

A mod sets its own checking, as luau-analyze lets any module: `--!nocheck` and `--!nolint`
lines, and a `.luaurc` in its folder that turns its mode down, its lint off, or its unknown
globals into `any`. A game's mod checker wants its own rules to stand, so `LuauCheckerOptions`
has `Enforced`: with it, the checker's mode and Luau's default lint hold for every module, and
a `.luaurc` gives only its aliases.

```vb
Import std.collections
Import std.test
Import luau

Local files: Map<String, String> = {
    "mods/sly/init.luau": "--!nocheck\nlocal n: number = 'x'\nprint(n)\n"
}
Local options = LuauCheckerOptions()
options.Mode = LuauCheckMode.STRICT
options.Enforced = True
Local checker = LuauChecker(Function(path: String): String? => files.Get(path), options)
Expect((Try checker.Check("mods/sly"))[0].Name == "TypeError")
```

A check has one time limit, `TimeLimit`, 10 seconds by default, for all its modules: past it,
the check gives one diagnostic named "TimeLimit". It's Luau's solver that's stopped there; a
module's constraint generation, which comes before, and its lint can't be, so a check can run
past the limit by those of the modules under way.

Luau's solver would run a mod's code as it checks it: a `type function`'s body, with the
program's memory and native stack, which no limit holds. So a check that reaches a module with
a `type function` is refused, with a diagnostic named "TypeFunction" at each, and none is run.

The checker is a help: to a mod's author, who sees what they got wrong before the game does,
and to a game that wants mods it can read. It isn't what keeps a game safe from a mod. A mod
that passes its check runs in its State's sandbox as one that wasn't checked does, and the
sandbox, its limits, and the API the game gives are what hold it.

## A game's modding

The parts above make the shape a game's modding takes, the one SCP: Containment Breach X's mods
take, which any game can follow:

1. **A State for each mod**, made with `LuauOptions.Sandbox()`, and its `Modules` over the mods'
   folder. Its `Print` goes to the game's log.
2. **What mods reach, and nothing else:** the game's types registered, marking only what a mod
   may use, its functions set, events a mod hooks by giving a function, then `Freeze()`.
3. **Its scheduler started first:** `world.Run(lua.Start())` on the game's own Scheduler, so a
   mod's waits run on the game's time, stop when it pauses, and a mod can wait as it loads.
4. **Each mod checked, then loaded:** `LuauChecker`, Enforced, with `lua.Definitions()` finds
   what a mod gets wrong before it runs; then it runs in its State's `NewScript`.
5. **Each mod's failure its own:** each mod's check, its load, and every call of the game's
   into it are caught for that mod alone, as a LuauError of the mod's: one that fails is dropped,
   its State closed, which ends its waits and threads, and the game goes on. `ThreadError` hears
   of a thread that failed on the scheduler.

examples/modded_game is that game, small, with eight mods: one refused by the checker, one that
runs out of memory as it loads, one that loops as it loads, which tried to turn its checking
off, one that fails as it loads after hooking the tick, one of two files where it can have one,
one stopped at its budget in a tick, one that waits and requires a shared module, and one that
hooks the game's tick; the last two play to the end.
