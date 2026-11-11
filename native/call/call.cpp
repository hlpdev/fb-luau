#include "core/core.hpp"
#include "fbl.h"
#include "state/state.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <lua.h>
#include <lualib.h>
#include <string>

namespace {

// The thread calls at `depth` run on, made the first time one's that deep, and kept by the
// registry; or null, with the State's error.
lua_State* runner(fbl::state_t& state, lua_State* thread, int depth) {
    if (std::cmp_less(depth, state.runner_refs.size())) {
        lua_getref(thread, state.runner_refs[static_cast<size_t>(depth)]);
        lua_State* found = lua_tothread(thread, -1);
        lua_pop(thread, 1);
        return found;
    }
    try {
        state.runner_refs.reserve(state.runner_refs.size() + 1);
    } catch (const std::exception&) {
        fbl::record_error(state, FBL_MEMORY, "not enough memory");
        return nullptr;
    }
    int made = LUA_NOREF;
    auto work = [&made](lua_State* stack) {
        lua_newthread(stack);
        made = lua_ref(stack, -1);
        lua_pop(stack, 1);
        return 0;
    };
    if (fbl::settle(state, thread, fbl::protect(state, thread, 0, 0, work)) != FBL_OK) {
        return nullptr;
    }
    state.runner_refs.push_back(made);
    return runner(state, thread, depth);
}

// Keeps the error a runner's call met, the value on top of its stack, with where it was raised, and
// makes the runner ready for its next call.
int32_t failed(fbl::state_t& state, lua_State* thread, lua_State* runner, int status) noexcept {
    std::string traceback;
    // Its traceback is made of strings, which need memory, so it's made under protection, on the
    // caller's thread, from the runner's frames, which an error leaves where they were.
    auto work = [runner](lua_State* stack) {
        luaL_traceback(stack, runner, nullptr, 0);
        return 1;
    };
    const int made = fbl::protect(state, thread, 0, 1, work);
    if (made == LUA_OK) {
        size_t length = 0;
        const char* text = lua_tolstring(thread, -1, &length);
        try {
            traceback.assign(text, length);
        } catch (const std::exception&) {
            traceback.clear();
        }
    }
    // The traceback, or the error making it met, but nothing when there was no room for either.
    if (made != fbl::no_room) {
        lua_pop(thread, 1);
    }
    if (!traceback.empty() && traceback.back() == '\n') {
        traceback.pop_back();
    }
    // An error the interrupt raised, or any the timeout led to, is the timeout.
    const int32_t kind =
        state.timed_out && status == LUA_ERRRUN ? FBL_TIMEOUT : fbl::kind_of(status);
    if (status == LUA_YIELD) {
        fbl::record_error(state, FBL_YIELD,
                          "attempt to yield where nothing can wait for it: a call from the "
                          "program runs to its end",
                          traceback);
    } else if (lua_gettop(runner) > 0) {
        fbl::take_error(state, runner, kind, traceback);
    } else {
        fbl::record_error(state, kind, "an error with no value", traceback);
    }
    lua_resetthread(runner);
    return kind;
}

} // namespace

int32_t fbl_call_function(uint64_t state, int32_t frame, int32_t slot, int32_t results) try {
    fbl::state_t* found = fbl::find_state(state);
    const fbl::frame_t* in = found == nullptr ? nullptr : fbl::find_frame(*found, frame);
    const int index = in == nullptr ? 0 : fbl::slot_index(*in, slot);
    if (index == 0) {
        return FBL_MISUSE;
    }
    lua_State* thread = in->thread;
    if (lua_type(thread, index) != LUA_TFUNCTION) {
        return fbl::misuse(fbl::formatted("slot {} isn't a function", slot));
    }
    if (results < -1) {
        return fbl::misuse(fbl::formatted("a call can't give {} results", results));
    }
    const int count = lua_gettop(thread) - index + 1;
    if (found->depth >= fbl::max_depth) {
        lua_settop(thread, index - 1);
        return fbl::record_error(
            *found, FBL_RUNTIME,
            fbl::formatted("calls into Luau and back went more than {} deep", fbl::max_depth));
    }
    lua_State* co = runner(*found, thread, found->depth);
    if (co == nullptr || lua_checkstack(co, count) == 0) {
        lua_settop(thread, index - 1);
        return co == nullptr ? found->error_kind
                             : fbl::record_error(*found, FBL_MEMORY, "not enough memory");
    }
    lua_xmove(thread, co, count);
    const bool outermost = found->depth == 0;
    if (outermost && found->time_budget.count() > 0) {
        found->timed_out = false;
        found->deadline = std::chrono::steady_clock::now() + found->time_budget;
    }
    found->depth++;
    const int status = lua_resume(co, thread, count - 1);
    found->depth--;
    if (outermost) {
        found->deadline = {};
    }
    if (status != LUA_OK) {
        return failed(*found, thread, co, status);
    }
    const int made = lua_gettop(co);
    const int wanted = results == -1 ? made : results;
    if (lua_checkstack(thread, wanted) == 0) {
        lua_resetthread(co);
        return fbl::record_error(*found, FBL_MEMORY, "not enough memory");
    }
    const int moved = std::min(made, wanted);
    lua_settop(co, moved);
    lua_xmove(co, thread, moved);
    for (int extra = moved; extra < wanted; extra++) {
        lua_pushnil(thread);
    }
    return FBL_OK;
} catch (...) {
    return fbl::escaped();
}
