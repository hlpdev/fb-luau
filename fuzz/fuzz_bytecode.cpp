// Bytecode: arbitrary bytes as source, and what Luau's compiler makes of it, which must load and
// run as the source would. Raw bytecode isn't fuzzed into Luau, which trusts its bytecode by
// design, and the package never gives it untrusted bytes: its container, checked in FutureBlitz, is
// fuzzed by fuzz/containers.

#include "fuzz.hpp"

#include <cstddef>
#include <cstdint>

// NOLINTNEXTLINE(readability-identifier-naming): libFuzzer names it.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    int64_t length = 0;
    int32_t status = FBL_OTHER;
    // The compile options, from the input's first byte, each from 0 to 2.
    const uint8_t knobs = size == 0 ? 0 : data[0];
    uint8_t* bytecode =
        fbl_bytecode_compile(size == 0 ? nullptr : data, static_cast<int64_t>(size), knobs % 3,
                             (knobs / 3) % 3, (knobs / 9) % 3, &length, &status);
    if ((status == FBL_OK) != (bytecode != nullptr)) {
        finding("a compile's status and its bytecode disagree");
    }
    if (status != FBL_OK) {
        if (status != FBL_SYNTAX && status != FBL_MEMORY) {
            finding("a compile failed with what isn't a syntax or memory error");
        }
        return 0;
    }
    {
        const fuzz_state_t lua;
        if (lua.handle() != 0) {
            const int32_t frame = fbl_frame_open(lua.handle());
            int32_t ran = fbl_bytecode_load(lua.handle(), frame, bytecode, length, "=fuzz", -1);
            if (ran != FBL_OK) {
                finding("the compiler's own bytecode didn't load");
            }
            ran = fbl_call_function(lua.handle(), frame, 0, 0);
            if (!is_status(ran) || ran == FBL_MISUSE || ran == FBL_SYNTAX) {
                finding("running compiled bytecode gave what isn't a script's status");
            }
            fbl_frame_close(lua.handle(), frame);
        }
    }
    expect_nothing_left();
    fbl_bytecode_free(bytecode);
    return 0;
}
