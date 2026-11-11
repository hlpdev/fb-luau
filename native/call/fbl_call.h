#ifndef FBL_CALL_H
#define FBL_CALL_H

// Calling a function in a frame. A call runs on a Luau thread of its own, so an error keeps where
// it was raised for its traceback, and a yield, which nothing here can wait for, is an error.

#include "core/fbl_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Calls the function in `slot`, with the frame's values after it as its arguments. A status: with
// FBL_OK, the function and its arguments are replaced by its results, `results` of them, nil for
// any it didn't give, or all it gave for -1; with an error, they're gone, and the State keeps the
// error, with its traceback.
FBL_API int32_t fbl_call_function(uint64_t state, int32_t frame, int32_t slot, int32_t results);

#ifdef __cplusplus
}
#endif

#endif
