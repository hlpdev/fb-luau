"""Checks the package's notices name every third-party library its library has in it, as the build
made it: CMake's file API, which tests/CMakeLists.txt asks for, gives luau_native's targets, what
they link through every step, and their sources and include folders. Each library it links, and
each FetchContent folder its code is compiled from or includes, has the notices its code asks for
in luau/notices/, which the package's README names. A library this doesn't know stops it, so a new
one gets its notice before it ships. MinGW-w64's winpthreads, linked into the Windows library, is
always the package's, as one package has every target's library.

    python tools/check_notices.py <the build's folder> [the repository's folder]
"""

import json
import pathlib
import re
import sys

# The library this checks, and the package whose notices are its.
LIBRARY = "luau_native"
PACKAGE = "luau"
# Each library it links, by its target's name, and the notices its code needs. A name is matched
# whole, as a regular expression.
LINKED = {
    r"Luau\.[A-Za-z]+": ["luau.txt"],
    "fbl_objects": [],
    "Threads::Threads": [],
}
# Each FetchContent folder, _deps/<name>-src, its code is compiled from or includes, and its notices.
FOLDERS = {
    "luau": ["luau.txt"],
}
# The package's always, as cmake/native.cmake links MinGW's winpthreads into the Windows library.
ALWAYS = ["winpthreads.txt"]
# What's no library: the build's own steps.
UTILITY = {"UTILITY"}


def known(table, name):
    """The notices `table` gives `name`, or None if it doesn't know it."""
    for pattern, notices in table.items():
        if re.fullmatch(pattern, name):
            return notices
    return None


def graph(build):
    """The library's targets and what they're made of, from CMake's file API: (the targets it links,
    through every step, and the FetchContent folders they're compiled from or include). None if the
    build has no reply, or no such library."""
    reply = build / ".cmake" / "api" / "v1" / "reply"
    indexes = sorted(reply.glob("index-*.json")) if reply.is_dir() else []
    if not indexes:
        return None
    index = json.loads(indexes[-1].read_text(encoding="utf-8"))
    codemodel = next(entry for entry in index["objects"] if entry["kind"] == "codemodel")
    model = json.loads((reply / codemodel["jsonFile"]).read_text(encoding="utf-8"))
    targets = {target["id"]: target for target in model["configurations"][0]["targets"]}
    library = next((target for target in targets.values() if target["name"] == LIBRARY), None)
    if library is None:
        return None

    def read(target_id):
        return json.loads((reply / targets[target_id]["jsonFile"]).read_text(encoding="utf-8"))

    linked = set()
    folders = set()
    seen = set()
    todo = [library["id"]]
    while todo:
        target_id = todo.pop()
        if target_id in seen:
            continue
        seen.add(target_id)
        made = read(target_id)
        if made["type"] in UTILITY:
            continue
        if target_id != library["id"]:
            linked.add(made["name"])
        todo += [each["id"] for each in made.get("dependencies", [])]
        paths = [each["path"] for each in made.get("sources", [])]
        includes = [each["path"] for group in made.get("compileGroups", []) for each in group.get("includes", [])]
        for path in paths + includes:
            folder = re.search(r"(?:^|/)_deps/([^/]+?)-(?:src|build|subbuild)(?:/|$)", path.replace("\\", "/"))
            if folder:
                folders.add(folder.group(1))
    return sorted(linked), sorted(folders)


def problems(build, root):
    """What's wrong with the notices; a list, empty when they're right."""
    made = graph(build)
    if made is None:
        return [f"{build} has no CMake file API reply with {LIBRARY}: configure it with tests/CMakeLists.txt's query"]
    linked, fetched = made
    folder = root / PACKAGE
    wrong = []
    needed = list(ALWAYS)
    for library in linked:
        notices = known(LINKED, library)
        if notices is None:
            wrong.append(f"{LIBRARY} links {library}, whose notices this doesn't know: add them, and it to LINKED")
            continue
        needed += notices
    for name in fetched:
        notices = known(FOLDERS, name)
        if notices is None:
            wrong.append(f"{LIBRARY} is compiled from _deps/{name}-src, whose notices this doesn't know: add "
                         "them, and it to FOLDERS")
            continue
        needed += notices
    readme_path = folder / "README.md"
    readme = readme_path.read_text(encoding="utf-8") if readme_path.is_file() else ""
    if not readme:
        wrong.append(f"{PACKAGE} has no README.md to name its notices")
    for notice in sorted(set(needed)):
        if not (folder / "notices" / notice).is_file():
            wrong.append(f"{PACKAGE} has no notices/{notice}")
        elif f"notices/{notice}" not in readme:
            wrong.append(f"{PACKAGE}'s README doesn't name notices/{notice}")
    kept = sorted((folder / "notices").iterdir()) if (folder / "notices").is_dir() else []
    for path in kept:
        if path.name not in needed:
            wrong.append(f"{PACKAGE}'s notices/{path.name} is for nothing in {LIBRARY}")
    return wrong


def main():
    if len(sys.argv) not in (2, 3):
        print(__doc__)
        return 2
    build = pathlib.Path(sys.argv[1]).resolve()
    here = pathlib.Path(__file__).resolve().parent.parent
    root = pathlib.Path(sys.argv[2]).resolve() if len(sys.argv) == 3 else here
    wrong = problems(build, root)
    for line in wrong:
        print(line)
    if wrong:
        return 1
    print(f"the notices of {LIBRARY} name every library it links")
    return 0


if __name__ == "__main__":
    sys.exit(main())
