#include "core/core.hpp"
#include "fbl.h"
#include "state/state.hpp"

#include <lua.h>
#include <lualib.h>

namespace {

// Freezes `state`'s main globals, under lua_cpcall, which luaL_sandbox needs memory for, and then
// every table reachable from them through tables' fields and metatables, each once, as
// luaL_sandbox freezes only those in the globals themselves: a program's `Config.Graphics` too.
// Functions' upvalues aren't followed: what a function reaches by them stays its own to change.
int freeze(lua_State* stack) {
    luaL_sandbox(stack);
    fbl::freeze_tables(stack, LUA_GLOBALSINDEX);
    return 0;
}

} // namespace

namespace fbl {

int32_t frozen(state_t& state) noexcept {
    if (state.frozen) {
        return FBL_OK;
    }
    if (lua_cpcall(state.main, freeze, nullptr) != LUA_OK) {
        return take_error(state, state.main, FBL_MEMORY);
    }
    state.frozen = true;
    return FBL_OK;
}

void freeze_tables(lua_State* stack, int index) {
    index = lua_absindex(stack, index);
    // The tables seen, as keys, and those still to go through, by number from 1.
    lua_newtable(stack);
    const int seen = lua_gettop(stack);
    lua_newtable(stack);
    const int queue = lua_gettop(stack);
    lua_setreadonly(stack, index, 1);
    lua_pushvalue(stack, index);
    lua_pushboolean(stack, 1);
    lua_rawset(stack, seen);
    lua_pushvalue(stack, index);
    lua_rawseti(stack, queue, 1);
    int queued = 1;
    // Freezes the table on top, unless it's been seen, and queues it to go through.
    const auto follow = [&]() {
        if (lua_type(stack, -1) != LUA_TTABLE) {
            return;
        }
        lua_pushvalue(stack, -1);
        lua_rawget(stack, seen);
        const bool known = lua_type(stack, -1) != LUA_TNIL;
        lua_pop(stack, 1);
        if (known) {
            return;
        }
        lua_setreadonly(stack, -1, 1);
        lua_pushvalue(stack, -1);
        lua_pushboolean(stack, 1);
        lua_rawset(stack, seen);
        lua_pushvalue(stack, -1);
        lua_rawseti(stack, queue, ++queued);
    };
    for (int next = 1; next <= queued; next++) {
        lua_rawgeti(stack, queue, next);
        const int table = lua_gettop(stack);
        lua_pushnil(stack);
        while (lua_next(stack, table) != 0) {
            follow();
            lua_pop(stack, 1);
        }
        // Its metatable, as `getmetatable(m).__index = evil` would change it for everyone.
        if (lua_getmetatable(stack, table) != 0) {
            follow();
            lua_pop(stack, 1);
        }
        lua_pop(stack, 1);
    }
    lua_pop(stack, 2);
}

void push_environment(lua_State* stack, lua_State* main) {
    // As luaL_sandboxthread makes a thread's: its own table, reading through to the globals.
    lua_newtable(stack);
    lua_newtable(stack);
    lua_pushvalue(main, LUA_GLOBALSINDEX);
    lua_xmove(main, stack, 1);
    lua_setfield(stack, -2, "__index");
    // Locked, so a script neither cuts itself off from the globals nor gets their table.
    lua_pushstring(stack, "The metatable is locked");
    lua_setfield(stack, -2, "__metatable");
    lua_setreadonly(stack, -1, 1);
    lua_setmetatable(stack, -2);
    // Its _G is itself, as Luau's own tests make a thread's.
    lua_pushvalue(stack, -1);
    lua_setfield(stack, -2, "_G");
    lua_setsafeenv(stack, -1, 1);
}

} // namespace fbl

int32_t fbl_sandbox_freeze(uint64_t state) try {
    fbl::state_t* found = fbl::find_state(state);
    if (found == nullptr) {
        return FBL_MISUSE;
    }
    return fbl::frozen(*found);
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_sandbox_is_frozen(uint64_t state) try {
    const fbl::state_t* found = fbl::find_state(state);
    if (found == nullptr) {
        return -1;
    }
    return found->frozen ? 1 : 0;
} catch (...) {
    fbl::escaped();
    return -1;
}

int32_t fbl_sandbox_environment(uint64_t state, int32_t frame) try {
    fbl::state_t* found = fbl::find_state(state);
    const fbl::frame_t* in = found == nullptr ? nullptr : fbl::find_frame(*found, frame);
    if (in == nullptr) {
        return FBL_MISUSE;
    }
    const int32_t status = fbl::frozen(*found);
    if (status != FBL_OK) {
        return status;
    }
    lua_State* main = found->main;
    auto work = [main](lua_State* stack) {
        fbl::push_environment(stack, main);
        return 1;
    };
    // The main thread's globals go to the work over its stack, which needs room for them.
    if (lua_checkstack(main, 1) == 0) {
        return fbl::record_error(*found, FBL_MEMORY, "not enough memory");
    }
    return fbl::run_protected(*found, in->thread, 0, 1, work);
} catch (...) {
    return fbl::escaped();
}
