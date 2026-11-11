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

// Compiles and loads source, its environment the table `environment` is at on the frame's thread,
// or the thread's globals for 0.
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
    int environment = 0;
    if (environment_slot >= 0) {
        environment = fbl::slot_index(*in, environment_slot);
        if (environment == 0) {
            return FBL_MISUSE;
        }
        if (lua_type(in->thread, environment) != LUA_TTABLE) {
            return fbl::misuse(fbl::formatted("slot {} isn't a table", environment_slot));
        }
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
        const bool in_environment = environment != 0;
        // The environment goes to the work as its argument, a copy on top.
        if (in_environment) {
            if (lua_checkstack(in->thread, 1) == 0) {
                return fbl::record_error(*found, FBL_MEMORY, "not enough memory");
            }
            lua_pushvalue(in->thread, environment);
        }
        auto work = [code, name, in_environment](lua_State* stack) {
            if (!in_environment) {
                if (luau_load(stack, name, code.data(), code.size(), 0) != 0) {
                    lua_error(stack);
                }
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
            return 1;
        };
        const int arguments = in_environment ? 1 : 0;
        const int status = fbl::protect(*found, in->thread, arguments, 1, work);
        if (status == fbl::no_room) {
            lua_pop(in->thread, arguments);
        }
        if (!compiled && status == LUA_ERRRUN) {
            return fbl::take_error(*found, in->thread, FBL_SYNTAX);
        }
        return fbl::settle(*found, in->thread, status);
    } catch (const std::exception&) {
        return fbl::record_error(*found, FBL_MEMORY, "not enough memory to compile");
    }
}

} // namespace

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
