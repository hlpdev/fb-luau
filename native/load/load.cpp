#include "core/core.hpp"
#include "fbl.h"
#include "state/state.hpp"

#include <cstdlib>
#include <exception>
#include <lua.h>
#include <luacode.h>
#include <memory>
#include <string_view>

namespace {

// Compiles and loads source, its environment the table `environment_slot` is at, or the frame's
// thread's globals for -1.
int32_t load(uint64_t state, int32_t frame, const uint8_t* source, int64_t length, const char* name,
             int32_t environment_slot) {
    fbl::state_t* found = fbl::find_state(state);
    const fbl::frame_t* in = found == nullptr ? nullptr : fbl::find_frame(*found, frame);
    if (in == nullptr) {
        return FBL_MISUSE;
    }
    if (length < 0 || (source == nullptr && length > 0)) {
        return fbl::misuse(fbl::formatted("{} bytes of source aren't source", length));
    }
    if (name == nullptr) {
        return fbl::misuse("a chunk needs a name");
    }
    const int environment = fbl::environment_index(*in, environment_slot);
    if (environment < 0) {
        return FBL_MISUSE;
    }
    try {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): C's bytes, as chars.
        const char* text = length == 0 ? "" : reinterpret_cast<const char*>(source);
        size_t size = 0;
        const std::unique_ptr<char, decltype(&std::free)> bytecode(
            luau_compile(text, static_cast<size_t>(length), &found->compile_options, &size),
            &std::free);
        if (bytecode == nullptr) {
            return fbl::record_error(*found, FBL_MEMORY, "not enough memory to compile");
        }
        // What doesn't compile is bytecode that starts with a 0, then its error, which luau_load
        // gives as its own.
        const std::string_view code(bytecode.get(), size);
        const bool compiled = !code.empty() && code[0] != 0;
        return fbl::load_chunk(*found, in->thread, environment, code, name, !compiled);
    } catch (const std::exception&) {
        return fbl::record_error(*found, FBL_MEMORY, "not enough memory to compile");
    }
}

} // namespace

namespace fbl {

int environment_index(const frame_t& frame, int32_t environment_slot) noexcept {
    if (environment_slot == -1) {
        return 0;
    }
    const int index = slot_index(frame, environment_slot);
    if (index == 0) {
        return -1;
    }
    if (lua_type(frame.thread, index) != LUA_TTABLE) {
        misuse(formatted("slot {} isn't a table", environment_slot));
        return -1;
    }
    return index;
}

int32_t load_chunk(state_t& state, lua_State* thread, int environment, std::string_view code,
                   const char* name, bool syntax) noexcept {
    const bool in_environment = environment != 0;
    // The environment goes to the work as its argument, a copy on top.
    if (in_environment) {
        if (lua_checkstack(thread, 1) == 0) {
            return record_error(state, FBL_MEMORY, "not enough memory");
        }
        lua_pushvalue(thread, environment);
    }
    state_t* const loading = &state;
    auto work = [code, name, in_environment, loading](lua_State* stack) {
        if (!in_environment) {
            if (luau_load(stack, name, code.data(), code.size(), 0) != 0) {
                lua_error(stack);
            }
            native_compile(*loading, stack, -1);
            return 1;
        }
        // Luau resolves a chunk's globals as it loads it from the loading thread's globals, not
        // from the environment it's given, so it's loaded on a thread whose globals are it, as
        // Luau's CLI does with luaL_sandboxthread.
        lua_State* loader = lua_newthread(stack);
        lua_pushvalue(stack, 1);
        lua_xmove(stack, loader, 1);
        lua_replace(loader, LUA_GLOBALSINDEX);
        const int failed = luau_load(loader, name, code.data(), code.size(), 0);
        lua_xmove(loader, stack, 1);
        if (failed != 0) {
            lua_error(stack);
        }
        native_compile(*loading, stack, -1);
        return 1;
    };
    const int arguments = in_environment ? 1 : 0;
    const int status = protect(state, thread, arguments, 1, work);
    if (status == no_room) {
        lua_pop(thread, arguments);
    }
    if (syntax && status == LUA_ERRRUN) {
        return take_error(state, thread, FBL_SYNTAX);
    }
    return settle(state, thread, status);
}

} // namespace fbl

int32_t fbl_load_source(uint64_t state, int32_t frame, const uint8_t* source, int64_t length,
                        const char* name) try {
    return load(state, frame, source, length, name, -1);
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_load_source_in(uint64_t state, int32_t frame, const uint8_t* source, int64_t length,
                           const char* name, int32_t environment_slot) try {
    if (environment_slot < 0) {
        return fbl::misuse("an environment's slot is from 0");
    }
    return load(state, frame, source, length, name, environment_slot);
} catch (...) {
    return fbl::escaped();
}
