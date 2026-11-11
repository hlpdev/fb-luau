"""Writes luau/arity.fb: the package's code that's the same for each number of a function's
arguments, from none to MOST, which FutureBlitz has no variadic generics for: LuauState's
SetFunction and Wrap, for a FutureBlitz function of each shape, and LuauFunction's Call, Invoke,
Call2, Call3, CallAll, ToFunc, and ToAction.

    python tools/gen_arity.py [--check]

--check changes nothing, and fails if luau/arity.fb isn't what this writes.
"""

import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
TARGET = ROOT / "luau" / "arity.fb"
# The most arguments a function of either side takes here.
MOST = 8


def generics(names, constraint="LuauType"):
    """A declaration's type parameters, and its Where lines: one constrained parameter goes inline,
    as FutureBlitz's style has it, and more each on a Where line."""
    if not names:
        return "", []
    if len(names) == 1:
        return f"<{names[0]}: {constraint}>", []
    return f"<{', '.join(names)}>", [f"Where {name}: {constraint}" for name in names]


def header(indent, name, type_names, parameters, result):
    """A Method's first line and its Where lines, under @PanicsAtCaller, with its doc before."""
    type_list, where = generics(type_names)
    lines = [f"{indent}@PanicsAtCaller", f"{indent}Public Method {name}{type_list}({parameters}){result}"]
    lines += [f"{indent}    {line}" for line in where]
    return lines


def doc(indent, text):
    return [f"{indent}/// {line}" if line else f"{indent}///" for line in text.split("\n")]


def arguments(count):
    return [f"A{index}" for index in range(1, count + 1)]


def counted(count):
    return {0: "no arguments", 1: "one argument"}.get(count, f"{count} arguments")


def state_methods(count):
    """LuauState's SetFunction and Wrap for a function of `count` arguments, of each shape: giving a
    result or not, and throwing or not."""
    names = arguments(count)
    lines = []
    for gives in (True, False):
        for throws in (False, True):
            func = f"Func({', '.join(names)})" + (": R" if gives else "") + (" Throws Error" if throws else "")
            type_names = names + (["R"] if gives else [])
            what = ("that gives a result" if gives else "that gives nothing") + (", and throws" if throws else "")
            # SetFunction.
            lines += doc("    ", f"Sets the global `name` to `f`, a function of {counted(count)} {what}, as a Luau\n"
                                 "function, as [Wrap] makes it.\n\n# Errors\n\nA LuauError of MEMORY.")
            lines += header("    ", "SetFunction", type_names, f"name: String, f: {func}", " Throws LuauError")
            lines.append("        Try Set(name, Try Wrap(f, name))")
            lines.append("    End Method")
            lines.append("")
            # Wrap.
            lines += doc("    ", f"`f`, a function of {counted(count)} {what}, as a Luau function, named `name` in\n"
                                 "Luau's errors and tracebacks: its arguments are read as their types, a missing one as\n"
                                 "nil, and an argument of another type is an error in the script, \"invalid argument #1\n"
                                 "to 'name' (expected Int32, got string)\"; its result is given back as its type; and an\n"
                                 "error it throws is raised in the script, as its Message after where it was called,\n"
                                 "which a script's pcall catches, and which comes back out of the program's call as the\n"
                                 "LuauError's Cause.\n\n# Errors\n\nA LuauError of MEMORY.")
            lines += header("    ", "Wrap", type_names, f"f: {func}, name: String = \"function\"",
                            ": LuauFunction Throws LuauError")
            lines.append("        Return Try NewFunction(name, Function(slots: LuauSlots): Int32")
            if count == 0 and not gives and not throws:
                # Nothing in it throws, so there's nothing to catch.
                lines.append("            f()")
                lines.append("            Return slots.NoResult()")
                lines.append("        End Function)")
                lines.append("    End Method")
                lines.append("")
                continue
            lines.append("            Try")
            for index, name in enumerate(names):
                lines.append(f"                Local a{index + 1} = Try slots.Argument<{name}>({index}, name)")
            call = f"f({', '.join(f'a{index}' for index in range(1, count + 1))})"
            if throws:
                call = "Try " + call
            if gives:
                lines.append(f"                Return Try slots.Result({call})")
            else:
                lines.append(f"                {call}")
                lines.append("                Return slots.NoResult()")
            lines.append("            Catch error")
            lines.append("                Return slots.Raise(error)")
            lines.append("            End Try")
            lines.append("        End Function)")
            lines.append("    End Method")
            lines.append("")
    return lines


def pushes(count):
    return [f"        Try A{index}.ToLuau(slots, a{index})" for index in range(1, count + 1)]


