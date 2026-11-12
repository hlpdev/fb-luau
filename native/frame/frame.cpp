#include "core/core.hpp"
#include "fbl.h"
#include "state/state.hpp"

#include <cstddef>
#include <exception>
#include <lua.h>
#include <string_view>

namespace {

// A frame and the State it's on, as each call names them; null, saying why, if either isn't one.
struct found_t {
    fbl::state_t* state = nullptr;
    fbl::frame_t* frame = nullptr;

    explicit operator bool() const {
        return frame != nullptr;
    }
};

found_t find(uint64_t state, int32_t frame) noexcept {
    fbl::state_t* found = fbl::find_state(state);
    if (found == nullptr) {
        return {};
    }
    return { .state = found, .frame = fbl::find_frame(*found, frame) };
}

bool valid_length(const uint8_t* bytes, int64_t length) noexcept {
    if (length < 0 || (bytes == nullptr && length > 0)) {
        fbl::fail(
            fbl::formatted("{} bytes at {} aren't text", length, static_cast<const void*>(bytes)));
        return false;
    }
    return true;
}

// A string of `length` bytes, which C gives as a pointer that's NULL for none.
std::string_view text(const uint8_t* bytes, int64_t length) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): C's bytes, as chars.
    return { length == 0 ? "" : reinterpret_cast<const char*>(bytes), static_cast<size_t>(length) };
}

// Room for one more value on a frame's thread, which lua_checkstack makes without raising, or the
// State's error saying there's none.
int32_t room(found_t found) noexcept {
    if (lua_checkstack(found.frame->thread, 1) == 0) {
        return fbl::record_error(*found.state, FBL_MEMORY, "not enough memory");
    }
    return FBL_OK;
}

} // namespace

int32_t fbl_frame_open(uint64_t state) try {
    fbl::state_t* found = fbl::find_state(state);
    if (found == nullptr) {
        return -1;
    }
    try {
        found->frames.push_back({ .thread = found->current, .base = lua_gettop(found->current) });
    } catch (const std::exception&) {
        fbl::fail("not enough memory for a frame");
        return -1;
    }
    return static_cast<int32_t>(found->frames.size() - 1);
} catch (...) {
    fbl::escaped();
    return -1;
}

bool fbl_frame_close(uint64_t state, int32_t frame) try {
    const found_t found = find(state, frame);
    if (!found) {
        return false;
    }
    lua_settop(found.frame->thread, found.frame->base);
    found.state->frames.pop_back();
    return true;
} catch (...) {
    fbl::escaped();
    return false;
}

int32_t fbl_frame_count(uint64_t state, int32_t frame) try {
    const found_t found = find(state, frame);
    return found ? lua_gettop(found.frame->thread) - found.frame->base : -1;
} catch (...) {
    fbl::escaped();
    return -1;
}

bool fbl_frame_truncate(uint64_t state, int32_t frame, int32_t count) try {
    const found_t found = find(state, frame);
    if (!found) {
        return false;
    }
    const int has = lua_gettop(found.frame->thread) - found.frame->base;
    if (count < 0 || count > has) {
        return fbl::fail(fbl::formatted("a frame of {} values can't keep {}", has, count));
    }
    lua_settop(found.frame->thread, found.frame->base + count);
    return true;
} catch (...) {
    fbl::escaped();
    return false;
}

bool fbl_frame_insert(uint64_t state, int32_t frame, int32_t slot) try {
    const found_t found = find(state, frame);
    const int index = found ? fbl::slot_index(*found.frame, slot) : 0;
    if (index == 0) {
        return false;
    }
    lua_insert(found.frame->thread, index);
    return true;
} catch (...) {
    fbl::escaped();
    return false;
}

bool fbl_frame_remove(uint64_t state, int32_t frame, int32_t slot) try {
    const found_t found = find(state, frame);
    const int index = found ? fbl::slot_index(*found.frame, slot) : 0;
    if (index == 0) {
        return false;
    }
    lua_remove(found.frame->thread, index);
    return true;
} catch (...) {
    fbl::escaped();
    return false;
}

