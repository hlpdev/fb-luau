// A library that exports more than its C API, which tools/check-exports.sh has to refuse.

#include "core/fbl_core.h"

extern "C" {

FBL_API int fbl_overexported_fine(void) {
    return 1;
}

FBL_API int overexported_stray(void) {
    return 2;
}
}
