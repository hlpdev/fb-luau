#include "core/core.hpp"
#include "fbl.h"
#include "state/state.hpp"

#include <cstdint>
#include <lua.h>
#include <lualib.h>

namespace {

// The frame's thread in `slot`, or null, saying why, for a slot that isn't one; its stack index
// into `index`.
lua_State* thread_at(const fbl::frame_t& frame, int32_t slot, int& index) noexcept {
    index = fbl::slot_index(frame, slot);
    if (index == 0) {
        return nullptr;
    }
    lua_State* thread = lua_tothread(frame.thread, index);
    if (thread == nullptr) {
        fbl::misuse(fbl::formatted("slot {} isn't a thread", slot));
    }
    return thread;
}

// Why a thread can't be resumed: its status, as a resume would otherwise meet it.
const char* unresumable(int32_t status) noexcept {
    switch (status) {
    case FBL_THREAD_SUSPENDED:
        return "that a call of the program's runs on";
    case FBL_THREAD_RUNNING:
        return "that's running";
    case FBL_THREAD_NORMAL:
        return "that's resumed another";
    default:
        return "that's dead";
    }
}

// What `co` is now, to `thread`, as an FBL_THREAD_ constant.
int32_t status_of(lua_State* thread, lua_State* co) noexcept {
    switch (lua_costatus(thread, co)) {
    case LUA_CORUN:
        return FBL_THREAD_RUNNING;
    case LUA_COSUS:
        return FBL_THREAD_SUSPENDED;
    case LUA_CONOR:
        return FBL_THREAD_NORMAL;
    case LUA_COFIN:
        return FBL_THREAD_FINISHED;
    default:
        return FBL_THREAD_FAILED;
    }
}

} // namespace

