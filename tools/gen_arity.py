"""Writes luau/arity.fb: the package's code that's the same for each number of a function's
arguments, from none to MOST, which FutureBlitz has no variadic generics for: LuauState's
SetFunction and Wrap, and SetAsync and WrapAsync, for a FutureBlitz function of each shape,
LuauFunction's Call, Invoke, Call2, Call3, CallAll, ToFunc, and ToAction, and LuauThread's
Resume.

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
            lines.append("        Local types: String[] = [" + ", ".join(f"TypeName<{name}>()" for name in names) + "]")
            lines.append("        DeclareFunction(name, types, " + ("TypeName<R>()" if gives else "Null") + ")")
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


def async_methods(count):
    """LuauState's SetAsync and WrapAsync for a function of `count` arguments that gives a Task, of
    each shape: a Task of a result or not, which throws or doesn't."""
    names = arguments(count)
    lines = []
    for gives in (True, False):
        for throws in (False, True):
            task = ("Task<R>" if gives else "Task") + (" Throws Error" if throws else "")
            func = f"Func({', '.join(names)}): " + (f"({task})" if throws else task)
            type_names = names + (["R"] if gives else [])
            what = "that gives a Task" + (" of a result" if gives else "") + (", which throws" if throws else "")
            lines += doc("    ", f"Sets the global `name` to `f`, a function of {counted(count)} {what}, as a Luau\n"
                                 "function, as [WrapAsync] makes it.\n\n# Errors\n\nA LuauError of MEMORY.")
            lines += header("    ", "SetAsync", type_names, f"name: String, f: {func}", " Throws LuauError")
            lines.append("        Try Set(name, Try WrapAsync(f, name))")
            lines.append("        Local types: String[] = [" + ", ".join(f"TypeName<{name}>()" for name in names) + "]")
            lines.append("        DeclareFunction(name, types, " + ("TypeName<R>()" if gives else "Null") + ")")
            lines.append("    End Method")
            lines.append("")
            lines += doc("    ", f"`f`, a function of {counted(count)} {what}, an Async Function's, as a Luau\n"
                                 "function, named `name` in Luau's errors and tracebacks: its arguments are read as\n"
                                 "Wrap reads them, and a script that calls it waits for the Task as an Await does,\n"
                                 "while the program goes on, then has its result" + (", or its error raised where it\n"
                                 "called it, as Wrap raises one." if throws else ".") + " Only a thread on the scheduler\n"
                                 "can wait: a call of it on a call of the program's, or in a metamethod, is an error\n"
                                 "of YIELD in the script, before it calls `f`.\n\n# Errors\n\nA LuauError of MEMORY.")
            lines += header("    ", "WrapAsync", type_names, f"f: {func}, name: String = \"function\"",
                            ": LuauFunction Throws LuauError")
            lines.append("        Return Try NewFunction(name, Function(slots: LuauSlots): Int32")
            lines.append("            Try")
            lines.append("                Try slots.Yieldable(name)")
            for index, name in enumerate(names):
                lines.append(f"                Local a{index + 1} = Try slots.Argument<{name}>({index}, name)")
            call = f"f({', '.join(f'a{index}' for index in range(1, count + 1))})"
            lines.append(f"                Return Try slots.Awaited(Settled({call}))")
            lines.append("            Catch error")
            lines.append("                Return slots.Raise(error)")
            lines.append("            End Try")
            lines.append("        End Function)")
            lines.append("    End Method")
            lines.append("")
    return lines


def thread_methods(count):
    """LuauThread's Resume with `count` arguments."""
    names = arguments(count)
    parameters = ", ".join(f"a{index}: A{index}" for index in range(1, count + 1))
    with_arguments = f"with {counted(count)}" if count else "with no arguments"
    lines = doc("    ", f"Resumes it {with_arguments}, as `coroutine.resume` does: the first time, they're its\n"
                         "function's arguments, and after, what the `coroutine.yield` it waits at gives. It runs\n"
                         "until it yields or returns, and what it yields or returns comes back, of any types.\n\n"
                         "# Errors\n\nA LuauError for what it raises or meets as it runs, which ends it, or of\n"
                         "RUNTIME for a thread that isn't suspended, or that waits, which the scheduler resumes.")
    lines += header("    ", "Resume", names, parameters, ": LuauValue[] Throws LuauError")
    lines.append("        If Owner.Waits.Contains(Id)")
    lines.append("            Throw LuauError(LuauErrorKind.RUNTIME, \"the Luau thread waits on the scheduler, which \" +")
    lines.append("                            \"resumes it\")")
    lines.append("        End If")
    lines.append("        Local slots = Try Opened()")
    lines.append("        Defer Owner.Closed(slots.Frame)")
    lines += pushes(count)
    lines.append("        Return (Try Ran(slots, 0, True)).1")
    lines += ["    End Method", ""]
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


