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

// The version of the luau package this library is, like "0.1.0".
FBL_API const char* fbl_core_version(void);

// The release of Luau the library has in it, like "0.746".
FBL_API const char* fbl_core_luau_version(void);

// What the last call that failed on this thread says went wrong, or "" before any has.
FBL_API const char* fbl_core_error(void);

#ifdef __cplusplus
}
#endif

#endif
