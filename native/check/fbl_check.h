#ifndef FBL_CHECK_H
#define FBL_CHECK_H

// The type checker: Luau's Frontend with its new solver and lint, as luau-analyze runs them, over
// files the program gives. It asks for nothing itself: a check says which files it wanted that it
// hasn't been given, the program gives each, or says there's none, and checks again, until it
// wants none. Its modules are found as require finds them, by the same rules: paths from the
// files' root, "./" and "../" from the requiring module, "@self", and a .luaurc's aliases, and
// nothing above the root. A checker is used by one thread at a time.

#include "core/fbl_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// A module's mode, as its "--!" line says it, for a module without one.
#define FBL_CHECK_STRICT    0
#define FBL_CHECK_NONSTRICT 1
#define FBL_CHECK_NOCHECK   2

// What a diagnostic is: an error, or a lint's warning.
#define FBL_CHECK_ERROR   0
#define FBL_CHECK_WARNING 1

// A new checker, whose modules without a "--!" line are of `mode`, and whose checks each have
// `seconds`, above 0, for all their modules, across the runs it takes to give a check its files:
// one past it is a diagnostic of "TimeLimit" and none of its modules' others. It's the solver
// that's stopped at the deadline, and no module's begun after it; what runs before a module's
// solver, its constraint generation, and its lint can't be stopped, so a check can run past it
// by those of the modules under way, which Luau begins together when none needs another.
// Parsing and reading files aren't counted. Or 0, with fbl_core_error saying why.
//
// A check that reaches a module with a `type function` is refused, a diagnostic of "TypeFunction"
// at each, and none is run, as checking would run a mod's code outside any limit. Luau's heap
// limit for type functions, `heap` bytes, above 0, at most 2 GB, stays as a second fence: it's
// the process's, so the smallest any checker's asked for is every checker's, from when it's made.
FBL_API uint64_t fbl_check_new(int32_t mode, double seconds, int64_t heap);

// Lets go of a checker. A handle that isn't a checker's does nothing.
FBL_API void fbl_check_free(uint64_t checker);

// Adds a definitions file, Luau's "declare" syntax, of `length` bytes, to what every module
// checked from here sees, named `name` in its errors. A status: FBL_SYNTAX for one that doesn't
// parse or check, its first error as this thread's last error.
FBL_API int32_t fbl_check_definitions(uint64_t checker, const uint8_t* text, int64_t length,
                                      const char* name);

// Whether the checker's mode and Luau's default lint stand whatever a module's files say, when
// `enforced` isn't 0: a module's "--!strict", "--!nonstrict", "--!nocheck", and "--!nolint" lines
// are passed over, and a .luaurc gives only its aliases, not its languageMode, lint, lintErrors,
// typeErrors, or globals. Off by default, as luau-analyze has it. A status.
FBL_API int32_t fbl_check_enforce(uint64_t checker, int32_t enforced);

// Gives the file at `path`, from the files' root, `length` bytes of text, or, when `present` is
// 0, says there's no such file. A file given before is given again, as it's changed. A status.
FBL_API int32_t fbl_check_give(uint64_t checker, const char* path, const uint8_t* text,
                               int64_t length, int32_t present);

// Forgets every file it's been given, and what it checked, keeping its definitions. A status.
FBL_API int32_t fbl_check_forget(uint64_t checker);

// Checks the module at `module`, a module path from the root like "mods/doors", which is
// "mods/doors.luau", "mods/doors.lua", "mods/doors/init.luau", or "mods/doors/init.lua", and the
// modules it requires. It gives how many files it wanted and wasn't given, which fbl_check_wanted
// names, and which the program gives before it checks again; 0 when it's checked, its
// diagnostics in fbl_check_count; or -1, with fbl_core_error saying why: no such module, two of
// its files, or a path past what a require's may be.
FBL_API int32_t fbl_check_run(uint64_t checker, const char* module);

// The path of the `index`th file the last check wanted, or null.
FBL_API const char* fbl_check_wanted(uint64_t checker, int32_t index);

// How many diagnostics the last check that finished gave, or -1 for a handle that isn't a
// checker's.
FBL_API int32_t fbl_check_count(uint64_t checker);

// Diagnostic `index`'s number `which`: 0, its line, and 1, its column, from 1; 2 and 3, the line
// and the column just past where it ends; 4, its severity, an FBL_CHECK_ERROR or
// FBL_CHECK_WARNING; 5, Luau's code for it, or 0 for one of the checker's own. -1 for what isn't
// one.
FBL_API int32_t fbl_check_number(uint64_t checker, int32_t index, int32_t which);

// Diagnostic `index`'s text: `which` 0, its module's file path; 1, Luau's name for its kind, like
// "TypeError", "SyntaxError", or a lint's, like "UnknownGlobal"; 2, its message. Null for what
// isn't one. It's kept until the checker's next check.
FBL_API const char* fbl_check_text(uint64_t checker, int32_t index, int32_t which);

#ifdef __cplusplus
}
#endif

#endif
