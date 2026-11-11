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

**It's in development.** Nothing is released yet, and the API above is the plan's, not code
that runs today. [docs/plan.md](docs/plan.md) has the whole design and the milestones to the
first release, and [docs/decisions.md](docs/decisions.md) why.

## What it will do

- Values that cross by their types, tables and functions as handles, and functions both ways,
  throwing ones too, with Luau's tracebacks kept.
- Classes registered by compile-time reflection, exposing only what's marked.
- A sandbox by default: per-script globals, a memory limit, a time budget per call, and no file
  or OS access unless the program gives it.
- Luau's threads on FutureBlitz's schedulers, so `task.wait(2)` waits as an Await does.
- `require` resolved by the program, bytecode compiled ahead and signed, native code, and Luau's
  type checker with its diagnostics.

## Building

fb-luau needs fb 1.3.0, CMake 3.28, Ninja, Python 3, and a C++23 compiler: GCC or Clang on Linux
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
