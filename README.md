# fb-luau

[Luau](https://luau.org) for [FutureBlitz](https://futureblitz.dev), as one package, `luau`:
run scripts a program didn't write, sandboxed, with a typed API where Luau's stack is never
seen.

```vb
Import std.core
Import luau

Local lua = LuauState()
Try lua.SetFunction("add", Function(a: Int, b: Int) => a + b)
Print(Try lua.Evaluate<Int>("return add(2, 3)"))      // 5
```

[docs/guide.md](docs/guide.md) goes through it a part at a time, each with a program that runs;
[docs/plan.md](docs/plan.md) has the design, and [docs/decisions.md](docs/decisions.md) why.

## What it does

- Values that cross by their types, records as tables, tables and functions as handles, and
  functions both ways, throwing ones too, with Luau's tracebacks kept.
- Classes registered by compile-time reflection, exposing only what's marked.
- A sandbox by default, without asking: a memory limit of 256 MB and a time budget of 10
  seconds a call, per-script globals over shared ones frozen at the first script, no loadstring,
  getfenv, setfenv, or native code, and no file or OS access unless the program gives it.
  `LuauOptions.Sandbox()` adds what mods want: 64 MB, 50 ms a call, and 1,000 waits.
- Luau's threads on FutureBlitz's schedulers, so `task.wait(2)` waits as an Await does, once
  the program has called `lua.Start()`, which runs a State's waits until it closes.
- `require` resolved by the program, bytecode compiled ahead and signed by the build's Ed25519
  key, native code, and Luau's type checker with its diagnostics, against a definitions file of
  what a State gives.

The examples, which ctest runs: [examples/modded_game](examples/modded_game), a toy game with
mods; [examples/repl](examples/repl), a REPL; [examples/bytecode](examples/bytecode), a game's
build step for its own scripts.

## Building

fb-luau needs fb 1.3.2, CMake 3.28, Ninja, Python 3, and a C++23 compiler: GCC or Clang on Linux
and macOS, and on Windows a GCC built for the UCRT, as MSYS2's UCRT64 has, or winlibs' UCRT
build. The library is built into `luau/native/<target>/`, where the package finds it, and ctest
runs the tests:

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

## License

Apache-2.0. The package's library has Luau in it, under the MIT License, whose notice is in
[luau/notices/luau.txt](luau/notices/luau.txt).
