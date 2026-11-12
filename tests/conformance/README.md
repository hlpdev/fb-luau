# Luau's conformance scripts

`scripts/` is Luau's `tests/conformance`, at the release cmake/luau.cmake pins, as Luau has them,
under its MIT License (luau/notices/luau.txt). `main.fb` runs each through the package, as Luau's
own runner runs them, native code off and on, and lists those that need what only that runner
has, each with why. Where a script needs one of the runner's C functions, a stand-in of the
package's takes its place; where only a case of it can't run here, that case alone is cut, with
why, and the rest runs. When Luau's release moves, copy its scripts here again.
