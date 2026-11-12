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