def invoker(indent, reads, call, gives, throws):
    """A lambda that runs a member for a call of it from Luau: reads its slots, calls, and gives its
    result or raises its error."""
    lines = [f"{indent}Function(slots: LuauSlots): Int32", f"{indent}    Try"]
    lines += [f"{indent}        {read}" for read in reads]
    call = ("Try " if throws else "") + call
    if gives:
        lines.append(f"{indent}        Return Try slots.Result({call})")
    else:
        lines.append(f"{indent}        {call}")
        lines.append(f"{indent}        Return slots.NoResult()")
    lines += [f"{indent}    Catch error", f"{indent}        Return slots.Raise(error)", f"{indent}    End Try",
              f"{indent}End Function"]
    return lines


def loop(attribute, func, type_names, throws):
    """The head of a For Method loop over T's Methods of `func`'s shape that `attribute` marks."""
    lines = ["    For Method m: M In T", f"        Where m Has {attribute}", f"        Where M: {func}"]
    lines += [f"        Where {name}: LuauType" for name in type_names]
    if throws:
        lines.append("        Where E: Error")
    lines.append(f"        Local name = MemberName(m.Attribute({attribute})?.Name, m.Name)")
    kind = {"LuauExpose": 0, "LuauAction": 0, "LuauGet": 1, "LuauSet": 2}[attribute]
    lines.append(f"        type.Claim({kind}, name, m.Name)")
    # Its types, for Definitions.
    taken = [name for name in type_names if name not in ("R",)]
    if attribute == "LuauSet":
        lines.append("        Local types: String[] = [TypeName<V>()]")
        lines.append("        type.Declare(LuauDeclaredKind.SETTER, name, types, Null)")
    elif attribute == "LuauGet":
        lines.append("        Local types: String[] = []")
        lines.append("        type.Declare(LuauDeclaredKind.GETTER, name, types, TypeName<R>())")
    else:
        listed = ", ".join(f"TypeName<{name}>()" for name in taken)
        lines.append(f"        Local types: String[] = [{listed}]")
        result = "TypeName<R>()" if "R" in type_names else "Null"
        lines.append(f"        type.Declare(LuauDeclaredKind.METHOD, name, types, {result})")
    return lines


