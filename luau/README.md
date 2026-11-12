# luau

[Luau](https://luau.org) scripts in a FutureBlitz program: scripts a program didn't write, like a
game's mods, sandboxed, with a typed API where Luau's stack is never seen.

```toml
[dependencies]
luau = "0.1"
```

```vb
Import std.core
Import luau

Local lua = LuauState()
Try lua.SetFunction("add", Function(a: Int, b: Int) => a + b)
Print(Try lua.Evaluate<Int>("return add(2, 3)"))      // 5
```

- Values that cross by their types, records as tables, tables and functions as handles, and
  functions both ways, throwing ones too, with Luau's tracebacks kept.
- Classes registered by compile-time reflection, exposing only what's marked.
- A sandbox by default, without asking: a memory limit of 256 MB and a time budget of 10
  seconds a call, globals of each script's own over shared ones frozen at the first script, no
  loadstring, getfenv, setfenv, or native code, and no file or OS access unless the program
  gives it. `LuauOptions.Sandbox()` adds what mods want: 64 MB, 50 ms a call, and 1,000 waits.
- Luau's threads on FutureBlitz's schedulers, so a script's `task.wait(2)` waits as an Await
  does, and an Async Function a script calls is waited for, once the program has called
  `lua.Start()`, which runs a State's waits until it closes.
- `require` over files the program gives, bytecode compiled ahead and signed by the build's
  Ed25519 key, native code where Luau's code generator runs, and Luau's type checker, which
  checks a mod against the API the program gives it.

[The guide](https://github.com/hlpdev/fb-luau/blob/master/docs/guide.md) goes through each part
with a program that runs.

Needs fb 1.3.2 or later, on Windows x64, Linux x64 and arm64, or macOS arm64 and x64. Apache-2.0;
in its library, Luau is MIT (notices/luau.txt), and on Windows MinGW-w64's winpthreads is MIT
and BSD-style (notices/winpthreads.txt).
