#include "core/core.hpp"
#include "fbl.h"
#include "state/state.hpp"

#include <cmath>
#include <initializer_list>
#include <lua.h>
#include <lualib.h>

namespace {

// A table's frame and stack index, as each call names it; empty, saying why, if it isn't one.
struct table_t {
    fbl::state_t* state = nullptr;
    const fbl::frame_t* frame = nullptr;
    int index = 0;

    explicit operator bool() const {
        return index != 0;
    }
};

table_t find(uint64_t state, int32_t frame, int32_t slot) noexcept {
    fbl::state_t* found = fbl::find_state(state);
    const fbl::frame_t* in = found == nullptr ? nullptr : fbl::find_frame(*found, frame);
    const int index = in == nullptr ? 0 : fbl::slot_index(*in, slot);
    if (index == 0) {
        return {};
    }
    if (lua_type(in->thread, index) != LUA_TTABLE) {
        fbl::misuse(fbl::formatted("slot {} isn't a table", slot));
        return {};
    }
    return { .state = found, .frame = in, .index = index };
}

// The stack index of another slot of the table's frame, or 0, saying why, for one outside it.
int slot_of(const table_t& table, int32_t slot) noexcept {
    return fbl::slot_index(*table.frame, slot);
}

// Runs `work` under protection with copies of the stack's values at `indexes` as its arguments,
// leaving its `results` in the frame: a status.
template <typename Work>
int32_t with(const table_t& table, std::initializer_list<int> indexes, int results,
             Work work) noexcept {
    lua_State* thread = table.frame->thread;
    if (lua_checkstack(thread, static_cast<int>(indexes.size())) == 0) {
        return fbl::record_error(*table.state, FBL_MEMORY, "not enough memory");
    }
    for (const int index : indexes) {
        lua_pushvalue(thread, index);
    }
    const int status =
        fbl::protect(*table.state, thread, static_cast<int>(indexes.size()), results, work);
    if (status == fbl::no_room) {
        lua_pop(thread, static_cast<int>(indexes.size()));
    }
    return fbl::settle(*table.state, thread, status);
}

} // namespace

