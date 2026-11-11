#ifndef FBL_REF_H
#define FBL_REF_H

// A reference: a value the program keeps in a State, past any frame, as a handle of its own, like a
// table's or a function's, by a number the State gives it. It keeps its value alive in the VM until
// it's freed, and a freed one is refused.

#include "core/fbl_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// A reference to a slot's value, which mustn't be nil: its number, above 0, into `ref`. A status.
FBL_API int32_t fbl_ref_new(uint64_t state, int32_t frame, int32_t slot, int32_t* ref);

// Adds a reference's value to the frame. A status.
FBL_API int32_t fbl_ref_push(uint64_t state, int32_t frame, int32_t ref);

// Frees a reference, so its value can go. False for one the State doesn't have.
FBL_API bool fbl_ref_free(uint64_t state, int32_t ref);

#ifdef __cplusplus
}
#endif

#endif