int32_t fbl_frame_type(uint64_t state, int32_t frame, int32_t slot) try {
    const found_t found = find(state, frame);
    const int index = found ? fbl::slot_index(*found.frame, slot) : 0;
    if (index == 0) {
        return -1;
    }
    switch (lua_type(found.frame->thread, index)) {
    case LUA_TNIL:
        return FBL_TYPE_NIL;
    case LUA_TBOOLEAN:
        return FBL_TYPE_BOOLEAN;
    case LUA_TNUMBER:
        return FBL_TYPE_NUMBER;
    case LUA_TINTEGER:
        return FBL_TYPE_INTEGER;
    case LUA_TVECTOR:
        return FBL_TYPE_VECTOR;
    case LUA_TSTRING:
        return FBL_TYPE_STRING;
    case LUA_TTABLE:
        return FBL_TYPE_TABLE;
    case LUA_TFUNCTION:
        return FBL_TYPE_FUNCTION;
    case LUA_TUSERDATA:
        return FBL_TYPE_USERDATA;
    case LUA_TTHREAD:
        return FBL_TYPE_THREAD;
    case LUA_TBUFFER:
        return FBL_TYPE_BUFFER;
    case LUA_TLIGHTUSERDATA:
        return FBL_TYPE_LIGHTUSERDATA;
    default:
        return FBL_TYPE_OTHER;
    }
} catch (...) {
    fbl::escaped();
    return -1;
}

