#ifndef FBL_STATE_H
#define FBL_STATE_H

// A State: a Luau VM of its own, which only the thread that made it uses, with a memory limit,
// the options its code compiles with, and the last error a call on it met. A State's handle is a
// uint64_t, never 0, which a closed State's stays refused for.

#include "core/fbl_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// The libraries a State opens, as bits of fbl_state_options_t's libraries.
#define FBL_LIBRARY_BASE      1
#define FBL_LIBRARY_COROUTINE 2
#define FBL_LIBRARY_TABLE     4
#define FBL_LIBRARY_OS        8
#define FBL_LIBRARY_STRING    16
#define FBL_LIBRARY_MATH      32
#define FBL_LIBRARY_DEBUG     64
#define FBL_LIBRARY_UTF8      128
#define FBL_LIBRARY_BIT32     256
#define FBL_LIBRARY_BUFFER    512
#define FBL_LIBRARY_VECTOR    1024
#define FBL_LIBRARY_INTEGER   2048

// What `collectgarbage` a State's scripts have: none, only collectgarbage("count"), or every
// option of the one Luau's own tests have.
#define FBL_COLLECT_NONE  0
#define FBL_COLLECT_COUNT 1
#define FBL_COLLECT_FULL  2

// What a State is made with. C has no `using`, so it's a typedef.
// NOLINTNEXTLINE(modernize-use-using)
typedef struct fbl_state_options_t {
    // The most bytes the State's VM may hold at once, or 0 for no limit.
    int64_t memory_limit;
    // How long, in microseconds, each call of the program's into the State may run, or 0 for no
    // limit: past it, the script's code meets a FBL_TIMEOUT error at each of Luau's interrupts,
    // which a pcall in it can't keep, until the call returns.
    int64_t time_budget;
    // Luau's compile options for the State's code: optimization 0 to 2, debug 0 to 2, coverage 0
    // to 2.
    int32_t optimization;
    int32_t debug;
    int32_t coverage;
    // The libraries it opens, FBL_LIBRARY_ bits.
    int32_t libraries;
    // Its collectgarbage, an FBL_COLLECT_.
    int32_t collect_garbage;
    // Whether scripts have loadstring, which compiles source, never bytecode, and getfenv and
    // setfenv, which reach other functions' environments.
    bool load_string;
    bool environments;
    bool unused[2];
} fbl_state_options_t;

// Makes a State, with Luau's flags set first, as its release's CLI sets them, and its libraries
// open. Its handle, with FBL_OK in `status`; or 0, with FBL_MEMORY when it doesn't fit its limit,
// or FBL_MISUSE for options out of range.
FBL_API uint64_t fbl_state_new(const fbl_state_options_t* options, int32_t* status);

// Closes a State: its VM and everything in it go, frames the program left open too, and its handle
// is refused from then on. False, changing nothing, while it runs a call, as inside a function of
// the program's that Luau called, or for a handle that isn't a State's.
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

// Adds its value to the frame: what the script raised, which can be any value; a syntax error's
// message; or nil for an error that isn't a value of Luau's, like one the library met. A status.
FBL_API int32_t fbl_state_error_value(uint64_t state, int32_t frame);

// Where it was raised, Luau's traceback, one call a line, innermost first; empty when it wasn't
// raised in Luau code.
FBL_API const uint8_t* fbl_state_error_traceback(uint64_t state, int64_t* length);

#ifdef __cplusplus
}
#endif

#endif
