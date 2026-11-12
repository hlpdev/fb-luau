#ifndef FBL_BYTECODE_H
#define FBL_BYTECODE_H

// Luau's bytecode: compiling source ahead, with no State, and loading bytecode into one. Luau's
// loader checks nothing of what it's given, so the program checks bytecode before it loads it:
// the package's container, signed with the program's key, is what makes it the program's own.

#include "core/fbl_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Compiles `length` bytes of source with the compile options, each from 0 to 2, into bytecode the
// library keeps until fbl_bytecode_free: its bytes, and their count in `size`, with FBL_OK in
// `status`; or null, with FBL_SYNTAX in `status` for source that doesn't compile, the error,
// which says its line, as this thread's last error; FBL_MEMORY; or FBL_MISUSE.
FBL_API uint8_t* fbl_bytecode_compile(const uint8_t* source, int64_t length, int32_t optimization,
                                      int32_t debug, int32_t coverage, int64_t* size,
                                      int32_t* status);

// Lets go of bytecode fbl_bytecode_compile gave. Null does nothing.
FBL_API void fbl_bytecode_free(uint8_t* bytecode);

// Loads `length` bytes of bytecode as the chunk `name`, and adds its function to the frame, its
// environment the table in `environment_slot`, or the frame's thread's globals for -1. Luau
// trusts it: bytecode the program hasn't checked is its process's to lose. A status: FBL_RUNTIME
// for bytecode of a version this Luau doesn't load.
FBL_API int32_t fbl_bytecode_load(uint64_t state, int32_t frame, const uint8_t* bytecode,
                                  int64_t length, const char* name, int32_t environment_slot);

#ifdef __cplusplus
}
#endif

#endif
