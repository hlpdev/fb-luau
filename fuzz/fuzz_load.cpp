// The loader: arbitrary bytes as source, compiled and run in a State with a small memory limit and
// time budget, which must give a status, never crash, hang past its budget, or leak a State or its
// memory.

#include "fuzz.hpp"

#include <cstddef>
#include <cstdint>

namespace {

void run(const uint8_t* data, size_t size) {
    const fuzz_state_t lua;
    if (lua.handle() == 0) {
        return;
    }
    const int32_t frame = fbl_frame_open(lua.handle());
    if (frame < 0) {
        finding("a new State's first frame didn't open");
    }
    int32_t status = fbl_load_source(lua.handle(), frame, size == 0 ? nullptr : data,
                                     static_cast<int64_t>(size), "=fuzz");
    if (status == FBL_OK) {
        status = fbl_call_function(lua.handle(), frame, 0, 0);
    }
    if (!is_status(status) || status == FBL_MISUSE) {
        finding("loading or running source gave what isn't a script's status");
    }
    if (!fbl_frame_close(lua.handle(), frame)) {
        finding("the frame didn't close");
    }
}

} // namespace

// NOLINTNEXTLINE(readability-identifier-naming): libFuzzer names it.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    run(data, size);
    expect_nothing_left();
    return 0;
}
