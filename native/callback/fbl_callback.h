#ifndef FBL_CALLBACK_H
#define FBL_CALLBACK_H

// The program's functions in Luau: each is a Luau function that calls the one callback the
// program gives, with its State, a frame of the arguments it was called with, and the number the
// program gave the function. The callback reads the arguments, and leaves the frame holding the
// function's results, giving FBL_OK; or holding only the error to raise, as it is, giving
// FBL_RUNTIME. It runs on the State's thread, inside a call the program made, so it reaches what
// the program keeps of the State through that thread. A function's number is given back, by
// fbl_callback_released, once Luau has collected its last copy.

#include "core/fbl_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// The callback: the State, the frame of the call, whose slots from 0 are its arguments, and the
// function's number. A status, as above.
// NOLINTNEXTLINE(modernize-use-using): C has no `using`.
typedef int32_t (*fbl_callback_t)(uint64_t state, int32_t frame, int64_t function);

// Sets the callback, the same for every State, and every thread; one that's Null is refused.
FBL_API bool fbl_callback_set(fbl_callback_t callback);

// Adds a Luau function to the frame that calls the callback with `function`, as `name` in Luau's
// tracebacks and errors. A status.
FBL_API int32_t fbl_callback_function(uint64_t state, int32_t frame, int64_t function,
                                      const char* name);

// Takes up to `capacity` numbers of functions Luau has collected, into `functions`: how many it
// took, which are taken off what's left to take; -1 for a handle that isn't a State's.
FBL_API int32_t fbl_callback_released(uint64_t state, int64_t* functions, int32_t capacity);

// Adds to the frame where the function the frame's call is from was called, as Luau's errors start
// "chunk:line: ", or "" where that isn't known. A status.
FBL_API int32_t fbl_callback_where(uint64_t state, int32_t frame);

#ifdef __cplusplus
}
#endif

#endif
