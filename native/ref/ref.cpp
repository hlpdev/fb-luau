#include "core/core.hpp"
#include "fbl.h"
#include "state/state.hpp"

#include <exception>
#include <lua.h>

int32_t fbl_ref_new(uint64_t state, int32_t frame, int32_t slot, int32_t* ref) try {
    fbl::state_t* found = fbl::find_state(state);
    const fbl::frame_t* in = found == nullptr ? nullptr : fbl::find_frame(*found, frame);
    const int index = in == nullptr ? 0 : fbl::slot_index(*in, slot);
    if (index == 0) {
        return FBL_MISUSE;
    }
    if (ref == nullptr) {
        return fbl::misuse("a reference needs somewhere to go");
    }
    lua_State* thread = in->thread;
    if (lua_isnil(thread, index)) {
        return fbl::misuse("nil has no reference");
    }
    // The registry's table can grow, which needs memory, so it's made under protection, with a copy
    // of the value as the work's argument.
    if (lua_checkstack(thread, 1) == 0) {
        return fbl::record_error(*found, FBL_MEMORY, "not enough memory");
    }
    lua_pushvalue(thread, index);
    int made = 0;
    auto work = [&made](lua_State* stack) {
        made = lua_ref(stack, 1);
        return 0;
    };
    const int status = fbl::protect(*found, thread, 1, 0, work);
    if (status == fbl::no_room) {
        lua_pop(thread, 1);
    }
    const int32_t settled = fbl::settle(*found, thread, status);
    if (settled != FBL_OK) {
        return settled;
    }
    try {
        found->refs.insert(made);
    } catch (const std::exception&) {
        // One the program can't be given is freed again.
        lua_unref(found->main, made);
        return fbl::record_error(*found, FBL_MEMORY, "not enough memory");
    }
    *ref = made;
    return FBL_OK;
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_ref_push(uint64_t state, int32_t frame, int32_t ref) try {
    fbl::state_t* found = fbl::find_state(state);
    const fbl::frame_t* in = found == nullptr ? nullptr : fbl::find_frame(*found, frame);
    if (in == nullptr) {
        return FBL_MISUSE;
    }
    if (!found->refs.contains(ref)) {
        return fbl::misuse(fbl::formatted("the State has no reference {}", ref));
    }
    if (lua_checkstack(in->thread, 1) == 0) {
        return fbl::record_error(*found, FBL_MEMORY, "not enough memory");
    }
    lua_getref(in->thread, ref);
    return FBL_OK;
} catch (...) {
    return fbl::escaped();
}

bool fbl_ref_free(uint64_t state, int32_t ref) try {
    fbl::state_t* found = fbl::find_state(state);
    if (found == nullptr) {
        return false;
    }
    if (found->refs.erase(ref) == 0) {
        return fbl::fail(fbl::formatted("the State has no reference {}", ref));
    }
    // It frees the registry's slot, which needs no memory, so it can't raise.
    lua_unref(found->main, ref);
    return true;
} catch (...) {
    fbl::escaped();
    return false;
}
