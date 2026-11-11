"""Tests tools/check_notices.py on builds and packages made up for each case, so it's known to fail
when it should: a library it doesn't know, a fetched folder it doesn't know, a notice missing, one
the README doesn't name, one that's for nothing, and a build with no reply.

    python tools/check_notices_test.py
"""

import json
import pathlib
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import check_notices  # noqa: E402


def make_build(folder, linked, sources):
    """A build folder whose file API reply has luau_native linking `linked`, each a static library,
    compiled from `sources`."""
    reply = folder / ".cmake" / "api" / "v1" / "reply"
    reply.mkdir(parents=True)
    targets = []
    names = ["luau_native", *linked]
    for number, name in enumerate(names):
        target_id = f"{name}::@{number}"
        made = {
            "name": name,
            "type": "SHARED_LIBRARY" if name == "luau_native" else "STATIC_LIBRARY",
            "dependencies": [{"id": f"{other}::@{names.index(other)}"} for other in linked]
            if name == "luau_native" else [],
            "sources": [{"path": path} for path in (sources if name == "luau_native" else [])],
        }
        json_file = f"target-{number}.json"
        (reply / json_file).write_text(json.dumps(made), encoding="utf-8")
        targets.append({"id": target_id, "name": name, "jsonFile": json_file})
    (reply / "codemodel.json").write_text(json.dumps({"configurations": [{"targets": targets}]}), encoding="utf-8")
    (reply / "index-1.json").write_text(json.dumps({"objects": [{"kind": "codemodel", "jsonFile": "codemodel.json"}]}),
                                        encoding="utf-8")


def make_package(root, notices, readme):
    folder = root / "luau"
    (folder / "notices").mkdir(parents=True)
    for notice in notices:
        (folder / "notices" / notice).write_text("text\n", encoding="utf-8")
    (folder / "README.md").write_text(readme, encoding="utf-8")


FINE_README = "Luau (notices/luau.txt), and winpthreads (notices/winpthreads.txt).\n"
FINE_SOURCES = ["native/core/core.cpp", "build/_deps/luau-src/VM/src/lapi.cpp"]


def case(name, linked, sources, notices, readme, expected, reply=True):
    """Whether check_notices finds what `expected` says, a piece of each problem, or none."""
    with tempfile.TemporaryDirectory() as scratch:
        root = pathlib.Path(scratch)
        build = root / "build"
        build.mkdir()
        if reply:
            make_build(build, linked, sources)
        make_package(root, notices, readme)
        found = check_notices.problems(build, root)
    if len(found) != len(expected) or any(piece not in line for piece, line in zip(expected, found)):
        print(f"{name}: expected {expected}, found {found}")
        return False
    return True


def main():
    fine = ["luau.txt", "winpthreads.txt"]
    results = [
        case("all named", ["Luau.VM", "Luau.Compiler"], FINE_SOURCES, fine, FINE_README, []),
        case("an unknown library", ["Luau.VM", "zlib"], FINE_SOURCES, fine, FINE_README, ["links zlib"]),
        case("an unknown folder", ["Luau.VM"], [*FINE_SOURCES, "build/_deps/zstd-src/lib/zstd.c"], fine,
             FINE_README, ["_deps/zstd-src"]),
        case("a missing notice", ["Luau.VM"], FINE_SOURCES, ["winpthreads.txt"], FINE_README,
             ["has no notices/luau.txt"]),
        case("one the README doesn't name", ["Luau.VM"], FINE_SOURCES, fine, "notices/winpthreads.txt\n",
             ["doesn't name notices/luau.txt"]),
        case("one for nothing", ["Luau.VM"], FINE_SOURCES, [*fine, "zstd.txt"], FINE_README,
             ["notices/zstd.txt is for nothing"]),
        case("no reply", [], [], fine, FINE_README, ["has no CMake file API reply"], reply=False),
    ]
    if not all(results):
        return 1
    print(f"all {len(results)} cases hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
