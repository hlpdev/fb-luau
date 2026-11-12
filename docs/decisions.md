# Decisions

Why the luau package is as docs/plan.md has it, newest last. A decision the user made says so,
with its date; the rest are the lane's, which the user can change.

## Before L0 (2026-11-11)

- **One package, `luau`, over one library, `luau_native`.** Not a set of packages: the type
  checker is in it too (the user, 2026-11-11).
- **Two layers:** a C++ shim whose C API is safe to call with anything, where no Luau error or
  C++ exception gets out, and a typed FutureBlitz API over it, in sol2's style, where Luau's
  stack is never seen (the user, 2026-11-11, with the rest of the design's outline: handles,
  functions both ways, types by reflection, sandboxed by default, the scheduler, and bytecode,
  native code, and the type checker later).
- **Luau 0.746, pinned by its tag**, the latest release when the plan was written (2026-11-07),
  built as C++ with exceptions, Luau's default. `LUAU_EXTERN_C` stays off: it makes the VM's
  errors `longjmp`s, which skip C++ destructors, while the Compiler and Analysis throw C++
  exceptions anyway.
- **A DLL beside the program, not a static archive:** FutureBlitz links programs without a C++
  runtime, and Luau is C++. As fbengine's are, the library carries its runtime inside and
  exports only its `fbl_` functions.
- **Windows builds with MSYS2's UCRT64**, not MINGW64 with `-mcrtdll=ucrt` as fbengine does:
  Luau's Analysis uses `std::stringstream`, and fbengine found iostreams' `mbstate_t` differs
  between a `_UCRT` build and MINGW64's static libstdc++. UCRT64's libstdc++ is built for the
  UCRT. L0 tests it.
- **Luau's flags as the pinned `luau` CLI sets them**, once, before the first State, so scripts
  run as they do under `luau`. Provisional until the user answers plan question 3.
- **Names are prefixed `Luau`**, since `Thread`, `Function`, and `Buffer` are taken and `Import
  luau` brings names in unqualified. Provisional until the user answers plan question 1.
- **Only members marked `@LuauExpose` cross**, since mods are untrusted and reflection goes over
  Private members too. Provisional until the user answers plan question 2.
- **Bytecode is loaded only from a container signed with the program's key**, since Luau's
  loader trusts every byte. Provisional until the user answers plan question 4.
- **Numbers:** FutureBlitz's numbers cross as Luau's numbers, and a value Luau can't hold exactly
  is an error, not a rounded number; `LuauInteger` is Luau's 64-bit integer.
- **The callback path:** C calls one Callback of the package's, which finds its State through a
  ThreadLocal Global by the State's id, since a Callback can't capture and its code is checked as
  work on another thread. L1 proves it on fb 1.3.0 before anything rests on it.
- **The bindings are Package**, made so by tools/bindgen.sh after fb-bindgen, since a program
  mustn't call them and `fb doc --check` would need docs on them otherwise.

## L1 to L3 (2026-11-11)

- **Calls run on a runner thread for each depth, by lua_resume**, not lua_pcall: a failed call
  keeps its frames, which luaL_traceback reads after, and a yield is told apart from an error.
- **Work that can raise runs under one C function the State makes, by lua_pcall**, with copies of
  the frame's values as its arguments and its results left in the caller's frame; what can't
  raise, a push after lua_checkstack, runs bare; and every exported function is a
  function-try-block that catches everything.
- **Arrays don't implement LuauType** (an array type has no Statics); they cross through tables
  and records. Map does.
- **Records are std's Load and Save**, not a For Field loop of the package's own: a record's
  Fields get `T?`, arrays, Maps, Enums, and std's attributes. An empty Luau table is an Array,
  swapped for an Object where Load's error says a Field wanted one.
- **Handles hold their State weakly**, not strongly as the plan first said: a program function
  that holds a handle is held by the State, so a strong handle made a cycle in the most common
  use, a function of the program's calling a Luau function. A handle used after its State is gone
  panics. This changes what the plan, as the user approved it, said of lifetimes.
- **An error crossing into Luau is its Message after where it was called**, as Luau's own errors
  are, since FutureBlitz can't give an Error's Class name as the program runs; it's the Cause of
  the LuauError it comes back out as, matched by its message.
- **No `LuauState` first parameter** for the program's functions: a function captures `[Weak lua]`.
- **`ToFunc` and `ToAction` take their type arguments written**, as overloads by type-argument
  count with no arguments aren't told apart from the type expected.

## L4 (2026-11-11)

- **A Class crosses as itself when it implements `LuauObject`**, an Interface with Static defaults,
  as `LuauRecord` is for data; `Register<T>()` is generated whole by tools/gen_arity.py, as a
  function that calls reflection loops passes their needs on to its callers.
