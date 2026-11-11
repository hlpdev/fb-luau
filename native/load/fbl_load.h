#ifndef FBL_LOAD_H
#define FBL_LOAD_H

// Compiling source and loading it into a State, as a function to call.

#include "core/fbl_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Compiles `length` bytes of source with the State's compile options, as the chunk `name` (as
// Luau names chunks: "=name" as it is, "@path" for a file), and adds its function to the frame,
// its environment the globals of the frame's thread. A status: FBL_SYNTAX for source that doesn't
// compile, whose message says its line.
FBL_API int32_t fbl_load_source(uint64_t state, int32_t frame, const uint8_t* source,
                                int64_t length, const char* name);

#ifdef __cplusplus
}
#endif

#endif
