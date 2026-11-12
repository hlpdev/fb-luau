"""Runs each program in docs/guide.md, every ```vb block, as a program of its own on the package,
and fails if one doesn't build or run: the guide's programs are the package's own.

    python tools/guide_programs.py [fb] [scratch folder]
"""

import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
GUIDE = ROOT / "docs" / "guide.md"
BLOCK = re.compile(r"^```vb\n(.*?)^```", re.MULTILINE | re.DOTALL)


def main():
    fb = sys.argv[1] if len(sys.argv) > 1 else "fb"
    scratch = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / "build" / "guide"
    programs = BLOCK.findall(GUIDE.read_text(encoding="utf-8"))
    if not programs:
        print("the guide has no programs")
        return 1
    failed = 0
    for number, program in enumerate(programs, 1):
        folder = scratch / f"program{number}"
        folder.mkdir(parents=True, exist_ok=True)
        (folder / "project.toml").write_text(
            f'[project]\nname = "guide_program{number}"\nversion = "0.1.0"\n\n'
            f'[dependencies]\nluau = {{ path = "{(ROOT / "luau").as_posix()}" }}\n',
            encoding="utf-8")
        (folder / "main.fb").write_text(program, encoding="utf-8", newline="\n")
        run = subprocess.run([fb, "--color", "never", "run", str(folder)], capture_output=True,
                             text=True, check=False)
        first = program.splitlines()[0] if program else ""
        if run.returncode != 0:
            failed += 1
            print(f"program {number} ({first}) failed:\n{run.stdout}{run.stderr}")
        else:
            print(f"program {number} ran")
    print(f"{len(programs) - failed} of {len(programs)} programs ran")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