- **What's exposed is Methods, marked:** `@LuauExpose` (gives a value), `@LuauAction` (gives
  nothing), `@LuauGet`, `@LuauSet`. The plan's `@LuauExpose Field` and `@LuauReadOnly` wait for
  FutureBlitz to pick Fields by attribute (gap 10); the two kinds of Method are apart for gap 12.
- **Objects are kept boxed by their type** (gap 11), so one reads back as the type it crossed as.
- **Builder methods are AddMethod, AddGetter, AddSetter, AddOperator**, as Method, Get, Set, and
  Operator are keywords; each takes a `LuauFunction`.
- **Names are strings, not atoms**, in __index and __namecall; atoms wait for a measure that says
  they're needed.

## L5 and L6 (2026-11-11)

- **A buffer's reads and writes past its end panic**, as std's Buffer's do; the numbers are read
  and written in FutureBlitz, little-endian, over byte copies the shim checks.
- **A script is an environment, not a thread:** a table of its own, `_G` itself, reading through a
  frozen and locked metatable to the frozen globals; its chunks load on a thread whose globals it
  is, since Luau resolves globals at load from the loading thread's.
- **print is a Luau function over one of the program's**, made with each State that has the base
  library, writing to `LuauOptions.Print` or std's Print; the plan's script name in front is left
  out, as a call doesn't know its script.
- **No memory count per script:** scripts' code runs on the State's shared runner threads.
- **The time budget is per outermost call**, the interrupt raising past it at every safe point.

## L7 (2026-11-11)

- **A wait is the waiting thread's, as Roblox's is:** `task.wait` and a `SetAsync` function queue
  the thread that called them, which yields to whatever resumed it; `Start`'s loop takes its own
  thread's waits, and the scheduler running the code runs any other's once the program's
  outermost call returns. So a coroutine that waits goes on by the scheduler, as in Roblox.
- **A thread's own `coroutine.yield`, under `Start`, waits for the next tick,** so a script can
  yield a frame; Roblox leaves such a thread suspended, which here would leave `Start`'s Task
  never done.
- **`task.wait` gives the seconds it was asked for**, the scheduler's time, not a clock's count,
  which a program's Scheduler's ticks would make wrong.
- **A thread's errors that nothing waits for go to `LuauOptions.ThreadError`**, by default std's
  `PrintError`, as Roblox prints them; `Start`'s Task throws its own.
- **Closing a State doesn't cut a wait short:** a TaskSource's Task wakes a coroutine on a
  program's Scheduler only after a main tick too (tried), so each driver ends at its wait's end,
  throwing `CLOSED`. A cancelled thread's driver ends quietly, its `Start` Task with no error.
- **A yield the package's own function refuses keeps the kind `YIELD`** out of the call, found by
  its Cause, as other errors raised in a script come out as `RUNTIME`.
- **A thread a call of the program's runs on can't be resumed or closed by anything,** and a
  bare yield on one is `YIELD`; a thread's id is its address, which its references keep alive.
- **The `task` table is frozen**, as Roblox's library tables are read-only.

## L8 (2026-11-11)

- **`Modules` reads files, not modules:** it's given a file's path with its extension, like
  `lib/init.luau` or `lib/.luaurc`, so one function gives both modules and their `.luaurc`s, and
  the shim looks for a module's files as Luau's CLI does. It can throw, and its error is the
  require's. A folder is taken to be there, as nothing asks the program for folders; a path with
  no module is Luau's "no module present at resolved path".
- **A chunk that isn't named `@path` requires from the root,** so `lua.Run(source)` can require
  without a path of its own; Luau's CLI allows only `@` chunks and stdin.
- **A module's globals are its own, as a script's environment is,** so its first load freezes the
  State, as `NewScript` does; its thread runs to its end, as a call's does, so a `task.wait` in
  it is YIELD at once, and a bare yield ends it with YIELD too.
- **A module's error is raised as it was,** its value and message, not wrapped as the CLI's
  "error while running module"; one that gives other than one value is an error saying how many.
- **`.config.luau` isn't supported:** it runs code to configure, which the sandbox doesn't need.
- **An error the shim raises for a yield nothing can wait for is YIELD** out of a call, by its
  message's start, which every such error of the shim's and the package's has.

## The review of L1 to L6, on dev/review-1 (2026-11-11)

The coordinator's decisions, as given in to-luau.md, with what the code made of them:

