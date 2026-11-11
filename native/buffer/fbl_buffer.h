#ifndef FBL_BUFFER_H
#define FBL_BUFFER_H

// Luau's buffers, in a frame's slots: making one, its length, and copying bytes out of it and into
// it, each copy within its bounds or refused. Each takes the buffer's slot, which has to hold one.

#include "core/fbl_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Adds a new buffer of `size` bytes, all 0, to the frame: from 0 to 1 GiB, as Luau's are. A
// status.
FBL_API int32_t fbl_buffer_new(uint64_t state, int32_t frame, int64_t size);

// A buffer's length in bytes, or -1 for a slot that isn't one.
FBL_API int64_t fbl_buffer_length(uint64_t state, int32_t frame, int32_t slot);

// Copies `count` bytes of the buffer from `offset` to `bytes`; false, copying nothing, for bytes
// that aren't all within it.
FBL_API bool fbl_buffer_read(uint64_t state, int32_t frame, int32_t slot, int64_t offset,
                             uint8_t* bytes, int64_t count);

// Copies `count` bytes from `bytes` into the buffer at `offset`; false, copying nothing, for bytes
// that wouldn't all be within it.
FBL_API bool fbl_buffer_write(uint64_t state, int32_t frame, int32_t slot, int64_t offset,
                              const uint8_t* bytes, int64_t count);

#ifdef __cplusplus
}
#endif

#endif
