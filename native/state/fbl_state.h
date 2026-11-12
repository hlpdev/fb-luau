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

// What a State compiles to native code, with Luau's code generator, where Luau supports it: none,
// the chunks that start with --!native and functions marked @native, or everything.
#define FBL_NATIVE_OFF    0
#define FBL_NATIVE_MARKED 1
#define FBL_NATIVE_ALL    2

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
    // What it compiles to native code, an FBL_NATIVE_; where Luau's code generator isn't
    // supported, everything runs in Luau's interpreter, the same.
    int32_t native_code;
    // Whether scripts have loadstring, which compiles source, never bytecode, and getfenv and
    // setfenv, which reach other functions' environments.
    bool load_string;
    bool environments;
    bool unused[2];
    // The most bytes its native code may take, when it has any, which isn't the VM's and doesn't
    // count to its memory limit: past it, what's compiled from there runs in Luau's interpreter.
    // 0 for Luau's own cap.
    int64_t native_memory;
} fbl_state_options_t;

// Makes a State, with Luau's flags set first, as its release's CLI sets them, and its libraries
// open. Its handle, with FBL_OK in `status`; or 0, with FBL_MEMORY when it doesn't fit its limit,
// or FBL_MISUSE for options out of range.
FBL_API uint64_t fbl_state_new(const fbl_state_options_t* options, int32_t* status);

// Closes a State: its VM and everything in it go, frames the program left open too, and a turn it
// began and didn't end, and its handle is refused from then on. False, changing nothing, while it
// runs a call, as inside a function of the program's that Luau called, or for a handle that isn't
// a State's.
FBL_API bool fbl_state_close(uint64_t state);

// Runs a step of the collector, as collectgarbage("step") does, of about `kilobytes`, or Luau's
// own step for 0: 1 when it finished a cycle, 0 when it didn't, or -1 for a handle that isn't a
// State's, or kilobytes below 0.
FBL_API int32_t fbl_state_step(uint64_t state, int32_t kilobytes);

// A turn of the program's driver of threads: 1 begins one, with the time budget for everything
// it resumes, as one call has it, which only begins when no call runs; 0 ends it; a status. 2
// gives 1 when the turn's budget is spent, 0 when it isn't, or -1 for a handle that isn't a
// State's. 3 gives 1 while a call of the program's into the State runs, when a driver's turn
// would resume threads inside it, and 0 when none does.
FBL_API int32_t fbl_state_turn(uint64_t state, int32_t what);

// How many States the process has open, on every thread, or, when `here` isn't 0, those of the
// calling thread: what a program's tests and soaks watch come back to what it was, the second
// where tests run side by side on a pool of threads.
FBL_API int64_t fbl_state_count(int32_t here);

// The bytes every State of the process holds, on every thread, or, when `here` isn't 0, the
// calling thread's States hold, as fbl_state_count counts them.
FBL_API int64_t fbl_state_memory_all(int32_t here);

// Whether Luau's code generator is supported here, the processor and the system: 1 or 0.
FBL_API int32_t fbl_state_native_supported(void);

// How many of the State's functions it's compiled to native code, or -1 for a handle that isn't
// a State's.
FBL_API int64_t fbl_state_native_functions(uint64_t state);

// The bytes the State's native code takes, of its native_memory, or -1 for a handle that isn't a
// State's.
FBL_API int64_t fbl_state_native_memory(uint64_t state);

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
