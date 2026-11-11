#ifndef FBL_STATE_H
#define FBL_STATE_H

// A State: a Luau VM of its own, which only the thread that made it uses, with a memory limit,
// the options its code compiles with, and the last error a call on it met. A State's handle is a
// uint64_t, never 0, which a closed State's stays refused for.

#include "core/fbl_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// What a State is made with. C has no `using`, so it's a typedef.
// NOLINTNEXTLINE(modernize-use-using)
typedef struct fbl_state_options_t {
    // The most bytes the State's VM may hold at once, or 0 for no limit.
    int64_t memory_limit;
    // Luau's compile options for the State's code: optimization 0 to 2, debug 0 to 2, coverage 0
    // to 2.
    int32_t optimization;
    int32_t debug;
    int32_t coverage;
    int32_t unused;
} fbl_state_options_t;

// Makes a State, with Luau's flags set first, as its release's CLI sets them, and its libraries
// open. Its handle, with FBL_OK in `status`; or 0, with FBL_MEMORY when it doesn't fit its limit,
// or FBL_MISUSE for options out of range.
FBL_API uint64_t fbl_state_new(const fbl_state_options_t* options, int32_t* status);

// Closes a State: its VM and everything in it go, and its handle is refused from then on. False,
// changing nothing, while it runs a call, or for a handle that isn't a State's.
FBL_API bool fbl_state_close(uint64_t state);

// The bytes the State's VM holds, or -1 for a handle that isn't a State's.
FBL_API int64_t fbl_state_memory(uint64_t state);

// A full collection of the State's garbage. A status.
FBL_API int32_t fbl_state_collect(uint64_t state);

// The kind of the last error a call on the State met, or FBL_OK while none has.
FBL_API int32_t fbl_state_error_kind(uint64_t state);

// Its message's bytes, `length` of them, which stay until the State's next call that fails; NULL
// for a handle that isn't a State's.
FBL_API const uint8_t* fbl_state_error_message(uint64_t state, int64_t* length);

// Where it was raised, Luau's traceback, one call a line, innermost first; empty when it wasn't
// raised in Luau code.
FBL_API const uint8_t* fbl_state_error_traceback(uint64_t state, int64_t* length);

#ifdef __cplusplus
}
#endif

#endif
