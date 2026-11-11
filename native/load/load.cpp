#include "core/core.hpp"
#include "fbl.h"
#include "state/state.hpp"

#include <cstdlib>
#include <exception>
#include <lua.h>
#include <luacode.h>
#include <memory>
#include <string_view>

int32_t fbl_load_source(uint64_t state, int32_t frame, const uint8_t* source, int64_t length,
                        const char* name) try {
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
        auto work = [code, name](lua_State* stack) {
            if (luau_load(stack, name, code.data(), code.size(), 0) != 0) {
                lua_error(stack);
            }
            return 1;
        };
        const int status = fbl::protect(*found, in->thread, 0, 1, work);
        if (!compiled && status == LUA_ERRRUN) {
            return fbl::take_error(*found, in->thread, FBL_SYNTAX);
        }
        return fbl::settle(*found, in->thread, status);
    } catch (const std::exception&) {
        return fbl::record_error(*found, FBL_MEMORY, "not enough memory to compile");
    }
} catch (...) {
    return fbl::escaped();
}
