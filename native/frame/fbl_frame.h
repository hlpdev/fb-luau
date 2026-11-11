#ifndef FBL_FRAME_H
#define FBL_FRAME_H

// A frame: the values the program gives Luau and Luau gives back, in slots numbered from 0, which
// the program opens on a State, fills and reads, and closes, so it never sees Luau's stack. Only
// the State's newest frame is used, and closing it takes its values away. A slot's value is read
// as the type fbl_frame_type says it is; reading it as another is refused.

#include "core/fbl_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// The type of a slot's value.
#define FBL_TYPE_NIL           0
#define FBL_TYPE_BOOLEAN       1
#define FBL_TYPE_NUMBER        2
#define FBL_TYPE_INTEGER       3
#define FBL_TYPE_VECTOR        4
#define FBL_TYPE_STRING        5
#define FBL_TYPE_TABLE         6
#define FBL_TYPE_FUNCTION      7
#define FBL_TYPE_USERDATA      8
#define FBL_TYPE_THREAD        9
#define FBL_TYPE_BUFFER        10
#define FBL_TYPE_LIGHTUSERDATA 11
// One of Luau's own that's none of these, like a user-defined class's.
#define FBL_TYPE_OTHER 12

// Opens a frame on a State, after its others. Its number, or -1.
FBL_API int32_t fbl_frame_open(uint64_t state);

// Closes the State's newest frame, and takes its values away.
FBL_API bool fbl_frame_close(uint64_t state, int32_t frame);

// How many values the frame has, or -1.
FBL_API int32_t fbl_frame_count(uint64_t state, int32_t frame);

// Takes the frame's values from `count` on away, so it has `count`.
FBL_API bool fbl_frame_truncate(uint64_t state, int32_t frame, int32_t count);

// Moves the frame's last value to `slot`, the slots from there each one higher.
FBL_API bool fbl_frame_insert(uint64_t state, int32_t frame, int32_t slot);

// Takes a slot's value out of the frame, the slots after it each one lower.
FBL_API bool fbl_frame_remove(uint64_t state, int32_t frame, int32_t slot);

// The type of a slot's value, or -1.
FBL_API int32_t fbl_frame_type(uint64_t state, int32_t frame, int32_t slot);

// Adds a value to the frame. A status: FBL_OK, FBL_MEMORY, or FBL_MISUSE.
FBL_API int32_t fbl_frame_push_nil(uint64_t state, int32_t frame);
FBL_API int32_t fbl_frame_push_boolean(uint64_t state, int32_t frame, bool value);
FBL_API int32_t fbl_frame_push_number(uint64_t state, int32_t frame, double value);
FBL_API int32_t fbl_frame_push_integer(uint64_t state, int32_t frame, int64_t value);
FBL_API int32_t fbl_frame_push_string(uint64_t state, int32_t frame, const uint8_t* bytes,
                                      int64_t length);

// Adds a copy of a slot's value to the frame.
FBL_API int32_t fbl_frame_push_copy(uint64_t state, int32_t frame, int32_t slot);

// A slot's value, of the type its function names, into `value`; false for one of another type.
FBL_API bool fbl_frame_boolean(uint64_t state, int32_t frame, int32_t slot, bool* value);
FBL_API bool fbl_frame_number(uint64_t state, int32_t frame, int32_t slot, double* value);
FBL_API bool fbl_frame_integer(uint64_t state, int32_t frame, int32_t slot, int64_t* value);

// A string slot's bytes, `length` of them, which stay while the slot does; NULL for a slot that
// isn't a string.
FBL_API const uint8_t* fbl_frame_string(uint64_t state, int32_t frame, int32_t slot,
                                        int64_t* length);

// Adds a global's value to the frame, a global of the thread the frame is on, reading it as a
// script does, through its globals' metatable. A status.
FBL_API int32_t fbl_frame_get_global(uint64_t state, int32_t frame, const uint8_t* name,
                                     int64_t length);

// Sets a global to a slot's value, as a script does. A status.
FBL_API int32_t fbl_frame_set_global(uint64_t state, int32_t frame, const uint8_t* name,
                                     int64_t length, int32_t slot);

#ifdef __cplusplus
}
#endif

#endif
