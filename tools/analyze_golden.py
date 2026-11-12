"""Writes what luau-analyze gives for each case of tests/check/corpus, its expected.txt, which the
checker's tests compare the shim's diagnostics with: each case's main.luau checked, from the
case's folder, its lines as "path:line:column: name: message" with paths from that folder.

luau-analyze is Luau's own, built from the Luau the library has (the tag cmake/luau.cmake pins),
with LUAU_BUILD_CLI on; it isn't part of the build, so this runs by hand when Luau or the corpus
changes:

    python tools/analyze_golden.py path/to/luau-analyze
"""

import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CORPUS = ROOT / "tests" / "check" / "corpus"
# A diagnostic as luau-analyze's default format writes it: "path(line,column): name: message".
LINE = re.compile(r"^(?P<path>.+?)\((?P<line>\d+),(?P<column>\d+)\): (?P<rest>.*)$")


def relative(path, case):
    """`path` from the case's folder, with "/" between its parts."""
    path = path.replace("\\", "/")
    base = case.as_posix() + "/"
    if path.startswith(base):
        path = path[len(base):]
    if path.startswith("./"):
        path = path[2:]
    return path


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    analyze = str(pathlib.Path(sys.argv[1]).resolve())
    for case in sorted(p for p in CORPUS.iterdir() if p.is_dir()):
        run = subprocess.run([analyze, "main.luau"], cwd=case, capture_output=True, text=True,
                             check=False)
        lines = []
        for line in (run.stdout + run.stderr).splitlines():
            found = LINE.match(line)
            if found:
                # A message that names a module names it by its full path.
                rest = found['rest'].replace(case.as_posix() + "/", "")
                lines.append(f"{relative(found['path'], case)}:{found['line']}:"
                             f"{found['column']}: {rest}")
            elif lines:
                # A message of more than one line goes on.
                lines[-1] += "\n" + line
        (case / "expected.txt").write_text("".join(each + "\n" for each in lines),
                                          encoding="utf-8", newline="\n")
        print(f"{case.name}: {len(lines)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
