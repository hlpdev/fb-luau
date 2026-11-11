#include "core/core.hpp"
#include "fbl.h"
#include "state/state.hpp"

#include <cstddef>
#include <cstring>
#include <lua.h>

namespace {

// The largest buffer Luau makes, MAX_BUFFER_SIZE.
constexpr int64_t largest = int64_t{ 1024 } * 1024 * 1024;

// A buffer slot's bytes, and its length into `length`; null, saying why, for one that isn't.
uint8_t* buffer_at(uint64_t state, int32_t frame, int32_t slot, int64_t* length) noexcept {
    fbl::state_t* found = fbl::find_state(state);
    const fbl::frame_t* in = found == nullptr ? nullptr : fbl::find_frame(*found, frame);
    const int index = in == nullptr ? 0 : fbl::slot_index(*in, slot);
    if (index == 0) {
        return nullptr;
    }
    size_t size = 0;
    void* bytes = lua_tobuffer(in->thread, index, &size);
    if (bytes == nullptr) {
        fbl::fail(fbl::formatted("slot {} isn't a buffer", slot));
        return nullptr;
    }
    *length = static_cast<int64_t>(size);
    return static_cast<uint8_t*>(bytes);
}

// Whether `count` bytes from `offset` are within a buffer of `length`, saying why not.
bool within(int64_t offset, int64_t count, int64_t length) noexcept {
    if (offset < 0 || count < 0 || offset > length || count > length - offset) {
        return fbl::fail(
            fbl::formatted("{} bytes at {} aren't within a buffer of {}", count, offset, length));
    }
    return true;
}

} // namespace

int32_t fbl_buffer_new(uint64_t state, int32_t frame, int64_t size) try {
    fbl::state_t* found = fbl::find_state(state);
    const fbl::frame_t* in = found == nullptr ? nullptr : fbl::find_frame(*found, frame);
    if (in == nullptr) {
        return FBL_MISUSE;
    }
    if (size < 0 || size > largest) {
        return fbl::misuse(fbl::formatted("a buffer of {} bytes isn't from 0 to 1 GiB", size));
    }
    auto work = [size](lua_State* stack) {
        lua_newbuffer(stack, static_cast<size_t>(size));
        return 1;
    };
    return fbl::settle(*found, in->thread, fbl::protect(*found, in->thread, 0, 1, work));
} catch (...) {
    return fbl::escaped();
}

int64_t fbl_buffer_length(uint64_t state, int32_t frame, int32_t slot) try {
    int64_t length = 0;
    return buffer_at(state, frame, slot, &length) == nullptr ? -1 : length;
} catch (...) {
    fbl::escaped();
    return -1;
}

bool fbl_buffer_read(uint64_t state, int32_t frame, int32_t slot, int64_t offset, uint8_t* bytes,
                     int64_t count) try {
    int64_t length = 0;
    const uint8_t* buffer = buffer_at(state, frame, slot, &length);
    if (buffer == nullptr || !within(offset, count, length)) {
        return false;
    }
    if (bytes == nullptr && count > 0) {
        return fbl::fail("bytes need somewhere to go");
    }
    if (count > 0) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): within its bounds.
        std::memcpy(bytes, buffer + offset, static_cast<size_t>(count));
    }
    return true;
} catch (...) {
    fbl::escaped();
    return false;
}

bool fbl_buffer_write(uint64_t state, int32_t frame, int32_t slot, int64_t offset,
                      const uint8_t* bytes, int64_t count) try {
    int64_t length = 0;
    uint8_t* buffer = buffer_at(state, frame, slot, &length);
    if (buffer == nullptr || !within(offset, count, length)) {
        return false;
    }
    if (bytes == nullptr && count > 0) {
        return fbl::fail("bytes need somewhere to come from");
    }
    if (count > 0) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): within its bounds.
        std::memcpy(buffer + offset, bytes, static_cast<size_t>(count));
    }
    return true;
} catch (...) {
    fbl::escaped();
    return false;
}
