#ifndef FBL_REQUIRE_H
#define FBL_REQUIRE_H

// require, Luau's own, Luau.Require, with its rules for paths: "./" and "../" from the requiring
// module, "@self", and aliases from a .luaurc. The shim walks the paths itself, over the files the
// program's reader gives, a module's path its files' from the modules' root, so a script can
// require only what the program gives it. Each module runs once, on a thread of its own with
// globals of its own, as a script's are, and its one value is kept for each require of it.

#include "core/fbl_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Sets the global require, for which the Luau function in `reader_slot` reads the modules' files:
// called with a file's path from the modules' root, with no "./" and "/" between its parts, like
// "doors.luau", "lib/init.luau", or "lib/.luaurc", it gives the file's text, or nil when there's
// no such file, and its errors are the require's. A module "lib/doors" is "lib/doors.luau",
// "lib/doors.lua", "lib/doors/init.luau", or "lib/doors/init.lua", and two of them are an error.
// A chunk named "@path" requires from that path; one of any other name, from the root. With no
// reader, `reader_slot` -1, require is an error saying the program gives no modules. The State's
// first module freezes it, as a script's environment does. A status.
FBL_API int32_t fbl_require_open(uint64_t state, int32_t frame, int32_t reader_slot);

// Forgets every module's value, so the next require of each runs it again. A status.
FBL_API int32_t fbl_require_forget(uint64_t state);

#ifdef __cplusplus
}
#endif

#endif