int32_t fbl_table_new(uint64_t state, int32_t frame, int32_t array, int32_t hash) try {
    fbl::state_t* found = fbl::find_state(state);
    const fbl::frame_t* in = found == nullptr ? nullptr : fbl::find_frame(*found, frame);
    if (in == nullptr) {
        return FBL_MISUSE;
    }
    if (array < 0 || hash < 0) {
        return fbl::misuse(fbl::formatted("a table can't have room for {} and {}", array, hash));
    }
    auto work = [array, hash](lua_State* stack) {
        lua_createtable(stack, array, hash);
        return 1;
    };
    return fbl::run_protected(*found, in->thread, 0, 1, work);
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_table_get(uint64_t state, int32_t frame, int32_t table, int32_t key_slot) try {
    const table_t found = find(state, frame, table);
    const int key = found ? slot_of(found, key_slot) : 0;
    if (key == 0) {
        return FBL_MISUSE;
    }
    return with(found, { found.index, key }, 1, [](lua_State* stack) {
        lua_gettable(stack, 1);
        return 1;
    });
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_table_set(uint64_t state, int32_t frame, int32_t table, int32_t key_slot,
                      int32_t value_slot) try {
    const table_t found = find(state, frame, table);
    const int key = found ? slot_of(found, key_slot) : 0;
    const int value = key != 0 ? slot_of(found, value_slot) : 0;
    if (value == 0) {
        return FBL_MISUSE;
    }
    return with(found, { found.index, key, value }, 0, [](lua_State* stack) {
        lua_settable(stack, 1);
        return 0;
    });
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_table_raw_get(uint64_t state, int32_t frame, int32_t table, int32_t key_slot) try {
    const table_t found = find(state, frame, table);
    const int key = found ? slot_of(found, key_slot) : 0;
    if (key == 0) {
        return FBL_MISUSE;
    }
    return with(found, { found.index, key }, 1, [](lua_State* stack) {
        lua_rawget(stack, 1);
        return 1;
    });
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_table_raw_set(uint64_t state, int32_t frame, int32_t table, int32_t key_slot,
                          int32_t value_slot) try {
    const table_t found = find(state, frame, table);
    const int key = found ? slot_of(found, key_slot) : 0;
    const int value = key != 0 ? slot_of(found, value_slot) : 0;
    if (value == 0) {
        return FBL_MISUSE;
    }
    return with(found, { found.index, key, value }, 0, [](lua_State* stack) {
        // Luau's rawset doesn't refuse a nil or NaN key itself, which its table can't hold.
        if (lua_isnil(stack, 2)) {
            luaL_error(stack, "table index is nil");
        }
        if (lua_type(stack, 2) == LUA_TNUMBER && std::isnan(lua_tonumber(stack, 2))) {
            luaL_error(stack, "table index is NaN");
        }
        lua_rawset(stack, 1);
        return 0;
    });
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_table_raw_get_index(uint64_t state, int32_t frame, int32_t table, int32_t index) try {
    const table_t found = find(state, frame, table);
    if (!found) {
        return FBL_MISUSE;
    }
    return with(found, { found.index }, 1, [index](lua_State* stack) {
        lua_rawgeti(stack, 1, index);
        return 1;
    });
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_table_length(uint64_t state, int32_t frame, int32_t table) try {
    const table_t found = find(state, frame, table);
    return found ? lua_objlen(found.frame->thread, found.index) : -1;
} catch (...) {
    fbl::escaped();
    return -1;
}

int32_t fbl_table_next(uint64_t state, int32_t frame, int32_t table, int32_t iterator) try {
    const table_t found = find(state, frame, table);
    if (!found) {
        return -2;
    }
    if (iterator < 0) {
        fbl::fail(fbl::formatted("{} isn't an iterator", iterator));
        return -2;
    }
    // Room for the key and the value first, so it never grows the stack itself.
    if (lua_checkstack(found.frame->thread, 2) == 0) {
        fbl::record_error(*found.state, FBL_MEMORY, "not enough memory");
        fbl::fail("not enough memory");
        return -2;
    }
    return lua_rawiter(found.frame->thread, found.index, iterator);
} catch (...) {
    fbl::escaped();
    return -2;
}

int32_t fbl_table_get_metatable(uint64_t state, int32_t frame, int32_t table) try {
    const table_t found = find(state, frame, table);
    if (!found) {
        return FBL_MISUSE;
    }
    if (lua_checkstack(found.frame->thread, 1) == 0) {
        return fbl::record_error(*found.state, FBL_MEMORY, "not enough memory");
    }
    if (lua_getmetatable(found.frame->thread, found.index) == 0) {
        lua_pushnil(found.frame->thread);
    }
    return FBL_OK;
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_table_set_metatable(uint64_t state, int32_t frame, int32_t table,
                                int32_t metatable_slot) try {
    const table_t found = find(state, frame, table);
    const int metatable = found ? slot_of(found, metatable_slot) : 0;
    if (metatable == 0) {
        return FBL_MISUSE;
    }
    const int type = lua_type(found.frame->thread, metatable);
    if (type != LUA_TTABLE && type != LUA_TNIL) {
        return fbl::misuse(fbl::formatted("slot {} isn't a table or nil", metatable_slot));
    }
    return with(found, { found.index, metatable }, 0, [](lua_State* stack) {
        lua_setmetatable(stack, 1);
        return 0;
    });
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_table_freeze(uint64_t state, int32_t frame, int32_t table) try {
    const table_t found = find(state, frame, table);
    if (!found) {
        return FBL_MISUSE;
    }
    lua_setreadonly(found.frame->thread, found.index, 1);
    return FBL_OK;
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_table_is_frozen(uint64_t state, int32_t frame, int32_t table) try {
    const table_t found = find(state, frame, table);
    return found ? lua_getreadonly(found.frame->thread, found.index) : -1;
} catch (...) {
    fbl::escaped();
    return -1;
}
