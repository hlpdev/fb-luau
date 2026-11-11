"""Checks fb-luau before a step lands: everything a landing needs.

    python tools/verify.py [--build <folder>] [--base <ref>] [--no-build] [--no-tidy] [--only <regex>]

It builds (configuring the folder first if it's new), runs ctest (the C++ tests, `fb test luau`, and
the tools' tests), checks the library exports only its C API and loads only the system's libraries,
checks the bindings are what tools/bindgen.sh makes, runs clang-format on every C and C++ file,
clang-tidy on the C++ files that changed since --base (all of an area's when its header changed),
`fb fmt --check`, and `fb doc --check` on the package. It ends with "everything passed", or how
many checks failed.

Four verifies run at a time on a machine: each waits for its turn before it builds. Its build
folder is held the whole run, so a second verify of that folder is refused. --only runs just the
tests matching a regular expression, then the lint, and never prints "everything passed", which
only the whole check does.

clang-format and clang-tidy come from $CLANG_FORMAT and $CLANG_TIDY, or PATH, or where the user's
machine has them. On Windows the build's compiler is $FBL_TOOLCHAIN's, or the UCRT GCC the user's
machine has, put first on PATH for the build and the tests.
"""

import argparse
import hashlib
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
FALLBACKS = {
    "CLANG_FORMAT": "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/Llvm/x64/bin/clang-format.exe",
    "CLANG_TIDY": "C:/Users/Hunter/AppData/Local/Programs/CLion/bin/clang/win/x64/bin/clang-tidy.exe",
    "FBL_TOOLCHAIN": "C:/Users/Hunter/.local/toolchains/gcc16-ucrt/mingw64/bin",
}
# The clang-format CI pins (ci.yml), which every check formats with.
CLANG_FORMAT_VERSION = "19.1.5"
# Tests ctest has to list, as one registered only by a second configure is skipped without a word.
REQUIRED_TESTS = ["luau.package", "tools.check_exports", "tools.check_exports_refuses", "tools.check_notices",
                  "tools.check_notices_test"]
failures = []
# A build's jobs: each compile of Luau's Analysis holds about a gigabyte.
BUILD_JOBS = 6
# One lock for each verify that may run at a time on this machine.
TURNS = ["turn.lock", "turn2.lock", "turn3.lock", "turn4.lock"]


def step(name):
    print(f"\n== {name}", flush=True)


def fail(message):
    print(f"FAIL: {message}", flush=True)
    failures.append(message)