def expose_members():
    """ExposeMembers, which goes over a registered type's Methods by reflection, a loop for each
    shape a Method can have."""
    lines = doc("    ", "Registers T, whose objects cross as themselves, and exposes what its reflection says a\n"
                        "script reaches: its Methods marked [LuauExpose], [LuauAction], [LuauGet], and\n"
                        "[LuauSet], each of LuauTypes, and its ToString, for tostring. Registering it again\n"
                        "gives the same builder.\n\n# Errors\n\nA LuauError of MEMORY.\n\n# Panics\n\n"
                        "For a marked Method of a shape none exposes, like one of 8 arguments, or one that\n"
                        "gives nothing marked [LuauExpose], and for two marked members of one name in Luau,\n"
                        "saying which. A marked Static, ByRef, or Unsafe Method isn't exposed, and isn't\n"
                        "said, as FutureBlitz's reflection doesn't go over them. And for a type of the\n"
                        "name, TypeName's, of another registered one: scripts, their errors, and the\n"
                        "checker tell types by their names.")
    lines += ["    @PanicsAtCaller",
              "    Public Method Register<T: LuauObject>(): LuauTypeBuilder<T> Throws LuauError",
              "        Local kept = KeptType<T>()",
              "        If kept != Null Then Return kept",
              "        If DeclaredTypes.Contains(TypeName<T>())",
              "            Panic(\"another type named {TypeName<T>()} is registered, and Luau tells types \" +",
              "                  \"by their names\")",
              "        End If",
              "        Local type = NewType<T>()"]
    body = []
    for count in range(MOST):
        names = arguments(count)
        reads = ["Local object = Try slots.Argument<T>(0, name)"]
        reads += [f"Local a{index} = Try slots.Argument<A{index}>({index}, name)" for index in range(1, count + 1)]
        call = f"m.Call({', '.join(['object'] + [f'a{index}' for index in range(1, count + 1)])})"
        for gives in (True, False):
            for throws in (False, True):
                func = f"Func({', '.join(names)})" + (": R" if gives else "") + (" Throws E" if throws else "")
                body += loop("LuauExpose" if gives else "LuauAction", func, names + (["R"] if gives else []),
                             throws)
                made = invoker("        ", reads, call, gives, throws)
                made[0] = "        type.Methods[name] = " + made[0].strip()
                body += made
                body.append("    End For")
    for throws in (False, True):
        body += loop("LuauGet", "Func(): R" + (" Throws E" if throws else ""), ["R"], throws)
        made = invoker("        ", ["Local object = Try slots.Argument<T>(0, name)"], "m.Call(object)", True, throws)
        made[0] = "        type.Getters[name] = " + made[0].strip()
        body += made
        body.append("    End For")
        body += loop("LuauSet", "Func(V)" + (" Throws E" if throws else ""), ["V"], throws)
        reads = ["Local object = Try slots.Argument<T>(0, name)", "Local value = Try slots.Argument<V>(1, name)"]
        made = invoker("        ", reads, "m.Call(object, value)", False, throws)
        made[0] = "        type.Setters[name] = " + made[0].strip()
        body += made
        body.append("    End For")
    body += ["    For Method m: M In T", "        Where M: Func(): String", "        If m.Name != \"ToString\" Then Continue"]
    made = invoker("        ", ["Local object = Try slots.Argument<T>(0, \"__tostring\")"], "m.Call(object)", True, False)
    made[0] = "        Local show = " + made[0].strip()
    body += made
    body += ["        type.Pending.Add((LuauOperator.TOSTRING, Try NewFunction(\"__tostring\", show)))",
             "    End For"]
    # A marked Method no loop above took is named in a panic: a loop of each shape, of any types,
    # finds it, as a For Method loop goes over one shape.
    for attribute, kind in (("LuauExpose", 0), ("LuauAction", 0), ("LuauGet", 1), ("LuauSet", 2)):
        for count in range(MOST + 1):
            names = arguments(count)
            for gives in (True, False):
                for throws in (False, True):
                    func = f"Func({', '.join(names)})" + (": R" if gives else "") + (" Throws E" if throws else "")
                    body += ["    For Method m: M In T", f"        Where m Has {attribute}", f"        Where M: {func}"]
                    if throws:
                        body.append("        Where E: Error")
                    body += [f"        type.Unexposed(m.Name, \"{attribute}\", {kind}, "
                             f"MemberName(m.Attribute({attribute})?.Name, m.Name))",
                             "    End For"]
    lines += ["    " + line for line in body]
    lines += ["        Try Finished(type)", "        Return type", "    End Method"]
    return lines


def generate():
    lines = [
        "//! What's the same for each number of a function's arguments, from none to "
        f"{MOST}: a State's",
        "//! SetFunction, Wrap, SetAsync, and WrapAsync, a LuauFunction's calls, and a LuauThread's",
        "//! resumes. tools/gen_arity.py writes it; run it again instead of editing it.",
        "",
        "Import std.core",
        "Import \"./c/fbl_call\"",
        "Import \"./definitions\"",
        "Import \"./errors\"",
        "Import \"./functions\"",
        "Import \"./objects\"",
        "Import \"./state\"",
        "Import \"./threads\"",
        "Import \"./value\"",
        "Import \"./values\"",
        "",
        "/// A State's functions both ways: the program's, given to Luau.",
        "Extend LuauState",
    ]
    for count in range(MOST + 1):
        lines += state_methods(count)
    for count in range(MOST + 1):
        lines += async_methods(count)
    lines += expose_members()
    lines += ["End Extend", "", "/// A Luau function's calls from the program.", "Extend LuauFunction"]
    for count in range(MOST + 1):
        lines += function_methods(count)
    if lines[-1] == "":
        lines.pop()
    lines += ["End Extend", "", "/// A Luau thread's resumes by hand.", "Extend LuauThread"]
    for count in range(MOST + 1):
        lines += thread_methods(count)
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
