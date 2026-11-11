# Security

## Reporting a vulnerability

Please don't report a vulnerability in a public issue. Report it privately, on GitHub, at
[Report a vulnerability](https://github.com/hlpdev/fb-luau/security/advisories/new) (the
repository's Security tab, then "Report a vulnerability"). Only you and the maintainers see it.

Say what's affected and in which version of the package, how to reproduce it, ideally with a
small program and the Luau script that does it, and what someone could do with it. You'll hear
back within a week, and as the fix goes on.

## What's covered

The `luau` package and its library: above all its sandbox, since it runs scripts a program
didn't write, as a game's mods. A script that gets out of its sandbox, reads or writes what it
wasn't given, gets past its memory limit or its time budget, or crashes or hangs the program
that runs it, is a vulnerability. So is a crash or a leak in what reads a script's source.

Loading bytecode is trusted, as Luau's own docs say: a program loads only bytecode it compiled
itself, never a mod's, so bytecode from elsewhere isn't covered. A vulnerability in Luau itself
goes to [Luau's](https://github.com/luau-lang/luau/security), and one in FutureBlitz, fb, std, or
the package registry to [FutureBlitz's](https://github.com/hlpdev/FutureBlitz/security/advisories/new).

## Supported versions

The latest release gets security fixes, as a new release of the package.

## What happens

The maintainers confirm the report, fix it in private, and release the fix, then publish a GitHub
security advisory with a CVE, crediting you unless you'd rather not be. Please keep it private
until the advisory is out.
