#ifndef FBL_UDATA_H
#define FBL_UDATA_H

// The program's objects in Luau: each is a userdata holding a number the program gives it, of a
// type the program registers, whose metatable has the type's __index, __newindex, and
// __namecall, the program's functions, and is locked: a script can't read it, change it, or give
// it to anything else. The same number is the same userdata while Luau has it, and a number is
// given back, by fbl_udata_released, once Luau has collected its userdata, so the program can let
// go of its object.

#include "core/fbl_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers type `type`, from 1, named `name`, as typeof() gives it and errors say it, whose
// __index, __newindex, and __namecall are the functions in those slots of the frame. A status:
// one already registered is refused.
FBL_API int32_t fbl_udata_type(uint64_t state, int32_t frame, int32_t type, const char* name,
                               int32_t index_slot, int32_t newindex_slot, int32_t namecall_slot);

// Sets another field of a registered type's metatable, like "__add" or "__tostring", to a slot's
// value. A status: one of the three the type was registered with, or __metatable, is refused.
FBL_API int32_t fbl_udata_type_set(uint64_t state, int32_t frame, int32_t type, const char* field,
                                   int32_t value_slot);

// Adds the userdata of `object` to the frame, if Luau still has it: FBL_OK, or FBL_OTHER,
// adding nothing, when it hasn't, and the program makes a new one.
FBL_API int32_t fbl_udata_push(uint64_t state, int32_t frame, int64_t object);

// Adds a new userdata of `object` to the frame, of a registered type. A status.
FBL_API int32_t fbl_udata_new(uint64_t state, int32_t frame, int64_t object, int32_t type);

// The number of the object a slot's userdata holds, into `object`, and its type, into `type`;
// false for a slot that isn't one of the program's objects.
FBL_API bool fbl_udata_object(uint64_t state, int32_t frame, int32_t slot, int64_t* object,
                              int32_t* type);

// Takes up to `capacity` numbers of objects Luau has collected, into `objects`: how many it
// took; -1 for a handle that isn't a State's.
FBL_API int32_t fbl_udata_released(uint64_t state, int64_t* objects, int32_t capacity);

#ifdef __cplusplus
}
#endif

#endif
