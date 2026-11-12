#ifndef FBL_SANDBOX_H
#define FBL_SANDBOX_H

// A State's sandbox: freezing its globals and libraries, as Luau's luaL_sandbox does, and the
// environments scripts run in, each a table of its own whose reads go through to the frozen
// globals, as luaL_sandboxthread makes one, so one script can't change what another sees.

#include "core/fbl_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Freezes the State's globals, and every table reachable from them through tables' fields, every
// library and a program's `Config.Graphics` alike, but not through metatables or functions, and
// the string metatable, and marks the globals safe for Luau's fast paths: from then on nothing
// changes them. A table scripts share and write is given by a function or an object instead.
// Freezing it again does nothing. A status.
FBL_API int32_t fbl_sandbox_freeze(uint64_t state);

// Whether the State is frozen: 1 or 0, or -1 for a handle that isn't a State's.
FBL_API int32_t fbl_sandbox_is_frozen(uint64_t state);

// Adds a new environment for a script to the frame: a table of its own, whose _G is itself, and
// whose metatable, frozen and locked, reads what it hasn't from the State's globals, freezing them
// first. A status.
FBL_API int32_t fbl_sandbox_environment(uint64_t state, int32_t frame);

#ifdef __cplusplus
}
#endif

#endif