- **The shim's own function trusts no caller (28):** protect() leaves its work in the State, which
  run_work takes and clears, and raises with none pending. Every other C function of the shim's
  checks its arguments as a script's; a test walks every C function on the stack, and on a thread
  a script kept, during each kind of call, and calls each with nothing, a number, a table, a proxy,
  and an object.
- **getfenv and setfenv are the shim's own (29),** refusing level 0 and C functions; a runner's
  globals are the main thread's at the start of every call. **loadstring loads into its caller's
  environment (30)**, on a thread whose globals are it, marking it unsafe.
- **A collected number always has room (1, 31):** room for every number queued and every live one,
  made at each creation; a destructor finding none asserts in a Debug build. Collect and Step drain
  when no call is open. **A box is committed as its making's last step (32)**, and an uncommitted
  one's destructor does nothing. Luau's own pages hold small objects without asking the allocator,
  so the tests fail a making at points between its steps, besides failing allocations.
- **Every outermost entry from the program has the time budget (12, 33)**, set in protect() and in
  a resume by one guard, which puts depth and the deadline back on every way out (42), saturates
  the deadline (18, 37), and makes a timeout inside protect TIMEOUT. A budget under a microsecond
  is one.
- **Nothing in `frames` is read across protect (40):** the thread's taken by value, by one helper.
- **Records (11, 13, 14, 15, 20):** numbers past an Int64's exact range are Float64s, never a
  panic; Load's error paths are read by a reader of the package's own; a swap covers every element
  of the arrays on the path, so a 10,000-element array of empty records is two Loads; a wanted
  scalar finding an empty table says so; a record's bytes are a Luau buffer; a table is an Array
  only when its keys are 1 to its count.
- **Errors (16, 17, 19, 24, 25):** a LuauError raised in a script by the program's function keeps
  its Kind out of the call, found by its message, the most recent raise winning; another State's
  error value isn't pushed, only its message; a State that fails as it's made is closed; a copied
  value reads through a State of its own once its State is gone.
- **Registration (3 to 8):** a type is kept by an id of its own, as `TypeName` has no module (gap
  17); two marked members of one Luau name, and a marked Method of a shape none exposes, panic at
  Register naming them (gap 16); Close empties each registration; an Add checks its function's
  State and replaces what the type had by that name. **Objects by identity (2) waits on the fb
  lane** (gap 14). A derived type crosses as its base (gap 15).
- **The shim's core (34, 36, 38, 41, 43 to 49):** the error slot holds false and a flag, never nil;
  object numbers past 2^53 are refused; a type's metatable is frozen again whatever happens; with a
  memory limit, the collector's goal is 125 and its step 400, and an outermost MEMORY collects
  everything; the owner's read under the lock; a slot whose 24-bit generations are spent is
  retired; room's reserved before a slot's taken; Close drops frames the program left open; the
  frame functions' catch-alls give -1.
- **Freeze reaches every table the globals' tables hold (35),** not through metatables or
  functions; a table scripts share and write is given by a function or an object.
- **loadstring refuses a source bigger than the memory left (39),** and looks at the budget when a
  compile returns; a compile itself can't be interrupted.
- **Luau stays pinned by its tag (50),** as CLAUDE.md says, and the configure stops when the tag
  isn't commit 12a6ba75 any more.
- **Added (26):** `lua.Step(kilobytes)`, `lua.Has(name)`, `NewScript(name)`, whose name is its
  code's chunk name by default; and `LuauStatesOpen()`, the process's States and their bytes.

## L9 (2026-11-11)

- **The container names its Luau release, not only its bytecode version:** a release can change
  what its bytecode means within one version, so `Load` takes only its own release's.
- **A key is 16 bytes at least,** and a shorter one panics where it's used: a key is a secret, and
  a short one is a mistake, not data.
- **The chunk's name goes in the container,** as the bytecode doesn't carry it, so a traceback
  says the script's path.
- **Compile has no State:** the shim compiles with options it's given, so a build step compiles
  without a VM; a syntax error says the chunk's name as a State's does.

## L10 (2026-11-11)

- **Native code is MARKED by default,** as Luau's own default is, so a game opts its scripts in
  with `--!native`, and `ALL` is an option; `ALL` compiles cold functions too.
- **No userdata types for the compiler yet:** objects share one userdata tag, and the code
  generator's typed lowering keys by tag, so naming the types without a tag each would mislead it.
  A tag per type is a change to the udata area, for later if it's worth it.
- **Native code isn't counted in the memory limit:** the code generator's executable memory is
  its own, outside the VM's allocator; it's about the size of the bytecode it compiles.
- **`lua.NativeFunctions`** is the count of functions compiled, which tests and a game's own
  diagnostics read; there's nothing else to tell native code from interpreted.
