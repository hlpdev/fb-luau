# Contributing

Thanks for helping with fb-luau. Bug reports, ideas, and pull requests are all welcome. Everyone
in the project's spaces keeps to the [code of conduct](CODE_OF_CONDUCT.md).

## Issues

[Open an issue](https://github.com/hlpdev/fb-luau/issues/new/choose) for a bug in the package or
an idea for it. Look for an existing one first, and for a bug, give the smallest program and
script that show it, the package's version, and what `fb --version` prints. A bug in Luau itself,
one that its own CLI shows too, goes to [Luau's issues](https://github.com/luau-lang/luau/issues);
one in the language, fb, or std to
[FutureBlitz's issues](https://github.com/hlpdev/FutureBlitz/issues/new/choose); and a
vulnerability is reported privately, as [SECURITY.md](SECURITY.md) says.

A new issue gets `needs-triage`. A maintainer marks it `confirmed`, asks for a program that shows
it with `needs-repro`, or closes it as a `duplicate` or `wontfix`. `good first issue` marks a small
one that's a good start, and `help wanted` one that's open to anyone who'd like to take it.

## Pull requests

Any pull request is welcome. For a new feature, or a change to the package's API, opening an
issue first lets it be talked through before you write it; [docs/plan.md](docs/plan.md) has how
the package fits together, and [docs/decisions.md](docs/decisions.md) why.

A pull request:

- Builds and passes its tests on Windows, Linux, and macOS, as CI checks.
- Comes with a test for what it fixes or adds.
- Follows the style: FutureBlitz's `.clang-format` and `.clang-tidy`, copied here, for C and C++,
  and `fb fmt` for .fb. The library exports only its C API, `fbl_<area>_<verb>` functions, and
  nothing in it lets a Luau error unwind into FutureBlitz code.
- Keeps to one change, so it's easy to review.

Commit messages are short and lowercase, saying what changed, with the area first when it helps:
`tables: a table's length`.

## Building and testing

[Building](README.md#building) has what fb-luau needs. `tools/verify.py` runs every check, as a
landing does: the build, ctest, the export check, the bindings, clang-format, clang-tidy on what
changed, `fb fmt --check`, `fb doc --check`, and `fb test`:

```sh
python tools/verify.py
```

## License

fb-luau is under the [Apache License 2.0](LICENSE), and what you contribute is under it too, as
its section 5 says. Luau, which it builds into its library, is under the MIT License, whose
notice ships with the package, in `luau/notices/`.
