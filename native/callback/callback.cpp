#include "core/core.hpp"
#include "fbl.h"
#include "state/state.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <lua.h>
#include <lualib.h>
#include <string>

namespace {

// The program's callback, which every State's functions call.
std::atomic<fbl_callback_t> the_callback{ nullptr };

// What a program's function holds: its number, in a userdata Luau collects with the function.
struct function_box_t {
    int64_t function;
};

// A function's userdata's destructor, run as Luau collects it: it keeps the function's number for
// the program to take, in room kept for it, as a destructor mustn't need memory, or touch the VM.
// NOLINTNEXTLINE(misc-const-correctness): Luau's lua_Destructor takes a void*.
void released(lua_State* thread, void* data) {
    auto* state = static_cast<fbl::state_t*>(lua_callbacks(thread)->userdata);
    const auto* box = static_cast<const function_box_t*>(data);
    if (state == nullptr) {
        return;
    }
    if (state->released.size() < state->released.capacity()) {
        state->released.push_back(box->function);
    }
    state->live_functions--;
}

int call_back(lua_State* thread);

// What a type's __namecall is: the method's name, which Luau keeps apart from the arguments, goes
// first, then it's what call_back is.
int call_back_named(lua_State* thread) {
    const char* method = lua_namecallatom(thread, nullptr);
    if (method == nullptr) {
        luaL_error(thread, "a method is called with ':'");
    }
    lua_pushstring(thread, method);
    lua_insert(thread, 1);
    return call_back(thread);
}

// What each of the program's functions is in Luau: it calls the callback with its arguments as a
// frame, and gives what's left in it, or raises the error left in it. Luau calls it, so an error
// it raises here, from its own frame, unwinds only through Luau.
int call_back(lua_State* thread) {
    auto* state = static_cast<fbl::state_t*>(lua_callbacks(thread)->userdata);
    const auto* box = static_cast<const function_box_t*>(
        lua_touserdatatagged(thread, lua_upvalueindex(1), fbl::function_tag));
    const fbl_callback_t callback = the_callback.load();
    if (state == nullptr || box == nullptr || callback == nullptr) {
        luaL_error(thread, "the program's function can't be called: it has no callback");
    }
    state->frames.push_back({ .thread = thread, .base = 0 });
    const auto frame = static_cast<int32_t>(state->frames.size() - 1);
    lua_State* const before = state->current;
    state->current = thread;
    const int32_t status = callback(state->handle, frame, box->function);
    state->current = before;
    // Its frame, and any the program left open on it, which it shouldn't.
    state->frames.resize(std::min(state->frames.size(), static_cast<size_t>(frame)));
    if (status == FBL_OK) {
        return lua_gettop(thread);
    }
    if (lua_gettop(thread) == 0) {
        lua_pushliteral(thread, "the program's function failed, and gave no error");
    }
    lua_error(thread);
}

} // namespace

namespace fbl {

void prepare_callbacks(lua_State* thread) noexcept {
    lua_setuserdatadtor(thread, function_tag, released);
}

} // namespace fbl

bool fbl_callback_set(fbl_callback_t callback) try {
    if (callback == nullptr) {
        return fbl::fail("the callback can't be Null");
    }
    the_callback.store(callback);
    return true;
} catch (...) {
    fbl::escaped();
    return false;
}

int32_t fbl_callback_namecall(uint64_t state, int32_t frame, int64_t function,
                              const char* name) try {
    fbl::state_t* found = fbl::find_state(state);
    const fbl::frame_t* in = found == nullptr ? nullptr : fbl::find_frame(*found, frame);
    if (in == nullptr) {
        return FBL_MISUSE;
    }
    if (name == nullptr) {
        return fbl::misuse("a function needs a name");
    }
    const char* kept = nullptr;
    try {
        // Room for its number when it's collected, before it can be.
        found->released.reserve(static_cast<size_t>(found->live_functions + 1));
        kept = found->names.emplace(name).first->c_str();
    } catch (const std::exception&) {
        return fbl::record_error(*found, FBL_MEMORY, "not enough memory");
    }
    auto work = [function, kept](lua_State* stack) {
        auto* box = static_cast<function_box_t*>(
            lua_newuserdatatagged(stack, sizeof(function_box_t), fbl::function_tag));
        box->function = function;
        lua_pushcclosurek(stack, call_back_named, kept, 1, nullptr);
        return 1;
    };
    const int32_t status =
        fbl::settle(*found, in->thread, fbl::protect(*found, in->thread, 0, 1, work));
    if (status == FBL_OK) {
        found->live_functions++;
    }
    return status;
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_callback_function(uint64_t state, int32_t frame, int64_t function,
                              const char* name) try {
    fbl::state_t* found = fbl::find_state(state);
    const fbl::frame_t* in = found == nullptr ? nullptr : fbl::find_frame(*found, frame);
    if (in == nullptr) {
        return FBL_MISUSE;
    }
    if (name == nullptr) {
        return fbl::misuse("a function needs a name");
    }
    const char* kept = nullptr;
    try {
        // Room for its number when it's collected, before it can be.
        found->released.reserve(static_cast<size_t>(found->live_functions + 1));
        kept = found->names.emplace(name).first->c_str();
    } catch (const std::exception&) {
        return fbl::record_error(*found, FBL_MEMORY, "not enough memory");
    }
    auto work = [function, kept](lua_State* stack) {
        auto* box = static_cast<function_box_t*>(
            lua_newuserdatatagged(stack, sizeof(function_box_t), fbl::function_tag));
        box->function = function;
        lua_pushcclosurek(stack, call_back, kept, 1, nullptr);
        return 1;
    };
    const int32_t status =
        fbl::settle(*found, in->thread, fbl::protect(*found, in->thread, 0, 1, work));
    if (status == FBL_OK) {
        found->live_functions++;
    }
    return status;
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_callback_released(uint64_t state, int64_t* functions, int32_t capacity) try {
    fbl::state_t* found = fbl::find_state(state);
    if (found == nullptr) {
        return -1;
    }
    if (capacity < 0 || (functions == nullptr && capacity > 0)) {
        fbl::fail(fbl::formatted("{} functions can't go there", capacity));
        return -1;
    }
    const auto taken =
        static_cast<int32_t>(std::min(found->released.size(), static_cast<size_t>(capacity)));
    for (int32_t index = 0; index < taken; index++) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): C's array of them.
        functions[index] = found->released.back();
        found->released.pop_back();
    }
    return taken;
} catch (...) {
    fbl::escaped();
    return -1;
}

int32_t fbl_callback_where(uint64_t state, int32_t frame) try {
    fbl::state_t* found = fbl::find_state(state);
    const fbl::frame_t* in = found == nullptr ? nullptr : fbl::find_frame(*found, frame);
    if (in == nullptr) {
        return FBL_MISUSE;
    }
    // Under protection, the work is a C function called from the program's function, which was
    // called from where Luau's code is: 2 levels up.
    auto work = [](lua_State* stack) {
        luaL_where(stack, 2);
        return 1;
    };
    return fbl::settle(*found, in->thread, fbl::protect(*found, in->thread, 0, 1, work));
} catch (...) {
    return fbl::escaped();
}
