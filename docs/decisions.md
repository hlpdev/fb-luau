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

## The second review, on dev/review-2 (2026-11-11)

- **A require's path is at most 1,024 bytes of 64 parts (51),** refused at require, before the
  reader's asked; the walk checks the budget each time it reads, keeps only the files it found,
  and lets them go when its module's loaded. A walk is its own, kept aside while the reader runs
  code that requires (58).
- **A part the reader could take for something else is refused (57):** empty, `:`, `\`, NUL, and
  Windows's device names.
- **An exporting module gives a table (52),** or its require's an error; any error after its
  placeholder's made clears the placeholder and its provided flag, so the next require loads it
  anew (53). A module's table is frozen through its fields (55). The modules can't be forgotten
  while one loads (56), and forgetting clears the provided flags too.
- **debug.info and debug.traceback are the shim's (54),** refusing a runner's or a module's thread
  that isn't the caller's, adding one to a level of the caller's own, as the wrap is a frame; a
  runner is reset after every call, which drops a finalizer one script left on it too.
- **getfenv and setfenv, given a function, take one of the caller's own environment (55);**
  loadstring's environment is the nearest Luau function's, and it's refused with none (59).
- **Every MEMORY error collects at the outermost (60),** in record_error, the one place.
- **A resume that fails as it starts ends the thread (61);** with a memory limit, collectgarbage
  can't set the goal, the step, its size, or stop the collector (62).
- **No wait is a timer of its own (64),** as decided, with option (b): a State keeps its waits by
  thread, each with when it's due, and a driver coroutine on a scheduler resumes them at its
  ticks. A thread's Start is a driver, for its thread and what the threads it starts wait for;
  `lua.Start()` is the State's, for the rest. Without one, a wait is a `YIELD` error that says
  so: a State on a worker thread has none, as nothing there can reach Async code (71). The plan
  changed back from "a wait isn't cut short": Close ends every wait at the driver's next wake.
  What replaces it, if the scheduler gets a readable time, is (a): the driver reads the
  scheduler's time at each tick, with no timer at all.
- **A driver's timer waits at most a second, not 50 ms (64), a change from what was approved:** a
  timer made in a tick counts from the tick's end, so at a coarse tick (a test's 0.1 s Step, a
  paused game's long one) a 50 ms cap loses most of each tick and waits run several times late.
  At a second, a wait is late by at most one tick's overshoot each second, and a closed State's
  driver ends within a second of the scheduler's time.
- **At most `LuauOptions.MostWaiting` threads wait (64),** 10,000 by default, past which the
  script's call is an error; a thread waits for one thing at a time, and a second is an error in
  the caller (73).
- **A thread a driver has is WAITING (65):** Resume and Start on it are LuauErrors, not panics,
  as a script can make one wait. A State's driver started twice still panics, as only the
  program does that.
- **One time budget a tick for all a driver resumes (68),** what's still due going first the
  next tick, by `fbl_state_turn`, which opens one deadline over several resumes.
- **A call keeps its most recent 64 errors (69),** the oldest losing only its Cause.
- **A driver is kept by what it holds, not a Defer (70):** its Destroy gives its waits to the
  State's driver, or drops them. A coroutine its Scheduler drops does let go of its frame, so
  that runs, where a Defer doesn't (asked of the fb lane). A thread's id is a number of its own,
  kept by a weak-keyed table in its State, never an address.
- **ThreadError runs once the outermost call ends (74),** so it can close the State, and
  `task.spawn` of a running, dead, or runner thread is an error in its caller.
- **A marked member that can't be exposed is found by its member, not its Luau name (75).** A
  marked Static, ByRef, or Unsafe Method isn't exposed and can't be said, as FutureBlitz's For
  Method doesn't visit them; Register's doc says so, and it's the fb lane's. A marked Method
  with an argument that isn't a LuauType doesn't compile, as Register's Where lines need it, so
  it has no panic case; one of 8 arguments, throwing or not, has.
- **An element type whose FromValue wants an empty Array in some and an Object in others is
  refused (77),** accepted as decided.
- **A copied value's Read still goes through a State made for it (78), against the decision:**
  fb 1.3.0 has no generic `As T` from a Bool or a String, and a Bool isn't an Object to test `Is
  T`, so only `T.FromLuau` makes a T (fbengine-notes/luau/generic_as.fb, for the fb lane).
  AsBool, AsNumber, AsInteger, and AsString read one with none, and Read's doc says so.
- **`LuauStatesOpen(here: True)` counts the calling thread's States:** `fb test` runs tests on a
  pool of threads, so a test that watched the process's count raced the others'; the package's
  own tests count theirs.

## L11 (2026-11-11)

- **The checker asks for nothing across the shim:** a check gives the files it wanted and wasn't
  given, and the package reads them and checks again. The VM's require calls a Luau function as
  its reader; a checker has no State, and a C callback into FutureBlitz would be the shim's first.
  Rounds are as many as the require graph is deep, as Frontend's parse finds every file it can
  reach from what it has before anything's checked.
- **Its diagnostics match luau-analyze's for the same files,** module by module: a corpus
  (tests/check/corpus) keeps what luau-analyze, built from the same Luau, gives
  (tools/analyze_golden.py), and the C++ tests compare. A cycle's modules can come in another
  order, as luau-analyze names them by its root's name and by their full paths.
- **A default limit of 10 seconds a module,** where luau-analyze has none: a game checking mods as
  it loads them mustn't hang on one.
- **Definitions record types where reflection sees them:** Register's loops and SetFunction keep
  each member's and global's FutureBlitz types by TypeName, which Definitions maps to Luau's. A
  record is `any`, as nothing reaches its fields from its name; arguments are `a1`, `a2`, as
  reflection gives no parameter names. What the State sets for itself, `print` and `task`, isn't
  the program's: `task` is declared by hand.

## L12 (2026-11-11)

- **The guide's programs are checked by running them:** tools/guide_programs.py runs every vb
  block of docs/guide.md as a program of its own on the package, as ctest's docs.guide_programs
  does, and each checks what it shows with Expect.
- **The REPL reads its input from stdin, as a REPL does;** ctest gives it a session through a
  CMake script (tests/run_with_input.cmake), as add_test can't give a program input.
- **fb 1.3.0's EmbedFolder leaves out dotfiles,** so examples/modded_game embeds its mods'
  .luaurc as luaurc.json, saying why; a game that reads its mods folder needs none of it. For the
  fb lane: fbengine-notes/luau/embed_dotfiles.fb.

## L13 (2026-11-11)

- **Luau's conformance scripts are copied in** (tests/conformance/scripts), so CI's test job, which
  has no Luau source, runs them. 35 of 59 pass, native code off and on; the rest are listed with
  why. A script's `is_native` is the runner's, which reads Luau's internals; the program's stands
  in from the native-code mode, so the two scripts that ask it of each function run only with
  native code on, as Luau's runner runs them, and native_types, which asks it of a function that
  left native code, is listed.
- **What the conformance scripts found, each fixed with a test:** the shim's debug.info guard
  turned a negative level into 0, so `debug.info(-1)` read the guard's own frame where Luau refuses
  it; loadstring named a chunk "loadstring" where Luau and Lua name it by its source, so its errors
  said `loadstring:1:` where scripts expect `[string "..."]:1:`; and the package had no way to load
  source that isn't UTF-8, which Luau's source can be: `Load` takes bytes now too.
- **UBSan's float-cast-overflow is off for Luau's own code, on for the shim's:** Luau casts a
  double to an int and keeps what the processor gives, a NaN's too (luaH_get, lua_tointegerx, the
  VM's loops), which the sanitizers and the fuzzers found at once.
- **The fuzzers run everywhere ctest does,** over their corpora and random changes of them by
  fuzz/replay.cpp, and for real with libFuzzer, ASan, and UBSan in stability.yml.
- **`LuauProcessMemory`** is new and public: std has no way to read a process's memory or its
  live objects (for the fb lane), which a soak needs. On Windows it's private bytes, as the
  working set jumps by megabytes as Windows trims and grows it.
- **The soak judges memory by thirds,** and only from 5,000 iterations: a heap that grows once
  steps once, where a leak grows at every stretch; a short run's slopes are within its noise.

## L14 (2026-11-11)

- **publish.yml publishes only from a tag the user pushes, or a run the user starts,** and only
  after a dry run of the same archives; its job is in an environment, `publish`, so the
  repository's settings can ask for a reviewer too. The installed check retries `fb add` for up to
  five minutes, as the registry's index can lag the publish.
- **The release's notes are a draft for the user** (fbengine-notes/luau/release-0.1.0.md): the
  tag, the GitHub release, and publishing are the user's to do.
## The third review, on dev/review-3 (2026-11-12)

Written while the PC was kept free for the user: none of it built or run until their go.

- **Bytecode is signed by Ed25519, not an HMAC (79), a change to what was approved (plan question
  4):** the HMAC's key both signs and checks, so the game shipped the key that signs. `Save` takes
  the build's private key and `Load` the game's public key; the signature covers a fixed context
  first; the format is 2. examples/bytecode is two modes, the build's and the game's, the game
  with the public key only.
- **The container's parser reads nothing before checking it's there (83),** and a name with a NUL
  is refused at Compile and Load (84).
- **Native code is off by default, and capped (80):** NativeCode's default is OFF; where on, the
  generator is made with 256 KB blocks and `NativeMemory`'s cap, 16 MB by default, its memory
  counted by its allocation callback and shown as `NativeMemoryUsed`; past it, code runs
  interpreted. L10's "about the size of the bytecode it compiles" was wrong: Luau committed 4 MB
  a State and allowed 256 MB.
- **Native code is used only where it can run (82):** `isNativeExecutionEnabled` after `create`.
- **Five tests unwind through native frames (81),** in ALL and with `--!native`: the budget, an
  error caught and let go, the memory limit, a yield, and a traceback.
- **A task's arguments stay in the State (85):** one table a wait, kept by reference, pushed at the
  resume, counted to the State's limit; nothing of a script's arguments is copied to the program.
- **A chunk's name, loadstring's too, reaches nothing above the root (86):** a chunk of any name
  but a path's requires from the root, as a module "=" there; a path's name goes through the
  parts and length checks, as require's argument does; the .luaurc search keeps to the budget.
  The decision had loadstring mark a script's "@" name "=", so its chunk never named where
  require looks; that changes how Luau shortens a long name in errors, which Luau's conformance
  checks (errors.luau), and the checks are what keep a script within the root, so the name stays
  as given. A script that names a chunk for a module's path requires from there, which reaches
  only what the program gives every script.
- **The waits' driver reads its scheduler's time (87),** fb 1.3.2's `Scheduler.Current.Time()`: no
  timer coroutine, no quantum, no 1 s cap. A wait made in the driver's turn counts from the time
  then; one made outside it, in a call of the program's or a thread's first resume, from the
  driver's next turn, on its own scheduler. So a wait is never early, and at most a tick late.
  A thread's Start, ending, gives what it still drives to the State's driver, or, with none,
  ends each as a YIELD error to ThreadError.
- **A driver checks its State after every resume and Report (88),** and gives CLOSED once
  ThreadError closed it, not a panic.
- **The tick's budget decides only whether another thread starts (89):** each resume has the
  State's budget of its own, so a hog ends only itself; `fbl_state_turn`'s shared deadline isn't
  used by the driver now.
- **A tick inside a call of the program's is no turn (94):** `fbl_state_turn`'s 3 says whether a
  call runs, and what's due goes the next tick.
- **A level's function of another environment is refused (90),** by getfenv, setfenv, and
  debug.info's "f", as a function given one is.
- **Freezing follows metatables (91);** what a module's functions reach by upvalues stays its own,
  and the docs say so.
- **An Async function's wait is checked before the program's function is called (92).**
- **lua.Start() runs until the State closes (93):** said in its doc, the guide, the READMEs, and the
  plan; a program closes the State to end it.
- **A require's placeholder is cleared and frozen on every way out (95),** by a catch that clears
  and rethrows, and the load fills a cyclic requirer's placeholder itself, where a failure clears
  it, rather than leave Luau's require to, where one partway would leave it writable.
- **Kept errors are capped by a megabyte of text too (96).**
- **A require's path counts backslashes as separators (97).**
- **A wait entry belongs to its wait (98):** a thread's own wait keeps the function it waits in
  (fbl_thread_waits_in), and is dropped at its turn when the thread isn't suspended in it, as when
  a script resumed or closed it; a thread that waits again while its old entry is kept replaces
  it.
- **The two tests (99) can't stand as they were:** the wrong-self test took a type's handler from
  the stack of the thread that called the Method, and the Method's call back into Luau runs on a
  runner of its own, whose levels don't reach that thread, and review-2 refused another call's
  thread to debug.info: the handler is on no stack a script can read, which is the fix. For the
  same reason escape_test's walker stays at `found >= 1`. 54's finalizer test now fails without
  its fix, a failing call resetting the runner. 61 has no test yet: it wants 200 nested C calls
  ending in a program function that resumes.
- **Objects by identity (2) still waits on fb:** fb 1.3.1's `Identity<T>` needs `T: Class`, and
  LuauObject's Static defaults, an Interface's, can't say Self is one, convert their value to an
  Object, or key by `Identity<Object>` (fbengine-notes/luau/interface_identity.fb, for the fb
  lane). Written with `T: LuauObject & Class`, it didn't compile there; it's backed out, and the
  Map is by the Class's own `==` and Hash meanwhile.
- **Only the driver reads its scheduler's time:** `Scheduler.Current` is the main thread's, and a
  State on a worker thread would reach it through a wait's call, so a wait made in a turn is due
  from the turn's time plus the real time the turn's taken since, which is exact on the main
  scheduler and late by that much on a hand-ticked one.
- **fb 1.3.2,** in project.toml and every workflow. The workarounds fb 1.3.1's fixes made needless
  are out: the Throws Error lambda, the Dog that couldn't list LuauObject, the drivers' array
  instead of a Map of Weak, and the compile test, now a run that Register's panic names.
  `@LuauAction` stays, as it says what a Method is.

## The fourth review, on dev/review-4 (2026-11-12)

- **A level given as a numeric string is checked as its integer (100),** as Luau takes one.
- **Any thread's levels go through the environment check, and a thread belongs to its main
  function's environment (101):** coroutine.resume, close, and a wrapped coroutine's call, and
  task.spawn, defer, delay, and cancel given a thread, refuse another script's; its status is
  anyone's to read. The coroutine library's resume, close, and wrap are the shim's, over Luau's.
  With no scripts of their own, every thread's environment is the shared one, and nothing
  changes.
- **The bytecode example's game has its public key compiled in (102),** `Embed("game.pub")`, and
  the build is given its private key from outside what it writes; keys/build.key is the
  example's own, which a real game's never is. A key of the wrong length stays a panic.
- **A turn resumes the entry it collected (103),** by RefEquals, never a newer wait of the same
  thread's.
- **A stale wait of a thread's own is dropped wherever a wait is checked (104),** in CanWait, so an
  Async program function a thread calls after a script went around its wait isn't "waits
  already".
- **A turn's clock starts where its time is read (105);** the plan says a wait made outside a turn
  is at most two ticks late.
- **A driver destroyed with no heir gives its waits to ThreadError (106),** as HandedOver does.
- **The tests (107):** the hog's bystanders spin 10 ms each, so one is resumed with the tick's
  budget nearly spent, which a shared deadline would end; 98's second half asserts `done`.
- **A native stack guard, found on the way:** writing 61's test, string.gsub's callbacks nested in
  each other ended the test process with a stack overflow, 0xC00000FD, at about 135 levels on
  Windows's 1 MB main thread, Release too, short of Luau's 200 C calls: any script could end a
  game. The shim's interrupt, which every Luau call reaches, now raises a stack overflow error
  128 KB short of the thread's stack's end, which each thread asks its platform for once
  (VirtualQuery's reservation, pthread_getattr_np, pthread_get_stackaddr_np); where it can't,
  past 384 KB from the call's outermost entry. A fixed 384 KB from the entry, tried first, stopped
  Luau's own pm.luau, 60 nested gsubs, at about 50. On a thread with a small stack, a script nests
  less: the thread's stack is the limit, not Luau's 200 C calls. It also makes 61's failure, a
  resume refused at Luau's 200 C calls, unreachable from a script where the stack is small, so
  61's test is this one's.
- **An Async program function's String argument is a copy (108),** noted in the guide, with
  LuauValue or LuauTable for large ones.
- **Checking runs a mod's type functions under two limits (109):** Luau 0.746 always parses
  `type function`, and its solver runs the body in a VM whose heap had no limit. LuauChecker
  takes `typeFunctionMemory`, 64 MB by default, Luau's DebugLuauTypeFunctionRuntimeHeapLimit,
  which is the process's: the smallest any checker asks for is every checker's, lowered only
  while no check runs (a shared lock each check holds). A time limit of 0, "as long as it
  takes", is refused, so a loop in a type function always ends. The C API's fbl_check_new takes
  the heap limit.
- **The modded game survives each bad mod (110):** its check and load are caught for it alone,
  as a LuauError, and a mod that fails is dropped with the tick hooks it gave; four more mods
  show it, one for each way a mod failed to load before (a MEMORY error, a loop with its
  checking off, an error after hooking the tick, and two init files), which ctest's regular
  expression expects, beside the good ones playing.
- **Sandboxed by default, as the approved plan said (112):** LuauOptions' MemoryLimit is 256 MB
  and TimeBudget 10 seconds unless the program sets them, 0 being none by its own choice, so a
  runaway script can't take a program with it unasked; the rest of the sandbox was already the
  default. `LuauOptions.Sandbox()` is the mods' preset: 64 MB, 50 ms, 1,000 waits, and the
  defaults' libraries, collectgarbage, loadstring, getfenv, and native code said again, so a
  mod keeps them whatever the defaults become. Every library stays, each being safe for a
  script of its own. The modded game starts from it; the conformance runner sets no limits, as
  Luau's has none.
- **The guide says what a pcall catches and whose memory it is (113):** a script's pcall catches
  "not enough memory", as Luau's does, but not a timeout; the memory limit is the State's, so a
  hoarding mod fails the others' calls, and a State for each mod is how to give each its own.
- **An object of a Class with its own `==` is documented (114),** in the guide and objects.fb,
  until 1.3.3 lets finding 2 be fixed.
- **Compiling ahead sets Luau's flags (117),** as every entry that reaches Luau does, so bytes
  compiled before any State are a State's, and a compile on one thread doesn't race the first
  State on another. The bytecode tests' first case compares bytes from before a State with
  bytes from after.
- **An exception at the shim's edge is counted (118),** `fbl_core_escaped`, the process's: every
  fuzzer fails when it moves in an input, and every C++ test's State checks it's 0 as it closes,
  so the shim's first rule is a finding though the export gives a status the API has. The new
  fuzz targets and the exports not yet fuzzed are 0.1.1's.

## The fifth review, on dev/review-5 (2026-11-12)

- **The thread fence of 101 is out whole (123):** coroutine.resume, close, wrap, and finally, and
  task.spawn, defer, delay, and cancel are Luau's and the package's own again, with no check of
  whose thread it is. It refused ordinary module code (require gives each module an environment
  of its own, so a module's generator, a helper resuming what it's given, and a Signal's Wait),
  it was bypassed wherever the guarded function had no Luau caller, and the wrapped call popped
  one value too many with no caller. What stays: debug.info's levels and thread argument checked
  by environment, and runners refused. A thread a script hands to another's code is theirs to
  resume or close, as a function it hands over is theirs to call; mods that mustn't touch each
  other get a State each (125). fbl_thread_is_callers is gone.
- **A check that reaches a `type function` is refused (124),** a "TypeFunction" diagnostic at
  each, found in the parse before the solver, so none is run: a type function's print, types.*,
  and nested calls ran in the host's strings, arena, and native stack, which no limit held. The
  heap limit of 109 stays as a second fence.
- **An own level of INT_MAX isn't shifted (128),** which would overflow; Luau refuses it.
- **A State a mod (125):** in one State a mod's memory, waits, threads, and required modules are
  every mod's, so one that hoards to just under the limit, or holds every wait, fails the
  others, and a dropped mod's threads run on. The modded game gives each mod a State of its own,
  made with Sandbox(), closed when the mod's dropped; the guide says a State is what keeps one
  mod from another, scripts in one State being one author's files. The checker's definitions
  come from a State given the API the same way, closed after.
- **LuauState() is LuauState(LuauOptions()) (126):** its tick budget and waits limit are the
  defaults', 10 seconds and 10,000, where the tick budget was none.
- **The bytecode example makes its key pair (127):** examples/bytecode/keygen makes an Ed25519
  pair once, where there's none, keys/build.key and game.pub, both ignored by git; ctest runs it
  first. The pair committed before is out of the tree, though in the history, which only a
  force-push would take out; it was only ever the example's. `*.key` and `*.pub` are binary.
- **The stack guard's own test (128):** tests/state runs 190 nested gsubs on a thread with 512 KB
  of stack reserved (CreateThread's reservation on Windows, where winpthreads' stack size is
  only the first commit; pthread's elsewhere) and expects the guard's message, so it can't pass
  on Luau's own C call limit. is_callers_thread went with 123.
- **Finding 2, on fb 1.3.3:** an object's userdata is found by `Identity<Object>`, the object
  converted by `As? Object`, which 1.3.3 allows of an Interface-constrained type parameter's
  value: two objects a Class calls equal each have their own, and one whose Hash changes keeps
  its one. fb 1.3.3 in project.toml, every workflow, and the READMEs; the plan's gap 14 is fixed.

## The sixth review, CI on dev/review-4 (2026-11-12)

- **The shim links pthreads wherever it's linked (129):** the stack guard's pthread_getattr_np
  and pthread_attr_getstack are in libpthread before glibc 2.34, so Threads::Threads is on the
  shim's objects, which the library, its tests, and the fuzzers all link.
- **No test's result depends on the machine's speed (130, 132):** a memory test takes its memory
  in one step, one string.rep past the limit; a volume test has no time budget; the tick's
  budget is tested by counting what a tick starts (at most six 4 ms resumes in 20 ms); the hog
  test's margins are half its budget, and a wall-clock bound goes through luau/tests/support's
  Slack, four times on GitHub's runners. The modded game's hoarder takes its memory in one step.
- **The guard's "fine" depth is twenty (131):** fb test runs on threads of 512 KB on macOS,
  where the guard stops about fifty levels of nested gsubs, fewer in a debug build, as it
  should; the guard's own test is in tests/state.
- **The guard's own test runs on 256 KB (133):** 512 KB held 190 of a Release build's levels on
  Linux, about 2 KB each, so nothing stopped them. On 256 KB the guard leaves 128 KB, which 190
  levels pass in any build whose level takes over 700 bytes, short of Luau's 200 C calls.

## The seventh review, publish.yml on dev/l14-release (2026-11-12)

- **Publishing is by hand only (134),** on the tag v<version> of luau/project.toml, which a
  `version` job checks before anything else runs, as fbengine's does: a pushed tag publishes
  nothing, as a published version can't be taken back.
- **A first version publishes by token (135):** the registry answers a package it doesn't have
  with 403, so the Publish step's env has FB_REGISTRY_TOKEN from the secret of that name, which
  fb uses when it's set, and trusted publishing when it isn't.
- **The release is posted to Discord (136),** as fbengine's: the GitHub release's first
  paragraph, made before the run with the notes the user approves, pinging
  DISCORD_RELEASES_ROLE, only for futureblitz.dev, and with nothing posted and nothing failed when
  DISCORD_RELEASES_WEBHOOK or the role isn't set.
- **The registry is a choice of two (137):** `futureblitz`, futureblitz.dev, the default, or
  `staging`, https://staging-index.futureblitz.dev/, as fbengine's. The input was
  `futureblitz.dev`, which `fb publish --registry` would have taken for a folder, as fb names
  futureblitz.dev's registry `futureblitz`; the installed check adds from staging by a
  [registries] entry, as fbengine's does.


## 0.1.1, the findings left from the reviews (2026-11-12)

- **One deadline for a check (115):** Luau gives each module its own time limit, so a check of N
  modules took N times it. A check now has the checker's limit for all its modules, across the
  runs it takes the program to give its files: a thread of the shim's cancels Luau's token at the
  deadline, which the solver asks between constraints, and no module's begun after it. Luau's
  constraint generation, which runs before a module's solver, and its lint can't be stopped, and
  Luau begins together the modules that need none of the others, so a check can run past the
  limit by those of the modules under way; the header, the docs, and the plan say so. A check
  past it gives one TimeLimit diagnostic, at the module checked, and none of its modules' others,
  as Luau gives no results for a cancelled check.
- **No Discord post:** by the user's word, luau isn't an official library, so publish.yml's
  `discord` job (136) is taken out, and nothing presents the package as official.
- **LuauCheckerOptions.Enforced (116):** a struct of the checker's options, Mode, TimeLimit,
  TypeFunctionMemory, and Enforced, for a LuauChecker made from it, beside the New of 0.1.0,
  which stays. With Enforced, the checker's mode and Luau's default lint hold for every module:
  its `--!strict`, `--!nonstrict`, `--!nocheck`, and `--!nolint` lines are taken out after the
  parse, its mode set, and a `.luaurc` gives only its aliases. It's off by default, as
  luau-analyze has it, for editor support and tooling; the modded game turns it on, and the guide
  says the checker is a help to a mod's author and the sandbox is what holds a mod. In the C API
  it's fbl_check_enforce, a new export, so fbl_check_new is as it was.
- **The type functions' heap lock is warmed before its first use:** MinGW's winpthreads sets a
  std::shared_mutex's lock up on its first use, and threads that first use it at once can fail
  (a lock_shared that gives EINVAL: a debug build asserts, a release build goes on unlocked and
  later unlocks what it never held). It showed as an assertion in `fb test luau` when the checker's
  tests first ran together. The lock is now made by a function's static, which locks and unlocks
  it once, alone; it's the only shared_mutex in the shim and Luau, and std::mutex has no such race.
- **The fuzzers reach every export (118):** the API fuzzer calls each of the 104, with a callback
  whose function's number picks the status it gives, valid or not; a new fuzzer, `mod`, runs the
  input as a mod does, in a frozen State's script environment with the program's functions that
  call back in, raise, and yield, a registered type, and require over a module the input gives;
  the checker's fuzzer gives definitions, and is enforced and forgets for some inputs. stability.yml
  runs `mod` as it does the others.
- **A State closes with a driver's turn left open:** fbl_state_close refused a State whose
  program began a turn and didn't end it, as a turn counts as a call, so the State never closed;
  the API fuzzer found it. Nothing runs in a turn the program holds open, so close now ends it;
  a call running, or a function of the program's that Luau called, still refuses the close.
- **The soak keeps a State and a checker for its run (119):** beside the State it makes and
  closes each iteration, it keeps one, as a game keeps a mod's, frozen by its first require, and
  does the same work in a script of its own there, its modules required and forgotten; and one
  checker, which forgets and checks two modules each iteration. It judges the kept State's own
  memory, collected whole at each sample, at 8 bytes an iteration, beside the process's at 64.
  ctest's run is judged by its exit code, which a failed iteration sets, not by a line it prints.
- **The package's tests under ASan and UBSan (120):** stability.yml's `package-sanitized` job
  builds the library alone with them, by GCC, whose runtimes are shared, builds `fb test luau`'s
  program with `fb test -o`, and runs it with the runtimes preloaded, as fb's program isn't built
  with them, and LeakSanitizer on. GCC rather than the C++ tests' Clang, as Clang's runtime for a
  shared library is one more thing to preload; run in WSL's Ubuntu with GCC 15 before it landed,
  all 202 tests passed. Windows has none, as MinGW's GCC has no sanitizers; the plan says so.
