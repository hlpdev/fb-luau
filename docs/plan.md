# The luau package: the plan

`luau` is a FutureBlitz package that runs [Luau](https://luau.org) scripts in a FutureBlitz
program: one package, `luau`, over one library of its own, `luau_native`, which has Luau in it.
Its first user is SCP: Containment Breach X, whose mods are Luau and TOML, sandboxed, and never
native code; any program that wants scripts it didn't write, or scripts of its own, can use it.

This plan has what the package is, its API by area, the library under it, how it's built,
shipped, tested, and documented, and the milestones L0 to the first release. Each milestone's
steps land one at a time, as fbengine's and FutureBlitz's do. docs/decisions.md has why each
choice was made, newest last; a change to this plan is made here first and said there.

Everything here was checked against FutureBlitz 1.3.0's spec (docs/spec/ in its repo) and
Luau's sources at the release this plan pins, 0.746. Where FutureBlitz can't yet do what the
design needs, [What FutureBlitz can't do yet](#what-futureblitz-cant-do-yet) says so, with what
the package does meanwhile.

## What it is and isn't

It is:

- A typed API in sol2's style: values cross by their FutureBlitz types, and Luau's stack is
  never seen. A program reads `Try lua.Get<Int>("lives")`, not a stack index.
- Functions both ways, throwing ones too, with errors that keep Luau's traceback.
- Classes registered by compile-time reflection, which cross as userdata, and Structs that cross
  by value.
- Sandboxed by default: a script gets what the program gives it and nothing else, within a
  memory limit and a time budget per call.
- Luau's threads driven by FutureBlitz's schedulers, so a script's `task.wait(2)` waits as an
  Async Function's `Await Wait(2.Seconds)` does.
- `require` resolved by the program, bytecode compiled ahead, native code generation, and Luau's
  type checker and linter with their diagnostics.

It isn't:

- Lua 5.x. Luau is its own language, Lua 5.1 with types and more; scripts written for Lua 5.4
  may not run.
- A way around FutureBlitz's own rules. A Class that crosses into Luau stays on its thread, as
  every Class does, and so does a State.
- A way to load bytecode a program didn't compile itself. Luau trusts its bytecode completely, so
  the package loads only what the program compiled, and a mod is always source.
- Part of FutureBlitz, std, or fbengine. It's a package like any other, on the registry, and an
  fbengine game uses it as any program does.

## The two layers

```
  a program's .fb code
        |
  luau/*.fb              the public API: LuauState, LuauTable, LuauFunction, ... (typed, Throws)
        |
  luau/c/fbl_*.fb        the bindings, Package-only, which fb-bindgen writes from the headers
        |  C ABI: fixed-size types, handles, a status per call
  native/<area>/         the shim, C++: fbl_<area>_<verb> functions, every one protected
        |
  Luau 0.746             VM, Compiler, CodeGen, Analysis, Require, built into luau_native
```

1. **The shim** is a small C++ library, `luau_native`, whose C API is the only thing the DLL
   exports. Every function in it is safe to call with any arguments: it checks its handles,
   indexes, and types, since Luau's own API checks nothing in a release build (`api_check` is
   an assert there), and it runs whatever can raise a Luau error under protection, so no Luau
   error, and no C++ exception, ever leaves it. It gives back a status, and what went wrong in
   its last-error text, as fbengine's libraries do.
2. **The public API** is FutureBlitz, over the bindings. It turns statuses into `LuauError`s and
   panics, values into types, and FutureBlitz functions and Classes into what Luau calls. A
   program sees only this layer: the bindings are Package declarations, which a program can't
   name.

## The API

Every name is prefixed with `Luau`, since `Import luau` brings them in without a qualifier, and
the plain names are taken: `Thread` and `Function` are FutureBlitz's own, `Buffer` is std.ffi's,
and `Table` and `State` are what a game names its own things. Everything that runs Luau, or makes
anything in its memory, can run out of memory or time, so it `Throws LuauError`.

### State

```vb
Import std.core
Import luau

Local lua = LuauState()
Try lua.Run("lives = 3")
Print(Try lua.Get<Int>("lives"))           // 3
```

- `LuauState()` makes a VM with the default options, which are sandboxed
  ([Sandboxing](#sandboxing-and-limits)), and panics only when the system has no memory for one;
  `LuauState(options)` takes a `LuauOptions`, and throws a `LuauError` of `MEMORY` when the State
  doesn't fit in its memory limit.
- A State is a Class, so it isn't Sendable: it stays on the thread that made it, as FutureBlitz
  keeps every Class's objects, and nothing else in the package is Sendable either. Two States on
  two threads are two VMs that share nothing, which Luau allows, as nothing in its VM is per
  process but its flags, which the library sets once, before the first State.
- `lua.Close()` closes it at once: the VM and everything in it go, and so does what it held of
  the program's (the objects and functions it was given). A State closes in its Destroy when the
  program lets go of it. Its handles hold it weakly, as sol2's references don't keep their state,
  since a function of the program's that holds a handle is held by the State: a handle used once
  its State is closed, or gone, panics, saying so, at the call.
- `lua.MemoryUsed`, `lua.Collect()` (a full collection), and `lua.Step(kilobytes)` look at and
  drive Luau's collector.

### Running and compiling code

```vb
Try lua.Run("print('hi')")                              // source, named "=chunk"
Try lua.Run(source, "@mods/doors/init.luau")            // a chunk name, which tracebacks show
Local total = Try lua.Evaluate<Int>("return 1 + 2")    // its first result
Local main = Try lua.Load(source, "@main.luau")         // a LuauFunction, not run yet
```

- `Run` compiles and runs source, `Evaluate<T>` gives its first result as a T, and `Load` gives
  the chunk as a function to call later. Each compiles with the State's compile options, and a
  syntax error is a `LuauError` of `SYNTAX`, with the line it's at.
- `LuauBytecode.Compile(source, name, options)` compiles without a State, for running later, or
  for shipping ([Bytecode](#bytecode)). `lua.Run(bytecode)` and `lua.Load(bytecode)` take one.
- `LuauCompileOptions` has Luau's: `Optimization` (0 to 2, 1 by default), `Debug` (0 to 2, 1 by
  default, the lines and names a traceback needs), `Coverage`, and the globals a script may
  change, whose lookups the compiler then doesn't fold.

### Values and handles

A value crosses by its type. The ones that are Luau's own, a table, a function, a thread, a
buffer, and a userdata that isn't one of the program's objects, are handles: a reference to it,
which keeps it alive in the VM until the handle goes.

| FutureBlitz | Luau |
|---|---|
| `Bool` | boolean |
| `Int8` to `Int64`, `UInt8` to `UInt64`, `Float32`, `Float64` | number |
| `LuauInteger` (a Struct of an `Int64`) | integer |
| `String` | string |
| `LuauBytes` (a Struct of a `UInt8[]`) | string, as bytes |
| `Vec3` (std.math) | vector |
| `Map<K, V>` of any of these | a table, copied |
| a Struct or Class that implements `LuauRecord` | a table of its Fields, by name, copied |
| a Class registered with `Register<T>` | userdata, the object itself |
| `LuauTable`, `LuauFunction`, `LuauThread`, `LuauBuffer`, `LuauUserdata` | the value itself |
| `LuauValue` | any value, its type known only as the program runs |
| `LuauNil` | nil |

- What a type converts by is `LuauType`, an Interface with Static Methods, which the package's
  own Extends give each type above. Its members take an opaque `LuauSlot`, whose Fields are the
  package's, so a program can name the Interface, as a constraint, but can't touch the stack
  through it.
- A number reads as an integer type only when it's whole and in the type's range, and as a
  `Float32` only when it's finite or a float's infinity or NaN, or else the read is a
  `LuauError` of `CONVERSION`, saying what it found: `expected Int32, got 2.5`. An `Int64` or
  `UInt64` past 2^53 is no number Luau holds exactly, so it's a `CONVERSION` error too, not a
  rounded one; a `LuauInteger` goes as Luau's 64-bit integer.
- A string that isn't well-formed UTF-8 reads as a `String` with U+FFFD for each byte that
  doesn't start well-formed UTF-8, as FutureBlitz's CStrings do, and as `LuauBytes` byte for
  byte.
- A `Map` copies: the table is read whole, or made whole. A table that holds itself, or is more
  than 256 deep, is a `CONVERSION` error, as Load's 256 levels are.
- An array can't implement `LuauType`, as FutureBlitz gives an array type no Statics yet
  ([What FutureBlitz can't do yet](#what-futureblitz-cant-do-yet)), so a `T[]` crosses through a
  table, `lua.NewTable(items)` and `table.ToArray<T>()`, or as a record's Field.
- `LuauRecord` is an Interface whose Static defaults convert through std's `Save` and `Load`, so
  a Struct or Class gets its conversion by listing it, `Struct Stats Implements LuauRecord`, and
  its Fields can be whatever Load takes: a `T?`, a `T[]`, a `Map`, an Enum by its name, another
  record, with std's attributes (`@Rename`, `@Skip`, `@Strict`, ...). A Field the table hasn't is
  its default, or its zero, as Load has it. A Luau table is an Array when its keys are 1 to its
  length and an Object when they're strings; an empty one is whichever its Field is, which the
  package finds by Load's error and its path. A record is data: a handle isn't a Field Load
  takes.
- `LuauValue` is any value, its `Kind` known as the program runs: a nil, boolean, number,
  integer, or string copied, and anything else kept as a handle is, with `AsBool()`,
  `AsNumber()`, `AsString()`, and the rest, and `Read<T>()`.
- Nullables and tuples: `GetOrNull<T>` gives a `T?`, Null for nil, and `Call2<A, B>` and its
  kin give results as tuples, since a `T?` and a tuple can't implement `LuauType` yet; a record's
  Field can be a `T?`.

### Globals and tables

```vb
Try lua.Set("speed", 4.5)
Local speed = Try lua.Get<Double>("speed")

Local player = Try lua.NewTable()
Try player.Set("name", "ada")
Try player.Set(1, "first")
Try lua.Set("player", player)
Print(Try player.Get<String>("name"), Try player.Length())

For entry In Try player.Entries<String, LuauValue>()
    Print(entry.0)
End For

Local stats = Try lua.Get<Stats>("stats")         // a LuauRecord, read by its Fields
```

- `lua.Get<T>(name)` and `lua.Set(name, value)` are the State's globals, or a script's own
  ([Sandboxing](#sandboxing-and-limits)); `Has(name)` says whether one isn't nil.
- A `LuauTable` has `Get<T>(key)`, `Set(key, value)`, `GetOrNull<T>(key)`, `Has(key)`,
  `Remove(key)`, `Length()` (raw, as rawlen gives it), `Entries<K, V>()` (each pair, as a
  `(K, V)[]`), `ToArray<T>()`, `ToMap<K, V>()`, `Metatable()` and `SetMetatable(table)`, and
  `Freeze()` and `IsFrozen`. `lua.NewTable()`, `lua.NewTable(items)`, and `lua.NewTable(entries)`
  make one. Its keys are Strings, integers, or any `LuauType`. `Get` and `Set` go through
  `__index` and `__newindex`, which can run code, so they throw; `RawGet` and `RawSet` don't
  run any, as Luau's rawget doesn't.
- sol2 writes `lua["speed"] = 4.5`. FutureBlitz's `[]=` takes one type of value, and isn't
  generic, so it can't take any value; `Set` is the way.

### Functions both ways

A FutureBlitz function becomes a Luau function, with its arguments read by their types and its
result given back by its type:

```vb
Function Damage(target: Player, amount: Int): Int
    ...
End Function

Function ReadSave(name: String): String Throws IOError
    ...
End Function

Try lua.SetFunction("damage", Damage)
Try lua.SetFunction("read_save", ReadSave)             // its IOError goes to the script
Try lua.SetFunction("add", Function(a: Int, b: Int) => a + b)
```

- `SetFunction(name, f)` and `lua.Wrap(f)`, which gives the `LuauFunction`, take a Func of 0 to
  8 parameters, each a `LuauType`, that gives nothing or one `LuauType`, and throws or doesn't,
  as fb 1.3.0's `Func(A): R Throws E` has it. A missing argument is nil, and an extra one is let
  go, as Luau's own functions do. A script that passes the wrong type gets an error worded as
  Luau's are: `chunk:3: invalid argument #2 to 'damage' (expected Int32, got string)`. A
  function that needs its State captures it `[Weak lua]`, which isn't a cycle.
- An error a FutureBlitz function throws is a Luau error in the script, whose message is its
  Message after where the function was called, as Luau's own errors are, `mods/doors.luau:3:
  save.dat doesn't exist`, and which a script's `pcall` catches. One the script lets go comes
  back out of the call that ran it as a `LuauError` whose `Cause` is that IOError, the object
  itself. A `LuauError` that comes up through the program's function from a call it made into
  Luau goes on as it was, its value and its message.
- `SetAsync(name, f)` takes a Func that gives a `Task<R>`, an Async Function's: the script's
  thread waits for the Task, as an Await does ([Threads](#coroutines-and-the-scheduler)).

A Luau function becomes a FutureBlitz one:

```vb
Local area = Try lua.Get<LuauFunction>("area")
Local a = Try area.Call<Double>(2.0, 3.0)                 // its first result, as a Double
Try area.Invoke(2.0, 3.0)                                  // no result
Local f: Func(Double, Double): Double Throws LuauError = Try area.ToFunc()
Local b = Try f(4.0, 5.0)
```

- `Call<R>(args...)` gives the first result as an R, with its arguments' types inferred, as
  `db.Query<User>(sql, id)` infers them; `Invoke(args...)` gives none; `Call2<R1, R2>` and
  `Call3<R1, R2, R3>` give tuples; `CallAll(args...)` gives a `LuauValue[]`. Each takes 0 to 8
  arguments.
- `ToFunc<A, B, R>()` gives a `Func(A, B): R Throws LuauError`, and `ToAction<A, B>()` a
  `Func(A, B) Throws LuauError`, which the program calls like any function value, and passes
  where one is wanted; their type arguments are written, as overloads that differ only by how
  many there are can't be told apart from the type expected.
- A handle keeps its function alive in the VM; `LuauFunction` is a Class, so the handle goes when
  its last reference does.

### Errors

```vb
Try
    Try lua.Run(source, "@mods/doors/init.luau")
Catch e As LuauError
    Print(e.Kind, e.Message)       // RUNTIME  mods/doors/init.luau:12: attempt to index nil
    Print(e.Traceback)
End Try
```

- `LuauError Extends Error`, with a `Kind`, a `LuauErrorKind`: `SYNTAX`, `RUNTIME`, `MEMORY`
  (past its memory limit, or out of memory), `TIMEOUT` (past its time budget), `CONVERSION` (a
  value that isn't the type asked for), `YIELD` (a yield where nothing can wait for it),
  `CLOSED` (a State closed while it ran), and `OTHER`.
- `Traceback` is Luau's, as `debug.traceback` writes it, taken where the error was raised, before
  Luau unwinds; `Value` is the error's own value, a `LuauValue`, since a script can raise a
  table; and `Cause` is the FutureBlitz error that started it, when one did.
- Misuse panics, at the caller's line, as std's does: a handle of a closed State, a handle of
  another State, or a State used inside a destructor. A panic ends the program, so nothing a
  script does can cause one.

### Registering types

```vb
Class Player Implements LuauObject
    Field health: Int = 100
    Field secret: Int                                  // never seen by a script

    @LuauAction Method Heal(amount: Int)
        health = Min(health + amount, 100)
    End Method

    @LuauExpose("hurt") Method Damage(amount: Int): Int
        health -= amount
        Return health
    End Method

    @LuauGet Method Health(): Int => health

    Method ToString(): String => "Player"
End Class

Try lua.Register<Player>()
Try lua.Set("player", player)
```

```lua
player:Heal(10)
print(player:hurt(5), player.Health, tostring(player))
player.Health = 1        -- error: Player's Health is read-only
```

- A Class that implements `LuauObject` crosses as itself, a userdata of the object, and
  `Register<T>()` says what a script reaches of it, by compile-time reflection, `For Method` over
  T, with a loop for each shape of Method it takes (0 to 7 arguments, a result or none, throwing
  or not). It's done once per State, before the first T crosses; crossing before is a panic that
  says so.
- Nothing is exposed unless it says so: a Method that gives a value crosses with `@LuauExpose`,
  one that gives nothing with `@LuauAction`, a Method of no arguments that gives a value is a field
  a script reads with `@LuauGet`, and one of one argument is what a script's assignment calls with
  `@LuauSet`; each takes a name, `@LuauExpose("hurt")`, for another name in Luau. A mod is
  untrusted code, so a member that isn't marked is one it can't reach. Reflection goes over
  Private Methods too, so a Private Method a program marks is exposed; that's its choice, made in
  the type's own file. Fields and Properties can't be marked yet, nor `@LuauExpose` take both
  kinds of Method, as FutureBlitz can't yet pick them in a loop
  ([What FutureBlitz can't do yet](#what-futureblitz-cant-do-yet)), so a field a script reads is a
  `@LuauGet` Method meanwhile.
- What reflection doesn't reach is added by hand, on what `Register<T>` gives, a
  `LuauTypeBuilder<T>`: `AddMethod(name, f)`, `AddGetter(name, f)`, `AddSetter(name, f)`, with a
  `LuauFunction` that takes the object first, and `AddOperator(LuauOperator.ADD, f)` for `__add`
  and the other metamethods, since reflection doesn't go over Operators yet.
- An object reads back as the type it crossed as: FutureBlitz can't yet keep an Interface's
  implementers to Classes, so an object is kept boxed by its type, and one that crossed as a
  `Player` isn't read as a Class it extends.
- Its metatable is locked: `getmetatable` gives "The metatable is locked", and `setmetatable`,
  `rawget`, and `pairs` on the object are errors. `__index`, `__newindex`, and `__namecall`, which
  makes `obj:Method()` one call, are the program's; `typeof` gives the Class's name, and
  `tostring` its `ToString` when it has one.
- `lua.Definitions()` gives a Luau definitions file of what's registered and set, the types of
  each member and function, for the type checker and for a mod's editor
  ([Type checking](#type-checking-and-diagnostics)).

### Userdata lifetimes

A program's object in Luau is a userdata whose memory holds an id; the State keeps the object
in a map by that id, which counts as one reference. So an object a script holds stays alive
while the script holds it, however many times it crosses, and goes when neither side does:

- The same object crossing again is the same userdata, found through a weak table of Luau's by
  its id, so `==` and table keys work in a script, as they do in FutureBlitz; once Luau has
  collected that userdata, the object crossing again is a new one, with a new id.
- When Luau's collector frees the userdata, its destructor queues the id; it can't release the
  object there, since the object's `Destroy` could call into Luau, which a destructor mustn't.
  The queue is drained, and its objects let go of, as each call into the State returns to
  FutureBlitz, and at `Collect()`.
- A handle to a Luau value, like a `LuauTable`, is a reference in Luau's registry, released in
  its Destroy.
- A cycle across the edge, an object that holds a `LuauTable` that holds the object's userdata,
  isn't seen by either collector, so it lives until the State closes, when the State lets go of
  everything. That's the one leak this design can have, and the guide says how to avoid it:
  hold a `Weak<T>` in the table's place, or a key, not the handle.
- A function given to Luau that captures the State strongly keeps it alive the same way, through
  the State's own map of the program's functions; one that captures `[Weak lua]` doesn't. A
  handle holds its State weakly, so a function that holds a handle doesn't.

### Vectors and buffers

```vb
Try lua.Set("origin", Vec3(0, 1, 0))
Local v = Try lua.Get<Vec3>("velocity")

Local data = Try lua.NewBuffer(256)              // a LuauBuffer
Try data.WriteUInt32(0, 7)
Local bytes = data.ToArray()
```

- Luau's vector is 3 Float32s, which is std.math's `Vec3`; the package builds Luau with its
  default vector, which native code needs.
- A `LuauBuffer` is a handle to a Luau buffer, with its `Count`, `Read(offset, count)` and
  `Write(offset, bytes)`, `ReadInt8` to `ReadFloat64` and their `Write`s, little-endian as Luau's
  buffer library has them, each within its bounds or a panic at the caller, and `ToArray()`.
  Luau's memory doesn't move, but it's the collector's, so the package gives no Pointer to it.

### Coroutines and the scheduler

```vb
Local lua = LuauState()
Try lua.Run(source)                                   // defines main(), which waits
Local script = Try lua.Spawn(Try lua.Get<LuauFunction>("main"))
Local done = script.Start()                           // a Task, on the scheduler running it

While window.IsOpen()
    window.Draw()
    Scheduler.Main.Step()                             // the script goes on here
End While
```

```lua
function main()
    door:Open()
    task.wait(2)            -- the program's loop goes on meanwhile
    door:Close()
    local text = fetch("https://example.com")   -- an Async Function's Task, waited for
end
```

- `lua.Spawn(f)` makes a `LuauThread` that will run `f`. `thread.Start()` runs it as an Async
  Function would: it resumes the thread, and each time the thread yields to wait, the Async
  code under it Awaits what it waits for, then resumes it. Its Task is done when the thread
  ends, and throws its error. It's on the scheduler running the code, so `s.Run(...)` moves it
  to a Scheduler of the program's own, which a pause stops, and whose `Tick(dt * speed)` scales
  its waits.
- The `task` library, which the State opens when `LuauOptions.Tasks` is on, as it is by default,
  is Roblox's: `task.wait(seconds)`, `task.spawn(f, ...)`, `task.defer(f, ...)`,
  `task.delay(seconds, f, ...)`, and `task.cancel(thread)`, so a mod's author writes what they
  know. `task.wait` is `Await Wait(seconds)` on the thread's scheduler.
- A function from `SetAsync` that a script calls on a thread yields it until its Task is done,
  then resumes it with the result, or raises the Task's error in it.
- A yield where nothing can wait, as in `lua.Run`, which runs on the program's own stack, or
  inside a metamethod, is a `LuauError` of `YIELD`, saying so.
- `thread.Resume(args...)` and `thread.Status` drive a thread by hand, as `coroutine.resume`
  does, for a program with a loop of its own.

### Sandboxing and limits

```vb
Local options = LuauOptions()
options.MemoryLimit = 64 * 1024 * 1024          // bytes, for the whole State
options.TimeBudget = 50.Milliseconds            // each call into the State, or each resume
Local lua = Try LuauState(options)

Try lua.SetFunction("spawn_item", SpawnItem)    // what mods are given
Try lua.Register<Door>()
lua.Freeze()                                    // the shared globals are read-only from here

Local doors = Try lua.NewScript("mods/doors")   // a LuauScript: its own globals
Try doors.Run(source, "@mods/doors/init.luau")
```

- A State is sandboxed as Luau's own `luaL_sandbox` and `luaL_sandboxthread` make it:
  `Freeze()` makes the globals and every library read-only, and each `LuauScript` has globals of
  its own, its `_G`, which read through to the frozen ones by a locked metatable, so one mod can't
  change what another sees, or reach the shared table. A State that's never frozen freezes at its
  first `NewScript`. A script's code is loaded on a thread whose globals are the script's, as Luau
  resolves a chunk's globals as it loads from the loading thread's, not from the environment it's
  given.
- The libraries, `LuauOptions.Libraries`, a `@Flags` Enum, are by default Luau's safe ones:
  the base library, `coroutine`, `table`, `string`, `math`, `utf8`, `bit32`, `buffer`, `vector`,
  `integer`, `os` (which in Luau is only `clock`, `date`, `difftime`, and `time`), `debug`
  (only `info` and `traceback`), and `task`. Left out by default: `getfenv` and `setfenv`, which
  reach other functions' environments and turn off Luau's fast paths; `loadstring`, which a
  program can turn on, and which never takes bytecode; and `collectgarbage`, which is only
  `collectgarbage("count")` unless the options say `FULL`, as Luau's conformance tests need.
  Nothing in Luau reads or writes a file, starts a process, or opens a socket: a script does
  that only through what the program gives it.
- `print` goes to `LuauOptions.Print`, a `Func(String)` of each line, its arguments by tostring
  with tabs between, by default std's `Print`, since Luau's own writes to the C runtime's stdout.
- The memory limit is the State's allocator's: past it, an allocation fails, and the script gets
  Luau's `not enough memory`, which reaches the program as a `LuauError` of `MEMORY`. The State
  stays usable: what was freed is free. `lua.MemoryUsed` says how much it holds; a script's own
  share isn't kept, as its code runs on the State's threads, which Luau's memory categories count
  by.
- The time budget is Luau's interrupt, which Luau calls at each loop's back edge, each call, and
  each return. Once a call is past its budget, the interrupt raises `TIMEOUT` there, and again at
  each interrupt after, so a script's own `pcall` can't swallow it, until the call returns. A C
  function that runs long, like a string pattern that backtracks, or a function of the program's,
  delays it to that function's end, which Luau's docs say too.
- Deep recursion: Luau stops its own at 200 C calls; the package stops a call that goes from
  FutureBlitz into Luau and back more than 64 times deep, with `RUNTIME`, before the native
  stack runs out.

### require

```vb
options.Modules = Function(path: String): String? => Try? modFiles.ReadText(path + ".luau")
```

```lua
local doors = require("./doors")
local util = require("@shared/util")
```

- `require` is Luau's own, `Luau.Require`, with Luau's rules for paths: `./` and `../` from the
  requiring module, `@self`, and aliases from a `.luaurc` beside the modules. The shim walks the
  paths itself, and asks the program only for a module's source, by its path, through
  `LuauOptions.Modules`, and for a folder's `.luaurc`, so a mod can require only what the program
  gives it, from wherever the program keeps it: a pak, a folder, or memory.
- Each module runs once per State, in a thread of its own with its own globals, as the `luau`
  CLI does, and gives one value, which `require` caches. A module that errors errors in each
  `require` of it; one that yields while it loads is a `YIELD` error.
- With no `Modules`, `require` is an error that says the program gives no modules.

### Bytecode

```vb
Const KEY = EmbedText("bytecode.key")              // the program's own secret
Local code = Try LuauBytecode.Compile(source, "@rooms/door.luau")
Local bytes = code.Save(KEY)                        // shipped with the game

Local loaded = Try LuauBytecode.Load(bytes, KEY)    // checks its tag, then trusts it
Try lua.Run(loaded)
```

- Luau runs bytecode as it's given: its loader checks nothing, and its security policy says
  bytecode from anywhere but its own compiler has no guarantees, and should be signed. So
  `Save` writes a container: a magic, the Luau release and bytecode version it was compiled by,
  the compile options, the bytecode, and an HMAC-SHA256 of all of it by the program's key, std's
  `std.crypto`'s. `Load` checks the tag first, in constant time, and gives a `LuauError` of
  `OTHER` for a wrong one, a container of another Luau, or anything that isn't one, before Luau
  sees a byte.
- A mod is source, always. A program that loads a mod's bytecode would be trusting the mod with
  its process, which the package doesn't offer.
- The compile is ahead: a game's build step (FutureBlitz's `[build]`) can compile its own
  scripts, and Embed them, or write them into its paks.

### Native code generation

- `LuauOptions.NativeCode`: `OFF`, `MARKED` (the default: modules that start with `--!native`,
  and functions with `@native`), or `ALL`. It's Luau's CodeGen, on x64 with AVX and on arm64;
  where it isn't supported, code runs in the interpreter, the same, and `LuauState.NativeCode
  Supported` says so.
- It's made per State when the State is, before any code loads, as Luau needs, and compiles each
  chunk as it loads. A registered type's name is given to the compiler as a userdata type, so
  typed code over it compiles natively.

### Type checking and diagnostics

```vb
Local checker = LuauChecker(Function(path: String): String? => Try? modFiles.ReadText(path))
checker.AddDefinitions(lua.Definitions())       // the API the game gives mods
For d In Try checker.Check("mods/doors/init")
    Print("{d.Module}:{d.Line}:{d.Column}: {d.Severity} {d.Code} {d.Message}")
End For
```

- `LuauChecker` is Luau's `Frontend` with its new solver, the one `luau-analyze` uses by
  default: it reads modules through the program's function, follows their `require`s as the VM
  does, and gives each type error and lint warning as a `LuauDiagnostic`: its module, line and
  column (from 1), end, severity, Luau's code and name for it, and message, as Luau writes it.
- `AddDefinitions(text)` adds a definitions file, Luau's `declare` syntax, which
  `lua.Definitions()` writes for what a State gives scripts, so a mod is checked against the
  game's real API. A mode, `STRICT`, `NONSTRICT`, or `NOCHECK`, is each module's `--!` line, or
  the checker's default.
- `Check` takes a time limit per module, as Luau's does, so a module that makes the checker work
  too long gives a diagnostic saying so, not a hang.
- It's for mod tooling: a game's mod checker, its editor support, or a command. Running mods
  doesn't need it, but it's in the same library, so a game that checks mods as it loads them can.

## The shim's C API and its rules

The library exports only `fbl_<area>_<verb>` functions, whose header for each area is
`native/<area>/fbl_<area>.h`, and whose bindings fb-bindgen writes into `luau/c/`. The areas:

| Area | What's in it |
|---|---|
| `core` | `fbl_core_version`, `fbl_core_error` (this thread's last error), the flags set once |
| `state` | make and close a State, its options, memory, collection, freezing, the released-id queue |
| `load` | compile source, check and load a bytecode container, compile options |
| `frame` | a call's values: a frame of slots that the FutureBlitz side fills and reads by index |
| `value` | push and read each kind of value in a slot, with its type checked |
| `table` | get, set, raw get and set, length, next, metatables, freeze |
| `call` | call a function in a frame, with its status, results, and error |
| `callback` | the one Callback the FutureBlitz side gives, and how a function's id reaches it |
| `udata` | register a type's metatable, push an object's userdata, its id, the destructor queue |
| `thread` | make, resume, yield, status, and reset a thread |
| `buffer` | make, read, write, and copy a buffer |
| `require` | the module callback and Luau.Require's configuration over it |
| `codegen` | whether it's supported, and turning it on |
| `check` | Luau's Frontend, its definitions, and its diagnostics |

The rules:

1. **Nothing unwinds across it.** Luau is built as C++, as it is by default, where a Luau error is
   a C++ exception of Luau's own, and FutureBlitz's code has no unwind tables, so an exception
   that reached one of its frames would end the program. Every exported function is a
   `noexcept` function whose body is in a `try` that catches everything, and every Luau call
   that can raise, which is most of them, a metamethod or an allocation, runs under
   `lua_pcall` or `lua_cpcall`, never bare. A status comes back: `FBL_OK`, or an error, whose
   text and traceback the error area gives.
2. **A Luau error is raised only from the shim's own frames.** When Luau calls a FutureBlitz
   function, the shim's C function calls the FutureBlitz Callback, which returns a status, and
   only after it has returned does the shim raise the error, with `lua_error`, from its own
   frame. A yield works the same way: the Callback returns `YIELD`, and the C function returns
   `lua_yield`.
3. **Everything is checked.** Luau's own API checks nothing in a release build, so the shim
   checks each handle (a slot, a generation, and a kind, in a `uint64_t`, as fbengine's are),
   each slot index against its frame, each value's type before reading it, each table before
   indexing it, and the stack's room before pushing (`lua_checkstack`, which can't raise). A
   wrong one is an error, which the FutureBlitz side turns into a panic where it's misuse.
4. **Who owns what.** A State owns its VM, and every handle's registry reference; a handle is
   the State's, and is only good with it. A string the shim gives is the shim's, good until the
   next call on that frame, so the FutureBlitz side copies it at once. Memory the FutureBlitz
   side gives, a source or a buffer, is borrowed for the call. Compiled bytecode is the shim's
   until it's copied out. The FutureBlitz side's objects are its own, which the shim names only
   by id.
5. **Threads.** A State is used on one thread at a time, and the FutureBlitz side keeps it on the
   thread that made it. The Callback is FutureBlitz code that C calls, which FutureBlitz checks
   as work on another thread, so it reaches its State's functions and objects through a
   ThreadLocal Global, a map from a State's id to it, which is its own thread's: the State's
   thread's, since the shim only calls it from inside a call that thread made. Luau's flags are
   set once, under a `std::call_once`, before the first State; the interrupt, which the time
   budget uses, reads a clock and a deadline that only its own State's thread writes. Nothing
   in the package is Sendable.
6. **Destructors don't call out.** A userdata's destructor runs during Luau's collection, where
   it mustn't touch the VM, so it only queues the object's id.

## Building and shipping

### Luau, pinned

Luau is fetched by CMake's FetchContent at its release tag `0.746` (2026-11-07, commit
`12a6ba75`), with `SYSTEM`, and built static and position-independent into `luau_native`, with
`LUAU_BUILD_CLI` and `LUAU_BUILD_TESTS` off: its `Luau.VM`, `Luau.Compiler`, `Luau.CodeGen`,
`Luau.Analysis`, `Luau.Config`, and `Luau.Require`. It's built as C++ with exceptions, Luau's
default, so `LUAU_EXTERN_C` stays off: that option would switch Luau's errors to `longjmp`,
which skips C++ destructors, and the Compiler and Analysis throw C++ exceptions either way.
The library is C++ inside, but its header is C.

A new Luau is taken by hand: the tag changes in one line, the conformance tests run again
through the package, and decisions.md says what changed.

### Luau's flags

Luau's features are behind flags, which are process globals and all off by default, while the
`luau` CLI turns on every flag whose name starts with `Luau` but its experimental ones. Without
them, a host gets an older Luau than its own CLI: no `integer` library, no `1i` literals. The
shim does as the CLI of its release does, once, before the first State, and decisions.md lists
the flags, so scripts behave as `luau` runs them. No `Debug` flag is turned on.

### A library beside the program

`luau/project.toml` has:

```toml
[native]
luau_native = { dynamic = "native/{target}/luau_native" }
```

The library is dynamic, a DLL beside the program, as fbengine's are, not a static archive.
FutureBlitz links a program with its own C runtime pieces and no C++ runtime, and Luau is C++,
so an archive would need each target's C++ runtime linked into every program, built by the
compiler the program's is. A shared library carries its C++ runtime inside it, linked statically
(`-static-libstdc++ -static-libgcc` on Linux, `-static` on Windows), exports only its `fbl_`
functions, and loads nothing but the OS's own libraries, which the export check tests. fb copies
it beside the program, which finds it by `$ORIGIN` on Linux and `@loader_path` on macOS, as spec
11 says.

Each target's library is built in CI, never checked in, and goes into the package's
`native/<target>/` before `fb publish`, for `windows-x64`, `linux-x64`, `linux-arm64`,
`macos-arm64`, and `macos-x64`. Linux builds in manylinux_2_28, glibc 2.28, as fb's programs
do. Windows builds with MSYS2's UCRT64 GCC, whose C++ library is built for the UCRT, as fb's
programs are: fbengine builds with MINGW64 and `-mcrtdll=ucrt`, which mismatches iostreams'
`mbstate_t` and is why fbengine has no iostreams in a DLL, and Luau's Analysis uses
`std::stringstream`. L0's first Windows build tests a stringstream in a Release DLL to prove it.

### Notices

`luau/LICENSE` is the Apache-2.0 LICENSE, byte for byte. `luau/notices/` has the texts for what's
in the library: `luau.txt`, Luau's MIT License with its copyright lines, Roblox's and Lua.org's,
and on Windows `winpthreads.txt`, for MinGW's runtime. The package's README names them, and
`tools/check_notices.py` fails the build when the library links something without one, as
fbengine's does.

## The repository

```
CMakeLists.txt           the library, Luau, and the C++ tests
cmake/                   native.cmake (the library: exports, runtime, the copy into luau/native),
                         luau.cmake (Luau's fetch and options), warnings.cmake
native/<area>/           fbl_<area>.h and its .cpp files; native/fbl.h includes every area's
luau/                    the package: project.toml, README.md, LICENSE, notices/
  *.fb                   the public API, by area: state.fb, values.fb, tables.fb, functions.fb, ...
  c/fbl_<area>.fb        the bindings, which tools/bindgen.sh writes, Package-only
  tests/                 the package's tests, which `fb test luau` runs
  native/<target>/       the built library, never checked in
tests/                   the shim's C++ tests (Catch2), the stability tests, and their data
tests/conformance/       the FutureBlitz program that runs Luau's conformance scripts
fuzz/                    the fuzzers, over the C API
examples/                programs on the package, which ctest runs
tools/                   verify.py, bindgen.sh, check-exports.sh, check_notices.py, gen_arity.py
docs/                    plan.md, decisions.md, guide.md
.github/                 workflows, the install-fb action, templates
```

`tools/gen_arity.py` writes the `.fb` code that's the same for each arity, `SetFunction`'s 0 to
8 parameters, `Call`'s, and `Register`'s loops, into files that say they're generated, and its
`--check` fails when they differ, as the bindings' does.

## Tests

- **The shim, in C++:** Catch2, a test for each function of the C API: what it does, each error
  it gives, and that no misuse crashes, through a test library built from the same sources, so
  tests reach what the DLL doesn't export. Each finding gets a test.
- **The package, in .fb:** `luau/tests/`, which `fb test luau` runs, a test for each public
  declaration and each error, and the `# Examples` in its `///` docs, which run as tests too.
- **Luau's conformance tests through the package:** the 59 scripts in Luau's `tests/conformance`,
  at the pinned release, run by a FutureBlitz program through the package's API, each in a
  State with the options they need (`collectgarbage` in full, `loadstring` on, `print` silent)
  and checked to return `"OK"`, as Luau's own runner does. About 30 need nothing else; those
  that need host functions of Luau's runner (`userdata`, `vector`, `cyield`, `interrupt`,
  `coroutinehost`, ...) get them written in FutureBlitz over the package where the package can
  do what the runner does, and the rest are listed, each with why it can't run here (a debugger
  callback, coverage counters, the internals they read).
- **Fuzzing,** with libFuzzer, on Linux with clang:
  - the loader: arbitrary bytes as source, compiled and run in a State with a small memory limit
    and time budget, which must give a status, never crash, hang past its budget, or leak;
  - bytecode containers: arbitrary bytes to `Load`, which must reject each one that isn't a
    container signed by its key before Luau sees it, and the compiler's own output for fuzzed
    source, which must load and run. Raw bytecode isn't fuzzed into Luau, since Luau trusts its
    bytecode by design and the package never gives it untrusted bytes;
  - the C API: a sequence of calls with arbitrary arguments, handles, and indexes, which must
    each give a status, never crash, since its checks are what stand between misuse and Luau's
    unchecked API;
  - the checker: arbitrary source to `LuauChecker`, within its time limit.

  Their corpora start from the conformance scripts, and each crash becomes a test.
- **Leaks and sanitizers:** the C++ tests under ASan and UBSan, with LeakSanitizer, and under
  TSan for States on several threads at once; every State's memory back to 0 at its close, by
  its allocator's count; and FutureBlitz's own leak report, which `fb test` fails on, for the
  objects and functions a State held.
- **A soak:** a program that makes and closes States, loads scripts, calls both ways, makes and
  drops userdata, tables, threads, and buffers, and runs scripts that hit their memory limits and
  time budgets, for 30 minutes, sampling its memory, the shim's counts, and FutureBlitz's object
  counts each minute, and failing when they grow: a slope past a few bytes per iteration, after
  the first minutes, catches a slow leak a short test misses.

`python tools/verify.py` runs what a landing needs and prints "everything passed": the Debug
build, ctest (the C++ tests, `fb test luau`, the conformance program, the examples, and the
tools' tests), the export check, `tools/bindgen.sh --check`, `tools/gen_arity.py --check`, the
notices check, clang-format 19.1.5, clang-tidy on what changed since master, `fb fmt --check`,
and `fb doc luau --check`. Its flags are fbengine's: `--build`, `--base`, `--no-build`,
`--no-tidy`, and `--only`.

## CI

- **native.yml**, reused by the others: the Release library on the five targets, Windows on
  `windows-2025` in MSYS2's UCRT64, Linux on `ubuntu-22.04` and `ubuntu-22.04-arm` in
  manylinux_2_28, and macOS on `macos-15` and `macos-15-intel`; the export check; and each
  target's `native/<target>/` as an artifact.
- **ci.yml**, on each push to master and each pull request: native.yml; then on each target,
  without a compiler, `fb test luau` against that Release library and the conformance program;
  a Debug build and ctest on each target; and the format job (clang-format, `fb fmt --check`,
  the bindings' and arity code's checks, `fb doc --check`). fb comes from the `install-fb`
  action at the version `FB_VERSION` pins, 1.3.0, as fbengine's does.
- **stability.yml**, nightly and by hand: ASan with UBSan, and TSan, over the stability tests;
  each fuzzer for 10 minutes; and the soak.
- **targeted.yml**, by hand: a ctest pattern on the targets it names.
- **publish.yml**, by hand, only on the user's yes: on a tag `v<version>`, native.yml, every
  target's library into the package, a check that all five are there, `fb publish` by trusted
  publishing to the registry it's given, staging or futureblitz.dev, and a check that a new
  project can `fb add luau` and run a script on each target.

The coordinating session watches CI; the lane doesn't.

## Docs

- **README.md:** what the package is, a short program, how to add it, what it needs, its
  license, and the notices.
- **docs/guide.md:** the guide: a State and running code, values, tables, functions both ways,
  errors, registering types and what to expose, lifetimes and the cycle to avoid, vectors and
  buffers, threads on the scheduler, sandboxing a game's mods, require, bytecode, native code,
  and checking mods, each with a program that runs. One section is for a game's modding: the
  shape SCP:CB X's mods take, as an example any game can follow.
- **`///` on everything public,** with `//!` on each module, so `fb doc luau --check` passes in
  verify.py and CI, and `# Examples` that run as tests.
- **The headers' comments,** in C, say what each function does and gives on failure, as
  fbengine's do; docs/plan.md's [rules](#the-shims-c-api-and-its-rules) are the shim's.

## What FutureBlitz can't do yet

Each of these is shown by a small program and sent to the coordinating session for the fb lane
when its milestone reaches it; the package's way around it is plain, and goes when the gap does.

1. **A `T?` or a tuple can't implement an Interface by an Extend,** as `Extend T? Implements
   LuauType Where T: LuauType` would. So `Get<Int?>` can't be written, and a registered Method
   can't take an `Int?`; the package has `GetOrNull<T>`, `Call2` and its kin, and a Method that
   takes a `T?` isn't one Register can expose.
2. **A Func type can't implement an Interface,** so `Set("add", f)` can't take a function as one
   value among the others; `SetFunction` is the way, an overload for each arity, which
   gen_arity.py writes.
3. **Reflection doesn't go over Operators,** so `Register<T>` can't expose them; the builder's
   `.Operator` does it by hand.
4. **`[]=` takes one type of value and isn't generic,** so `lua["speed"] = 4.5`, sol2's way, isn't
   possible; `Set` is.
5. **fb-bindgen writes every declaration Public, without `///` docs,** so `fb doc --check` would
   fail on the bindings, which a program mustn't see anyway. bindgen.sh makes them Package, and
   its check compares what it makes; an fb-bindgen option for the visibility would do it at the
   source.
6. **Proved at L1:** a Callback's code can reach a ThreadLocal Global holding Classes and Func
   values, and call them, on fb 1.3.0, which the package's whole calling path rests on.
7. **An array type has no Statics,** so `T[]` can't implement `LuauType`, whose members are
   Static Methods, as `Extend T[] Implements LuauType Where T: LuauType` would; generic code that
   converts by a type's Statics can't take arrays. Arrays cross through tables and records
   meanwhile (fbengine-notes/luau/array_statics.fb shows it).
8. **An Error can't say its own Class's name as the program runs,** which the runtime knows, as
   an uncaught error's message shows it, so an error crossing into Luau is its Message alone, not
   `IOError: ...`.
9. **A bug: beside an overload that takes a `Func()`, a lambda that throws an `IOError` fits no
   overload,** though `Func() Throws Error` alone takes it, and a named Function that throws
   does. A lambda given to `SetFunction` says `Throws Error` meanwhile
   (fbengine-notes/luau/throws_overload.fb shows it).
10. **A Field or Property loop can't pick by an attribute,** only a Method loop can, so a loop's
    Where lines on F hold for every Field, and `Register<T>` can't expose only the Fields marked
    `@LuauExpose`; field-like members are `@LuauGet` and `@LuauSet` Methods meanwhile
    (fbengine-notes/luau/field_pick.fb).
11. **An Interface can't say what implements it is a Class** (no `Where Self: Class` in a default),
    so `LuauObject`'s defaults can't keep an object as an `Object`; it's boxed by its type, and
    reads back only as that type.
12. **A bug: a `For Method` loop of `Func(): R` binds a Method that gives nothing, with R as
    nothing,** so a Where line on R fails for it, and a loop over the Methods that give a value
    can't be written beside those that don't; `@LuauAction` marks the second kind meanwhile
    (fbengine-notes/luau/void_result.fb).

## Milestones

Each milestone is steps small enough to review in one sitting, each on a branch `dev/<step>` off
master, verified with verify.py before it's reported, written up in fbengine-notes/from-luau.md
(what was done, how it was verified, what isn't tested), reviewed by the coordinating session,
and landed by it. Every finding gets a test.

### L0: setup

The repository builds an empty library on every target and tests it.

- CMakeLists.txt, cmake/native.cmake and luau.cmake: Luau 0.746 fetched and built, the
  `luau_native` library with `fbl_core_version` and `fbl_core_error`, exporting only `fbl_`, its
  C++ runtime inside, copied into `luau/native/<target>/`; the flags set once.
- The package: project.toml, `LuauLibraryVersion()` and `LuauVersion()`, the library's version
  and Luau's, its bindings, a test that the library is the package's version, the README,
  LICENSE, and notices.
- tools/: verify.py, bindgen.sh, check-exports.sh, check_notices.py. gen_arity.py comes with L3,
  the first code it writes.
- Catch2 and a first test; a stringstream in the Release DLL on Windows.
- .github/: install-fb, native.yml, ci.yml, targeted.yml.

Done means: verify.py passes; CI is green on the five targets, `fb test luau` running against
each Release library; the export and notices checks fail when they should, by their tests.

### L1: a State, running code, and errors

- The callback spike first: a C function calls the package's Callback, which reaches a closure
  through the ThreadLocal map and calls it, on fb 1.3.0, with a test; gap 6 settled.
- The shim's `state`, `load`, `frame`, `call` (no callbacks yet), `ref`, and `core`: a State with
  its allocator and memory limit, compile and load, a protected call, errors with their
  tracebacks.
- `LuauState`, `LuauOptions` (memory limit; the rest as they come), `Run`, `Evaluate<T>`, `Load`,
  `Close`, globals' `Get` and `Set`, `LuauType` for the numbers, Bool, and String, `LuauError` and
  its kinds, `LuauCompileOptions`.

Done means: source runs and gives results; a syntax error, a runtime error, and running out of
memory each give their `LuauError` with the line and the traceback; a closed State's handle
panics; the C tests cover every status of each function; nothing leaks.

### L2: values and tables

- `LuauType` for `LuauBytes`, `LuauInteger`, `LuauNil`, `LuauValue`, `LuauTable`,
  `LuauFunction`, and `Map<K, V>`; integers read by the number types.
- `LuauTable`: Get, Set, GetOrNull, Has, Remove, raw access, Length, Entries, ToArray, ToMap,
  metatables, Freeze; `NewTable` of items and entries; `LuauRecord` through Load and Save.

Done means: every row of the values table crosses both ways, with a test of each conversion
error; tables that hold themselves, or go too deep, are errors, not hangs.

### L3: functions both ways

- tools/gen_arity.py and its check in verify.py and CI.
- The shim's `callback` area: the one Callback, Luau functions that call it, the numbers of
  those Luau collects, where a call came from; the error's value.
- FutureBlitz to Luau: `SetFunction` and `Wrap` for 0 to 8 parameters, with and without
  results, throwing and not, generated; argument errors in Luau's words; a FutureBlitz error as a
  Luau error and back as the same object.
- Luau to FutureBlitz: `LuauFunction`, `Call`, `Invoke`, `Call2`, `Call3`, `CallAll`, `ToFunc`,
  `ToAction`.
- Handles hold their State weakly.
- Errors both ways with tracebacks; the nesting limit.

Done means: each arity and shape works both ways; an error crosses out and back in and out again
with its Cause and its traceback; deep recursion across the edge is an error, never a crash.

### L4: registering types and userdata

- The shim's `udata`: a registered type's locked metatable, an object's userdata by id, its weak
  table, the destructor's queue; a `__namecall` that gets the method's name.
- `LuauObject`, `Register<T>` by reflection, `@LuauExpose`, `@LuauAction`, `@LuauGet`,
  `@LuauSet`, `LuauTypeBuilder<T>`, `__namecall`, `__type`, `__tostring`.
- Lifetimes: one userdata per object, release after collection, Close releasing everything.

Done means: a registered Class's exposed members work from a script, and its others can't be
reached by any means a test knows (`getmetatable`, `rawget`, `pairs`, `debug.info`); objects
live exactly as long as either side holds them, which a test counts; the cycle across the edge
is freed by Close.

### L5: vectors and buffers

- `Vec3` as Luau's vector, and the `vector` library; `LuauBuffer` and its reads and writes of
  bytes and of each number type, little-endian as Luau's buffer library has them, each within its
  bounds or a panic; `lua.NewBuffer(size)` and `NewBuffer(bytes)`.

Done means: both cross both ways, with bounds checked on every access.

### L6: sandboxing and limits

- `Freeze`, `LuauScript` and its own globals, the libraries by `LuauOptions.Libraries`, `print`
  to the program, `loadstring`, `getfenv` and `setfenv`, and `collectgarbage` as options.
- The time budget by Luau's interrupt, sticky past a `pcall`.

Done means: tests of escapes fail, each one a test: a script changing another's globals, a
library, the string metatable, or a frozen global; reaching `getfenv`; an infinite loop, one in
a `pcall`, one in a metamethod, and one in a coroutine, each ending at its budget; a script
eating memory, which gets `MEMORY` while the State goes on.

### L7: threads on the scheduler

- The shim's `thread`; `LuauThread`, `Spawn`, `Start`, `Resume`, `Status`; the `task` library;
  `SetAsync`; YIELD errors where nothing can wait.

Done means: `task.wait` waits on the main scheduler and on a Scheduler of the program's own,
whose pause and time scale it follows; an Async Function's Task is waited for and its error
raised in the script; a thread's Task gives its error; a State closed with threads waiting lets
them go.

### L8: require

- Luau.Require over the program's `Modules` function: paths, `@self`, `.luaurc` aliases, the
  cache, errors.

Done means: Luau's require rules hold, by tests of each; a module can't reach a path the program
doesn't give.

### L9: bytecode

- `LuauBytecode`: Compile, Save with its tag, Load with its checks; `Run(bytecode)`; an example
  of a build step that compiles a game's scripts.

Done means: a container round-trips; a changed byte, another key, another Luau, and arbitrary
bytes are each rejected before Luau sees them.

### L10: native code generation

- `LuauOptions.NativeCode`, CodeGen per State, userdata types for the compiler.

Done means: the conformance scripts pass with `ALL` on each target that supports it; a test
shows code ran natively where it's supported, and the same results where it isn't.

### L11: the type checker and diagnostics

- The shim's `check`: Luau's Frontend over the program's reader, definitions, lint, time limits.
- `LuauChecker`, `LuauDiagnostic`, and `lua.Definitions()` from what's registered and set.

Done means: a mod with type errors gives each, with its place and code, as `luau-analyze` gives
them for the same files; `Definitions()` of a State checks a script that uses it without errors,
and one that misuses it with the right ones.

### L12: the guide, examples, and docs

- docs/guide.md, the examples (a REPL, a modded toy game, a build step for bytecode), the
  README, `///` docs read again whole.

Done means: every example runs under ctest; the guide's programs run; `fb doc luau --check`
passes.

### L13: the stability pass

- The fuzzers, the sanitizer runs, the leak checks, the soak, and the conformance scripts
  through the package, run long, each finding fixed with a test.

Done means: each fuzzer has run 10 minutes nightly without a finding for a week of runs, the
sanitizers are clean, the soak's slopes are flat, and every conformance script that can run
through the package passes, with the rest listed and why.

### L14: the first release

- publish.yml, a dry run of `fb publish` against a folder registry in ctest, the version, and
  draft release notes.

Done means: everything above is on master with CI green on the five targets; the user has the
draft notes, and the tag, the release, and publishing wait for the user's yes.

## Open questions

Each has what this plan does unless the user says otherwise; decisions.md records the answer.

1. **Names: `LuauState` and the rest, prefixed, or plain names under `Import luau As Lua`?**
   Recommended: prefixed. `Import luau` is how packages are used, and `Thread`, `Function`, and
   `Buffer` are taken.
2. **What crosses: only members marked `@LuauExpose`, or every Public member, with
   `@LuauHide`?** Recommended: marked only. Mods are untrusted, reflection can't tell Public
   from Private, and a member a program adds later shouldn't reach mods unless it says so.
3. **Luau's flags: as the pinned `luau` CLI has them, or Luau's defaults?** Recommended: the
   CLI's, so a mod's author gets the Luau they run at home, `integer` and all.
4. **Bytecode: signed with the program's key, or loaded as it's given?** Recommended: signed, as
   Luau's security policy asks of bytecode that crosses a file system.
5. **The sandbox's libraries:** the list in [Sandboxing](#sandboxing-and-limits), without
   `getfenv`, `setfenv`, `loadstring`, and a full `collectgarbage`. Recommended as listed.
6. **The `task` library, Roblox's names, in every State by default?** Recommended: yes, so a
   modder writes what they know; a program can turn it off.
7. **The type checker in the one library**, which makes it a few MB bigger, measured at L11, or
   left out of a build that doesn't need it? Recommended: in the one library, as one package
   has; L11 says how big it is, and if it's past 10 MB, that comes back as a question.
8. **The first release's version:** `0.1.0`, while the gaps above stand, or `1.0.0`?
   Recommended: `0.1.0`, then 1.0 when the API stops changing.
