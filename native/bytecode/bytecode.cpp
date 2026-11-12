#include "core/core.hpp"
#include "fbl.h"
#include "state/state.hpp"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <lua.h>
#include <luacode.h>
#include <string_view>

uint8_t* fbl_bytecode_compile(const uint8_t* source, int64_t length, int32_t optimization,
                              int32_t debug, int32_t coverage, int64_t* size, int32_t* status) try {
    int32_t ignored = FBL_OK;
    int32_t& said = status != nullptr ? *status : ignored;
    said = FBL_MISUSE;
    if (size == nullptr) {
        fbl::fail("a compile needs somewhere to give its bytecode's size");
        return nullptr;
    }
    if (length < 0 || (source == nullptr && length > 0)) {
        fbl::fail(fbl::formatted("{} bytes of source aren't source", length));
        return nullptr;
    }
    for (const int32_t level : { optimization, debug, coverage }) {
        if (level < 0 || level > 2) {
            fbl::fail(fbl::formatted("a compile option of {} isn't from 0 to 2", level));
            return nullptr;
        }
    }
    lua_CompileOptions options{};
    options.optimizationLevel = optimization;
    options.debugLevel = debug;
    options.coverageLevel = coverage;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): C's bytes, as chars.
    const char* text = length == 0 ? "" : reinterpret_cast<const char*>(source);
    size_t made = 0;
    char* bytecode = luau_compile(text, static_cast<size_t>(length), &options, &made);
    if (bytecode == nullptr) {
        said = FBL_MEMORY;
        fbl::fail("not enough memory to compile");
        return nullptr;
    }
    // What doesn't compile is a 0, then its error.
    if (made == 0 || bytecode[0] == 0) {
        const std::string_view error = made == 0 ? "" : std::string_view(bytecode + 1, made - 1);
        said = FBL_SYNTAX;
        fbl::fail(error);
        std::free(bytecode);
        return nullptr;
    }
    *size = static_cast<int64_t>(made);
    said = FBL_OK;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the bytes, as C takes them.
    return reinterpret_cast<uint8_t*>(bytecode);
} catch (...) {
    fbl::escaped();
    if (status != nullptr) {
        *status = FBL_OTHER;
    }
    return nullptr;
}

// NOLINTNEXTLINE(misc-const-correctness): what fbl_bytecode_compile gave, to free.
void fbl_bytecode_free(uint8_t* bytecode) {
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc): luau_compile's malloc.
    std::free(bytecode);
}

int32_t fbl_bytecode_load(uint64_t state, int32_t frame, const uint8_t* bytecode, int64_t length,
                          const char* name, int32_t environment_slot) try {
    fbl::state_t* found = fbl::find_state(state);
    const fbl::frame_t* in = found == nullptr ? nullptr : fbl::find_frame(*found, frame);
    if (in == nullptr) {
        return FBL_MISUSE;
    }
    // Bytecode that starts with a 0 is a compile's error, which luau_load would give as its own.
    if (bytecode == nullptr || length <= 0 || bytecode[0] == 0) {
        return fbl::misuse("bytecode is what a compile made, which starts with its version");
    }
    if (name == nullptr) {
        return fbl::misuse("a chunk needs a name");
    }
    const int environment = fbl::environment_index(*in, environment_slot);
    if (environment < 0) {
        return FBL_MISUSE;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): C's bytes, as chars.
    const std::string_view code(reinterpret_cast<const char*>(bytecode),
                                static_cast<size_t>(length));
    return fbl::load_chunk(*found, in->thread, environment, code, name, false);
} catch (...) {
    return fbl::escaped();
}
