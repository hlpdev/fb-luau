#ifndef FBL_CORE_H
#define FBL_CORE_H

// What every area of the luau package's C API shares: how its functions are exported, and its
// errors. Each area's header (fbl_<area>.h) has its own binding file in luau/c/, which
// tools/bindgen.sh makes, so an area's header uses only fixed-size types and its own structs,
// never another area's. No function here lets a Luau error or a C++ exception out: each one that
// can fail says so by what it gives, and fbl_core_error says why.

#include <stdbool.h>
#include <stdint.h>

#ifndef _WIN32
#define FBL_API __attribute__((visibility("default")))
#else
#ifdef FBL_BUILDING
#define FBL_API __declspec(dllexport)
#else
#define FBL_API __declspec(dllimport)
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

// What a call that runs Luau gives: FBL_OK, or the kind of error it met, whose message and
// traceback the State keeps (fbl_state_error_message and fbl_state_error_traceback) until its next
// call that fails; or FBL_MISUSE, for a call the program shouldn't have made, as with a handle
// that isn't a live State's, or a slot outside its frame, whose message is fbl_core_error's.
#define FBL_OK 0
// Source that doesn't compile.
#define FBL_SYNTAX 1
// An error a script raised, or met as it ran.
#define FBL_RUNTIME 2
// Past the State's memory limit, or out of memory.
#define FBL_MEMORY 3
// Past the State's time budget.
#define FBL_TIMEOUT 4
// A value that isn't of the type asked for.
#define FBL_CONVERSION 5
// A yield where nothing can wait for it.
#define FBL_YIELD 6
// A State closed while its thread waited: the package's, for a thread it ran on the scheduler,
// which the library doesn't give.
#define FBL_CLOSED 7
// Anything else.
#define FBL_OTHER 8
// A call the program shouldn't have made.
#define FBL_MISUSE 9

// The version of the luau package this library is, like "0.1.0".
FBL_API const char* fbl_core_version(void);

// The release of Luau the library has in it, like "0.746".
FBL_API const char* fbl_core_luau_version(void);

// What the last call that failed on this thread says went wrong, or "" before any has.
FBL_API const char* fbl_core_error(void);

// The bytes of memory the process holds now, as the OS counts them, which a soak watches for a
// slow leak: on Windows its private bytes, and on Linux and macOS its resident set; or -1 where
// they can't be read.
FBL_API int64_t fbl_core_resident(void);

#ifdef __cplusplus
}
#endif

#endif