def try_lock(file):
    """Takes `file`'s first byte for this process, until it exits; if it could."""
    try:
        if os.name == "nt":
            import msvcrt

            file.seek(0)
            msvcrt.locking(file.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl

            fcntl.flock(file, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        return False
    return True


def no_error_dialogs():
    """On Windows, a program that fails to start or crashes says so by its exit code, not a dialog
    on the user's screen. What this process starts inherits it, through ctest and fb."""
    if os.name == "nt":
        import ctypes

        # SEM_FAILCRITICALERRORS, SEM_NOGPFAULTERRORBOX, SEM_NOOPENFILEERRORBOX
        ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)


def use_toolchain():
    """On Windows, the UCRT GCC first on PATH, for the build and what the tests run."""
    if os.name != "nt":
        return
    folder = os.environ.get("FBL_TOOLCHAIN") or FALLBACKS["FBL_TOOLCHAIN"]
    if pathlib.Path(folder).is_dir():
        os.environ["PATH"] = folder + os.pathsep + os.environ["PATH"]


def lock_file(name):
    folder = pathlib.Path(tempfile.gettempdir()) / "fb-luau-verify"
    folder.mkdir(exist_ok=True)
    # Kept open, as closing it lets go.
    return (folder / name).open("a+b")


def hold_build(build):
    """Holds the build folder for this run, or refuses: two verifies of one folder undo each other."""
    name = hashlib.sha256(str(build).lower().encode()).hexdigest()[:16]
    file = lock_file(f"build-{name}.lock")
    if not try_lock(file):
        sys.exit(f"another verify is running in {build}")
    return file


def wait_for_turn():
    """Waits for one of the machine's turns, and holds it until this verify exits."""
    files = [lock_file(name) for name in TURNS]
    waited = False
    while True:
        for file in files:
            if try_lock(file):
                return file
        if not waited:
            print("waiting for the other verifies on this machine to finish", flush=True)
            waited = True
        time.sleep(5)


def run(command, **options):
    """Runs `command`, its output shown as it goes; its exit code."""
    return subprocess.run(command, cwd=options.pop("cwd", ROOT), check=False, **options).returncode


def output(command):
    return subprocess.run(command, cwd=ROOT, capture_output=True, text=True, check=False).stdout


def tool(variable, name):
    found = os.environ.get(variable) or shutil.which(name)
    if found is None and pathlib.Path(FALLBACKS[variable]).exists():
        found = FALLBACKS[variable]
    return found


def sources():
    """Every C and C++ file of ours, tracked or new, not a dependency's."""
    patterns = ["*.h", "*.hpp", "*.c", "*.cpp"]
    files = output(["git", "ls-files", *patterns]).split()
    files += output(["git", "ls-files", "--others", "--exclude-standard", *patterns]).split()
    return sorted({file for file in files if not file.startswith("build")})


def pinned_clang_format():
    """clang-format at the version CI pins, which formats as CI does; another version formats some
    lines differently, so it's never used."""
    candidates = [os.environ.get("CLANG_FORMAT"), FALLBACKS["CLANG_FORMAT"], shutil.which("clang-format")]
    for candidate in candidates:
        if candidate and (pathlib.Path(candidate).exists() or shutil.which(candidate)):
            version = output([candidate, "--version"])
            if f"version {CLANG_FORMAT_VERSION}" in version:
                return candidate
    return None


def changed_cpp(base):
    """The .cpp files to tidy: those that changed since `base`, and every .cpp of an area whose
    header changed."""
    changed = set(output(["git", "diff", "--name-only", base, "--", "*.h", "*.hpp", "*.cpp"]).split())
    changed |= set(output(["git", "ls-files", "--others", "--exclude-standard", "*.h", "*.hpp", "*.cpp"]).split())
    files = {file for file in changed if file.endswith(".cpp") and (ROOT / file).exists()}
    for header in changed:
        if header.endswith((".h", ".hpp")):
            area = pathlib.PurePosixPath(header).parent
            files |= set(output(["git", "ls-files", f"{area}/*.cpp"]).split())
    return sorted(files)


def check_build(build):
    step("build")
    if not (build / "CMakeCache.txt").exists():
        command = ["cmake", "-S", ".", "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Debug"]
        if os.name == "nt":
            command += ["-DCMAKE_C_COMPILER=gcc", "-DCMAKE_CXX_COMPILER=g++"]
        if run(command) != 0:
            fail("cmake couldn't configure")
            return False
    if run(["cmake", "--build", str(build), "--parallel", str(BUILD_JOBS)]) != 0:
        fail("the build failed")
        return False
    return True


def required_fb(ci_text):
    """The FutureBlitz CI tests with, by its workflow's FB_VERSION."""
    found = re.search(r"^\s*FB_VERSION:\s*([0-9][0-9.]*)\s*$", ci_text, re.MULTILINE)
    return found.group(1) if found else None


def fb_version_of(said):
    """The version an fb says with --version: "fb 1.2.3 (c77a32a, windows-x64)" is 1.2.3."""
    found = re.match(r"fb ([0-9][0-9.]*)", said.strip())
    return found.group(1) if found else None


def cached_fb(build):
    """The fb a build folder's tests run, as CMake found it when it first configured."""
    cache = pathlib.Path(build) / "CMakeCache.txt"
    if not cache.exists():
        return None
    found = re.search(r"^FBL_FB:FILEPATH=(.*)$", cache.read_text(errors="replace"), re.MULTILINE)
    return found.group(1).strip() if found else None


def check_fb(build):
    """Whether the fb the tests run, and the one on PATH, are CI's; false, with why, if not. One that's
    older runs what's written for the newer as if it were, and its tests fail in ways that look like
    anything but that."""
    step("fb")
    workflow = ROOT / ".github" / "workflows" / "ci.yml"
    required = required_fb(workflow.read_text()) if workflow.exists() else None
    if required is None:
        fail("there's no FB_VERSION in .github/workflows/ci.yml to check fb against")
        return False
    fine = True
    for name, program in (("the build's fb", cached_fb(build)), ("the fb on PATH", shutil.which("fb"))):
        if program is None:
            continue
        if not pathlib.Path(program).is_file():
            fail(f"{name}, {program}, isn't there")
            fine = False
            continue
        version = fb_version_of(output([program, "--version"]))
        if version != required:
            fail(f"{name}, {program}, is FutureBlitz {version}, and CI's is {required}")
            fine = False
    return fine


def check_tests(build, only=None):
    step("ctest")
    listed = re.findall(r"Test +#\d+: (\S+)", output(["ctest", "--test-dir", str(build), "-N"]))
    for name in REQUIRED_TESTS:
        if name not in listed:
            fail(f"ctest doesn't list {name}")
    command = ["ctest", "--test-dir", str(build), "--output-on-failure"]
    if only is not None:
        command += ["--no-tests=error", "-R", only]
    if run(command) != 0:
        fail("ctest failed")


def this_target():
    """This machine's target, as FutureBlitz names it, whose library the build made."""
    system = {"win32": "windows", "darwin": "macos"}.get(sys.platform, "linux")
    machine = platform.machine().lower()
    return f"{system}-{'arm64' if machine in ('arm64', 'aarch64') else 'x64'}"


def check_exports():
    step("exports")
    # Other targets' libraries, from CI's artifacts, are only checked on their own machines.
    folder = ROOT / "luau" / "native" / this_target()
    found = sorted(file for file in folder.glob("*") if file.suffix in (".dll", ".so", ".dylib"))
    if not found:
        fail(f"there's no built library in {folder.relative_to(ROOT)}")
        return
    names = [file.relative_to(ROOT).as_posix() for file in found]
    if run(["sh", "tools/check-exports.sh", "fbl_", *names]) != 0:
        fail("the library exports more than its C API, or loads what isn't the system's")


def check_bindings():
    step("bindings")
    if run(["sh", "tools/bindgen.sh", "--check"]) != 0:
        fail("the bindings aren't what their headers make; run tools/bindgen.sh")


def check_line_ends():
    """A raw carriage return in a CMake file is a regular expression that can't match, silently."""
    step("line ends")
    for path in [ROOT / "CMakeLists.txt", *(ROOT / "cmake").glob("*.cmake"), *ROOT.glob("*/CMakeLists.txt")]:
        if b"\r" in path.read_bytes():
            fail(f"{path.relative_to(ROOT)} has a carriage return")


def check_format():
    step(f"clang-format {CLANG_FORMAT_VERSION}")
    clang_format = pinned_clang_format()
    if clang_format is None:
        fail(f"no clang-format {CLANG_FORMAT_VERSION}, the version CI pins; set CLANG_FORMAT to one")
        return
    files = sources()
    if files and run([clang_format, "--dry-run", "-Werror", *files]) != 0:
        fail("clang-format would change files")


def check_tidy(build, base):
    step(f"clang-tidy on what changed since {base}")
    clang_tidy = tool("CLANG_TIDY", "clang-tidy")
    if clang_tidy is None:
        fail("clang-tidy isn't on PATH; set CLANG_TIDY")
        return
    files = changed_cpp(base)
    if not files:
        print("no C++ changed")
        return
    for file in files:
        result = subprocess.run([clang_tidy, "-p", str(build), "--quiet", file], cwd=ROOT,
                                capture_output=True, text=True, check=False)
        findings = [line for line in result.stdout.splitlines() if "warning:" in line or "error:" in line]
        if findings:
            print("\n".join(findings))
            fail(f"clang-tidy found {len(findings)} in {file}")
        else:
            print(f"ok   {file}")


def check_fb_format():
    step("fb fmt")
    folders = [folder for folder in ("luau", "examples", "tests/conformance") if (ROOT / folder).is_dir()]
    if run(["fb", "fmt", "--check", *folders]) != 0:
        fail("fb fmt would change files")


def check_docs():
    step("fb doc")
    if run(["fb", "doc", "--check", "luau"]) != 0:
        fail("fb doc --check found what isn't documented")


def lint(build, options):
    check_bindings()
    check_line_ends()
    check_format()
    if not options.no_tidy and build is not None:
        check_tidy(build, options.base)
    check_fb_format()
    check_docs()


def finish(passed_line):
    print()
    if failures:
        print(f"{len(failures)} failure(s):")
        for message in failures:
            print(f"  {message}")
        sys.exit(1)
    print(passed_line)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", default="build", help="the build folder (default: build)")
    parser.add_argument("--base", default="origin/master", help="what clang-tidy diffs against")
    parser.add_argument("--no-build", action="store_true", help="use the build as it is")
    parser.add_argument("--no-tidy", action="store_true", help="skip clang-tidy")
    parser.add_argument("--only", help="build, and run only the tests this regular expression matches")
    options = parser.parse_args()
    no_error_dialogs()
    use_toolchain()
    build = (ROOT / options.build).resolve()
    # Held until this process exits.
    held = [hold_build(build), wait_for_turn()]

    built = options.no_build or check_build(build)
    fb_fine = check_fb(build)
    if built and fb_fine:
        check_tests(build, options.only)
        if options.only is None:
            check_exports()
    lint(build if built else None, options)
    finish("those tests and the lint passed" if options.only is not None else "everything passed")
    del held


if __name__ == "__main__":
    main()