int32_t fbl_frame_push_nil(uint64_t state, int32_t frame) try {
    const found_t found = find(state, frame);
    if (!found) {
        return FBL_MISUSE;
    }
    const int32_t status = room(found);
    if (status == FBL_OK) {
        lua_pushnil(found.frame->thread);
    }
    return status;
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_frame_push_boolean(uint64_t state, int32_t frame, bool value) try {
    const found_t found = find(state, frame);
    if (!found) {
        return FBL_MISUSE;
    }
    const int32_t status = room(found);
    if (status == FBL_OK) {
        lua_pushboolean(found.frame->thread, value ? 1 : 0);
    }
    return status;
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_frame_push_number(uint64_t state, int32_t frame, double value) try {
    const found_t found = find(state, frame);
    if (!found) {
        return FBL_MISUSE;
    }
    const int32_t status = room(found);
    if (status == FBL_OK) {
        lua_pushnumber(found.frame->thread, value);
    }
    return status;
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_frame_push_integer(uint64_t state, int32_t frame, int64_t value) try {
    const found_t found = find(state, frame);
    if (!found) {
        return FBL_MISUSE;
    }
    const int32_t status = room(found);
    if (status == FBL_OK) {
        lua_pushinteger64(found.frame->thread, value);
    }
    return status;
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_frame_push_vector(uint64_t state, int32_t frame, const fbl_vector_t* value) try {
    const found_t found = find(state, frame);
    if (!found) {
        return FBL_MISUSE;
    }
    if (value == nullptr) {
        return fbl::misuse("a vector needs its value");
    }
    const int32_t status = room(found);
    if (status == FBL_OK) {
        // A vector is a value, as a number is, which needs no memory of the collector's.
        lua_pushvector(found.frame->thread, value->x, value->y, value->z);
    }
    return status;
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_frame_push_string(uint64_t state, int32_t frame, const uint8_t* bytes,
                              int64_t length) try {
    const found_t found = find(state, frame);
    if (!found || !valid_length(bytes, length)) {
        return FBL_MISUSE;
    }
    // Making a string needs memory, which can run out, so it's made under protection.
    const std::string_view value = text(bytes, length);
    auto work = [value](lua_State* stack) {
        lua_pushlstring(stack, value.data(), value.size());
        return 1;
    };
    return fbl::run_protected(*found.state, found.frame->thread, 0, 1, work);
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_frame_push_copy(uint64_t state, int32_t frame, int32_t slot) try {
    const found_t found = find(state, frame);
    const int index = found ? fbl::slot_index(*found.frame, slot) : 0;
    if (index == 0) {
        return FBL_MISUSE;
    }
    const int32_t status = room(found);
    if (status == FBL_OK) {
        lua_pushvalue(found.frame->thread, index);
    }
    return status;
} catch (...) {
    return fbl::escaped();
}

bool fbl_frame_boolean(uint64_t state, int32_t frame, int32_t slot, bool* value) try {
    const found_t found = find(state, frame);
    const int index = found ? fbl::slot_index(*found.frame, slot) : 0;
    if (index == 0 || value == nullptr) {
        return false;
    }
    if (lua_type(found.frame->thread, index) != LUA_TBOOLEAN) {
        return fbl::fail(fbl::formatted("slot {} isn't a boolean", slot));
    }
    *value = lua_toboolean(found.frame->thread, index) != 0;
    return true;
} catch (...) {
    fbl::escaped();
    return false;
}

bool fbl_frame_number(uint64_t state, int32_t frame, int32_t slot, double* value) try {
    const found_t found = find(state, frame);
    const int index = found ? fbl::slot_index(*found.frame, slot) : 0;
    if (index == 0 || value == nullptr) {
        return false;
    }
    if (lua_type(found.frame->thread, index) != LUA_TNUMBER) {
        return fbl::fail(fbl::formatted("slot {} isn't a number", slot));
    }
    *value = lua_tonumber(found.frame->thread, index);
    return true;
} catch (...) {
    fbl::escaped();
    return false;
}

bool fbl_frame_integer(uint64_t state, int32_t frame, int32_t slot, int64_t* value) try {
    const found_t found = find(state, frame);
    const int index = found ? fbl::slot_index(*found.frame, slot) : 0;
    if (index == 0 || value == nullptr) {
        return false;
    }
    if (lua_type(found.frame->thread, index) != LUA_TINTEGER) {
        return fbl::fail(fbl::formatted("slot {} isn't an integer", slot));
    }
    *value = lua_tointeger64(found.frame->thread, index, nullptr);
    return true;
} catch (...) {
    fbl::escaped();
    return false;
}

bool fbl_frame_vector(uint64_t state, int32_t frame, int32_t slot, fbl_vector_t* value) try {
    const found_t found = find(state, frame);
    const int index = found ? fbl::slot_index(*found.frame, slot) : 0;
    if (index == 0 || value == nullptr) {
        return false;
    }
    const float* vector = lua_tovector(found.frame->thread, index);
    if (vector == nullptr) {
        return fbl::fail(fbl::formatted("slot {} isn't a vector", slot));
    }
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic): Luau's 3 floats.
    *value = { .x = vector[0], .y = vector[1], .z = vector[2] };
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return true;
} catch (...) {
    fbl::escaped();
    return false;
}

const uint8_t* fbl_frame_string(uint64_t state, int32_t frame, int32_t slot, int64_t* length) try {
    const found_t found = find(state, frame);
    const int index = found ? fbl::slot_index(*found.frame, slot) : 0;
    if (index == 0 || length == nullptr) {
        return nullptr;
    }
    // A number would be made a string in its slot, which needs memory; only a string is read.
    if (lua_type(found.frame->thread, index) != LUA_TSTRING) {
        fbl::fail(fbl::formatted("slot {} isn't a string", slot));
        return nullptr;
    }
    size_t size = 0;
    const char* bytes = lua_tolstring(found.frame->thread, index, &size);
    *length = static_cast<int64_t>(size);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): its bytes, as C takes them.
    return reinterpret_cast<const uint8_t*>(bytes);
} catch (...) {
    fbl::escaped();
    return nullptr;
}

int32_t fbl_frame_get_global(uint64_t state, int32_t frame, const uint8_t* name,
                             int64_t length) try {
    const found_t found = find(state, frame);
    if (!found || !valid_length(name, length)) {
        return FBL_MISUSE;
    }
    // Its name is a string, made with memory, and reading it can run a metamethod.
    const std::string_view key = text(name, length);
    auto work = [key](lua_State* stack) {
        lua_pushlstring(stack, key.data(), key.size());
        lua_gettable(stack, LUA_GLOBALSINDEX);
        return 1;
    };
    return fbl::run_protected(*found.state, found.frame->thread, 0, 1, work);
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_frame_set_global(uint64_t state, int32_t frame, const uint8_t* name, int64_t length,
                             int32_t slot) try {
    const found_t found = find(state, frame);
    const int index = found ? fbl::slot_index(*found.frame, slot) : 0;
    if (index == 0 || !valid_length(name, length)) {
        return FBL_MISUSE;
    }
    lua_State* thread = found.frame->thread;
    // A copy of the value is the work's argument, at its stack index 1.
    if (room(found) != FBL_OK) {
        return FBL_MEMORY;
    }
    lua_pushvalue(thread, index);
    const std::string_view key = text(name, length);
    auto work = [key](lua_State* stack) {
        lua_pushlstring(stack, key.data(), key.size());
        lua_insert(stack, 1);
        lua_settable(stack, LUA_GLOBALSINDEX);
        return 0;
    };
    const int status = fbl::protect(*found.state, thread, 1, 0, work);
    if (status == fbl::no_room) {
        lua_pop(thread, 1);
    }
    return fbl::settle(*found.state, thread, status);
} catch (...) {
    return fbl::escaped();
}