int32_t fbl_thread_new(uint64_t state, int32_t frame, int32_t slot) try {
    fbl::state_t* found = fbl::find_state(state);
    const fbl::frame_t* in = found == nullptr ? nullptr : fbl::find_frame(*found, frame);
    const int index = in == nullptr ? 0 : fbl::slot_index(*in, slot);
    if (index == 0) {
        return FBL_MISUSE;
    }
    if (lua_type(in->thread, index) != LUA_TFUNCTION) {
        return fbl::misuse(fbl::formatted("slot {} isn't a function", slot));
    }
    if (lua_checkstack(in->thread, 1) == 0) {
        return fbl::record_error(*found, FBL_MEMORY, "not enough memory");
    }
    // The function goes to the work as its argument, onto the new thread's stack, which has room
    // for Luau's least, LUA_MINSTACK.
    lua_pushvalue(in->thread, index);
    auto work = [](lua_State* stack) {
        lua_State* made = lua_newthread(stack);
        lua_pushvalue(stack, 1);
        lua_xmove(stack, made, 1);
        return 1;
    };
    return fbl::run_protected(*found, in->thread, 1, 1, work);
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_thread_current(uint64_t state, int32_t frame) try {
    fbl::state_t* found = fbl::find_state(state);
    const fbl::frame_t* in = found == nullptr ? nullptr : fbl::find_frame(*found, frame);
    if (in == nullptr) {
        return FBL_MISUSE;
    }
    if (lua_checkstack(in->thread, 1) == 0) {
        return fbl::record_error(*found, FBL_MEMORY, "not enough memory");
    }
    lua_pushthread(in->thread);
    return FBL_OK;
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_thread_status(uint64_t state, int32_t frame, int32_t slot) try {
    fbl::state_t* found = fbl::find_state(state);
    const fbl::frame_t* in = found == nullptr ? nullptr : fbl::find_frame(*found, frame);
    int index = 0;
    lua_State* co = in == nullptr ? nullptr : thread_at(*in, slot, index);
    if (co == nullptr) {
        return -1;
    }
    return status_of(in->thread, co);
} catch (...) {
    fbl::escaped();
    return -1;
}

int64_t fbl_thread_waits_in(uint64_t state, int32_t frame, int32_t slot) try {
    fbl::state_t* found = fbl::find_state(state);
    const fbl::frame_t* in = found == nullptr ? nullptr : fbl::find_frame(*found, frame);
    int index = 0;
    lua_State* co = in == nullptr ? nullptr : thread_at(*in, slot, index);
    if (co == nullptr) {
        return 0;
    }
    lua_Debug info{};
    if (lua_checkstack(co, 1) == 0 || lua_getinfo(co, 0, "f", &info) == 0) {
        return 0;
    }
    const void* function = lua_topointer(co, -1);
    lua_pop(co, 1);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the function's address, as a number.
    return static_cast<int64_t>(reinterpret_cast<uintptr_t>(function));
} catch (...) {
    fbl::escaped();
    return 0;
}

uint64_t fbl_thread_id(uint64_t state, int32_t frame, int32_t slot) try {
    fbl::state_t* found = fbl::find_state(state);
    const fbl::frame_t* in = found == nullptr ? nullptr : fbl::find_frame(*found, frame);
    int index = 0;
    const lua_State* co = in == nullptr ? nullptr : thread_at(*in, slot, index);
    if (co == nullptr) {
        return 0;
    }
    // A number of its own, kept by a weak table of the thread's, so a thread made where a
    // collected one was is never taken for it.
    lua_State* thread = in->thread;
    if (lua_checkstack(thread, 2) == 0) {
        fbl::record_error(*found, FBL_MEMORY, "not enough memory");
        return 0;
    }
    lua_getref(thread, found->thread_ids_ref);
    lua_pushvalue(thread, index);
    lua_rawget(thread, -2);
    const auto known = static_cast<uint64_t>(lua_tonumber(thread, -1));
    lua_pop(thread, 2);
    if (known != 0) {
        return known;
    }
    const uint64_t number = found->next_thread_id + 1;
    const int ids = found->thread_ids_ref;
    lua_pushvalue(thread, index);
    auto work = [ids, number](lua_State* stack) {
        lua_getref(stack, ids);
        lua_pushvalue(stack, 1);
        lua_pushnumber(stack, static_cast<double>(number));
        lua_rawset(stack, -3);
        return 0;
    };
    if (fbl::run_protected(*found, thread, 1, 0, work) != FBL_OK) {
        return 0;
    }
    found->next_thread_id = number;
    return number;
} catch (...) {
    fbl::escaped();
    return 0;
}

int32_t fbl_thread_can_yield(uint64_t state, int32_t frame) try {
    fbl::state_t* found = fbl::find_state(state);
    const fbl::frame_t* in = found == nullptr ? nullptr : fbl::find_frame(*found, frame);
    if (in == nullptr) {
        return -1;
    }
    lua_State* thread = in->thread;
    const bool can =
        thread != found->main && !fbl::is_runner(thread) && lua_isyieldable(thread) != 0;
    return can ? 1 : 0;
} catch (...) {
    fbl::escaped();
    return -1;
}

int32_t fbl_thread_resume(uint64_t state, int32_t frame, int32_t slot, int32_t raise,
                          int32_t* finished) try {
    fbl::state_t* found = fbl::find_state(state);
    const fbl::frame_t* in = found == nullptr ? nullptr : fbl::find_frame(*found, frame);
    int index = 0;
    lua_State* co = in == nullptr ? nullptr : thread_at(*in, slot, index);
    if (co == nullptr) {
        return FBL_MISUSE;
    }
    if (finished == nullptr) {
        return fbl::misuse("a resume needs somewhere to say whether the thread finished");
    }
    lua_State* thread = in->thread;
    const int count = lua_gettop(thread) - index;
    if (raise != 0 && count != 1) {
        return fbl::misuse(
            fbl::formatted("an error raised in a thread is one value, not {}", count));
    }
    const int32_t now = status_of(thread, co);
    const bool raisable = raise == 0 || lua_status(co) == LUA_YIELD;
    if (now != FBL_THREAD_SUSPENDED || fbl::is_runner(co) || co == found->main || !raisable) {
        lua_settop(thread, index - 1);
        const bool fresh = now == FBL_THREAD_SUSPENDED && !raisable;
        const char* why =
            fresh ? "that hasn't started, so can't have an error raised in it" : unresumable(now);
        return fbl::record_error(*found, FBL_RUNTIME,
                                 fbl::formatted("cannot resume a thread {}", why));
    }
    if (found->depth >= fbl::max_depth) {
        lua_settop(thread, index - 1);
        return fbl::record_error(
            *found, FBL_RUNTIME,
            fbl::formatted("calls into Luau and back went more than {} deep", fbl::max_depth));
    }
    if (lua_checkstack(co, count) == 0) {
        lua_settop(thread, index - 1);
        return fbl::record_error(*found, FBL_MEMORY, "not enough memory");
    }
    // The thread stays in its slot while it runs, so it's alive however it's held.
    lua_xmove(thread, co, count);
    const int status = fbl::resume_from(*found, thread, co, count, raise != 0);
    if (status != LUA_OK && status != LUA_YIELD) {
        const int32_t kind = fbl::failed(*found, thread, co, status, false);
        // An error ends it, as one that failed as it started, past Luau's C calls, would be left
        // suspended.
        if (status_of(thread, co) == FBL_THREAD_SUSPENDED) {
            lua_resetthread(co);
        }
        lua_settop(thread, index - 1);
        return kind;
    }
    const int made = lua_gettop(co);
    if (lua_checkstack(thread, made) == 0) {
        lua_settop(co, 0);
        lua_settop(thread, index - 1);
        return fbl::record_error(*found, FBL_MEMORY, "not enough memory");
    }
    lua_xmove(co, thread, made);
    lua_remove(thread, index);
    *finished = status == LUA_OK ? 1 : 0;
    return FBL_OK;
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_thread_close(uint64_t state, int32_t frame, int32_t slot) try {
    fbl::state_t* found = fbl::find_state(state);
    const fbl::frame_t* in = found == nullptr ? nullptr : fbl::find_frame(*found, frame);
    int index = 0;
    lua_State* co = in == nullptr ? nullptr : thread_at(*in, slot, index);
    if (co == nullptr) {
        return FBL_MISUSE;
    }
    const int32_t now = status_of(in->thread, co);
    if (now == FBL_THREAD_RUNNING || now == FBL_THREAD_NORMAL || fbl::is_runner(co) ||
        co == found->main) {
        return fbl::record_error(*found, FBL_RUNTIME, "cannot close a thread that's running");
    }
    lua_resetthread(co);
    return FBL_OK;
} catch (...) {
    return fbl::escaped();
}