def function_methods(count):
    """LuauFunction's calls with `count` arguments."""
    names = arguments(count)
    parameters = ", ".join(f"a{index}: A{index}" for index in range(1, count + 1))
    passed = ", ".join(f"a{index}" for index in range(1, count + 1))
    with_arguments = f"with {counted(count)}" if count else "with no arguments"
    errors = "\n\n# Errors\n\nA LuauError for what it raises or meets as it runs, or of CONVERSION when a result isn't\nof its type."
    lines = []

    def call(results, kinds):
        body = ["        Local slots = Try Opened()", "        Defer Owner.Closed(slots.Frame)"]
        body += pushes(count)
        body.append(f"        Try Owner.Check(fbl_call_function(Owner.Handle, slots.Frame, 0, {results}))")
        return body

    # Call<R>.
    lines += doc("    ", f"Calls it {with_arguments}, and gives its first result as an R, or nil's when it gives\n"
                         "none." + errors)
    lines += header("    ", "Call", ["R"] + names, parameters, ": R Throws LuauError")
    lines += call(1, None)
    lines.append("        Return Try R.FromLuau(slots, 0)")
    lines += ["    End Method", ""]
    # Invoke.
    lines += doc("    ", f"Calls it {with_arguments}, and lets its results go.\n\n# Errors\n\n"
                         "A LuauError for what it raises or meets as it runs.")
    lines += header("    ", "Invoke", names, parameters, " Throws LuauError")
    lines += call(0, None)
    lines += ["    End Method", ""]
    # Call2 and Call3.
    for many in (2, 3):
        results = [f"R{index}" for index in range(1, many + 1)]
        reads = ", ".join(f"Try R{index}.FromLuau(slots, {index - 1})" for index in range(1, many + 1))
        lines += doc("    ", f"Calls it {with_arguments}, and gives its first {many} results, each as its type." + errors)
        lines += header("    ", f"Call{many}", results + names, parameters, f": ({', '.join(results)}) Throws LuauError")
        lines += call(many, None)
        lines.append(f"        Return ({reads})")
        lines += ["    End Method", ""]
    # CallAll.
    lines += doc("    ", f"Calls it {with_arguments}, and gives every result it gives, of any types.\n\n# Errors\n\n"
                         "A LuauError for what it raises or meets as it runs.")
    lines += header("    ", "CallAll", names, parameters, ": LuauValue[] Throws LuauError")
    lines += call(-1, None)
    lines.append("        Local results: LuauValue[] = []")
    lines.append("        For index = 0 Until slots.Count()")
    lines.append("            results.Add(Try LuauValue.FromLuau(slots, index))")
    lines.append("        End For")
    lines.append("        Return results")
    lines += ["    End Method", ""]
    # ToFunc and ToAction.
    signature = ", ".join(names)
    lines += doc("    ", f"It as a FutureBlitz function of {counted(count)} that gives a result, which calls it\n"
                         "as Call does, and keeps it alive while it's held.")
    lines += header("    ", "ToFunc", names + ["R"], "", f": (Func({signature}): R Throws LuauError)")
    lines.append(f"        Return Function({parameters}): R Throws LuauError")
    lines.append(f"            Return Try Call<{', '.join(['R'] + names)}>({passed})")
    lines.append("        End Function")
    lines += ["    End Method", ""]
    lines += doc("    ", f"It as a FutureBlitz function of {counted(count)} that gives nothing, which calls it\n"
                         "as Invoke does, and keeps it alive while it's held.")
    lines += header("    ", "ToAction", names, "", f": (Func({signature}) Throws LuauError)")
    lines.append(f"        Return Function({parameters}) Throws LuauError")
    lines.append(f"            Try Invoke({passed})")
    lines.append("        End Function")
    lines += ["    End Method", ""]
    return lines


def generate():
    lines = [
        "//! What's the same for each number of a function's arguments, from none to "
        f"{MOST}: a State's",
        "//! SetFunction and Wrap, and a LuauFunction's calls. tools/gen_arity.py writes it; run it again",
        "//! instead of editing it.",
        "",
        "Import std.core",
        "Import \"./c/fbl_call\"",
        "Import \"./errors\"",
        "Import \"./functions\"",
        "Import \"./state\"",
        "Import \"./value\"",
        "Import \"./values\"",
        "",
        "/// A State's functions both ways: the program's, given to Luau.",
        "Extend LuauState",
    ]
    for count in range(MOST + 1):
        lines += state_methods(count)
    if lines[-1] == "":
        lines.pop()
    lines += ["End Extend", "", "/// A Luau function's calls from the program.", "Extend LuauFunction"]
    for count in range(MOST + 1):
        lines += function_methods(count)
    if lines[-1] == "":
        lines.pop()
    lines.append("End Extend")
    return formatted("\n".join(lines) + "\n")


def formatted(text):
    """`text` as `fb fmt` writes it, so what's checked in is what the formatter keeps."""
    fb = shutil.which("fb")
    if fb is None:
        sys.exit("gen_arity.py needs fb on PATH, to format what it writes")
    with tempfile.TemporaryDirectory() as folder:
        path = pathlib.Path(folder) / "arity.fb"
        path.write_text(text, encoding="utf-8", newline="\n")
        result = subprocess.run([fb, "fmt", str(path)], capture_output=True, text=True, check=False)
        if result.returncode != 0:
            sys.exit(f"fb fmt failed on what gen_arity.py writes:\n{result.stdout}{result.stderr}")
        return path.read_text(encoding="utf-8").replace("\r\n", "\n")


def main():
    made = generate()
    if "--check" in sys.argv[1:]:
        kept = TARGET.read_text(encoding="utf-8").replace("\r\n", "\n") if TARGET.exists() else ""
        if kept != made:
            print(f"{TARGET.relative_to(ROOT).as_posix()} isn't what tools/gen_arity.py writes; run it")
            return 1
        print("luau/arity.fb is what tools/gen_arity.py writes")
        return 0
    with open(TARGET, "w", encoding="utf-8", newline="\n") as file:
        file.write(made)
    print(f"wrote {TARGET.relative_to(ROOT).as_posix()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
