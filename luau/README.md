# luau

[Luau](https://luau.org) scripts in a FutureBlitz program: a typed API where Luau's stack is never
seen, functions and types both ways, and a sandbox by default, for scripts a program didn't write,
like a game's mods.

```toml
[dependencies]
luau = "0.1"
```

```vb
Import std.core
Import luau

Print("luau {LuauLibraryVersion()}, Luau {LuauVersion()}")
```

It's in development: this release has only its library's versions, and
[docs/plan.md](https://github.com/hlpdev/fb-luau/blob/master/docs/plan.md) has the rest of its API,
as it's being built.

Needs fb 1.3.0 or later. Apache-2.0; in its library, Luau is MIT (notices/luau.txt), and on Windows
MinGW-w64's winpthreads is MIT and BSD-style (notices/winpthreads.txt).
