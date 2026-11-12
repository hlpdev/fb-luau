# fb-luau

Luau for FutureBlitz, as one package, `luau`, over one library of its own, `luau_native`, which
has Luau in it. Its own package, not part of FutureBlitz, std, or fbengine. docs/plan.md has the
design, the API, the shim's rules, how it's built, tested, and shipped, and the milestones L0 to
the first release; docs/decisions.md has why, newest last. Its first user is SCP: Containment
Breach X's mods.

## How we work
- You write the code, C++ and .fb, one milestone step at a time, in my code style, each on a
  branch `dev/<step>` off master. Verify each step before reporting it: `python tools/verify.py`
  prints "everything passed" (the build, ctest with the C++ tests and `fb test luau`, the export,
  bindings, arity, and notices checks, clang-format, clang-tidy on what changed, `fb fmt --check`,
  and `fb doc luau --check`). Every bug a check or review finds is fixed, with a test.
- Write each step up in C:\Users\Hunter\Projects\fbengine-notes\from-luau.md, newest last: what
  was done, how it was verified, and what isn't tested. The coordinating session reviews it and
  sends findings and decisions in to-luau.md, and lands it; don't wait idle on a review, start
  the next step on its own branch.
- Commit as me (hlpdev), a short lowercase message, no Claude attribution or co-author line. No
  force-push without my yes.
- Decide design questions yourself and record them in docs/decisions.md; tell me what changes
  what I approved. Where the plan turns out wrong, fix the plan first and say what changed.
- A bug or gap in FutureBlitz, fb, or std goes to the coordinating session, for the fb lane, with
  a small program that shows it; never worked around silently here.
- Don't watch or poll CI; the coordinator sends red logs. No windows or error dialogs on my PC.
- Ask me first for public acts: tags, releases, publishing to the registry, any post.
- Keep explanations short.

## Conventions
- C++23, CMake, deps via FetchContent, pinned (Luau by its release tag); FutureBlitz's
  .clang-format and .clang-tidy, copied here.
- snake_case; types end in _t; private/protected members use m_; enum constants UPPER_CASE;
  template params CamelCase; never align declarations or assignments; spaces inside braced lists.
- The library exports only `fbl_<area>_<verb>` functions, C, of fixed-size types, with handles as
  `uint64_t`; its C++ runtime is linked in; no iostreams of its own.
- Nothing unwinds across the shim: every exported function catches everything, every Luau call
  that can raise is protected, and a Luau error is raised only from the shim's own frames, never
  through a FutureBlitz one. Luau's API checks nothing in a release build, so the shim checks
  every handle, index, and type.
- An area's C API is native/<area>/fbl_<area>.h with its .cpp files beside it; its bindings,
  luau/c/fbl_<area>.fb, are Package, made by `sh tools/bindgen.sh`.
- The .fb code is written as std's and fbengine's is; every public declaration has a `///` doc,
  and every module a `//!` one. Public names start with `Luau`.

## Layout
- native/<area>/: the shim. cmake/: the library and Luau's build.
- luau/: the package: project.toml, the .fb API, c/ (bindings), tests/ (`fb test luau`),
  notices/, native/<target>/ (built, never checked in).
- tests/: the C++ tests and stability tests; tests/conformance/: Luau's conformance scripts run
  through the package. fuzz/: the fuzzers. examples/: programs ctest runs.
- tools/: verify.py, bindgen.sh, check-exports.sh, check_notices.py, gen_arity.py.

## Current step
The plan and the repository's setup are done (2026-11-11). L0, setup, is on dev/l0-setup, L1, a State, on dev/l1-state, off it, L2, values and tables, on dev/l2-values, off L1, L3, functions both ways, on dev/l3-functions, off L2, L4, types, on dev/l4-types, off L3, L5, vectors and buffers, on dev/l5-vectors, off L4, L6, the sandbox, on dev/l6-sandbox, off L5, L7, threads on the scheduler, on dev/l7-threads, off L6, L8, require, on dev/l8-require, off L7, the review of L1 to L6 on dev/review-1, off L8, with master merged in, L9, bytecode, on dev/l9-bytecode, off it, and L10, native code, on dev/l10-native, off L9.
