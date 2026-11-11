#pragma once

// What the shim's tests share: a State made and closed around a test, and running source on it as
// the package does, through a frame.

#include "fbl.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <string_view>

// The options the package makes a State with by default.
inline fbl_state_options_t default_options() {
    return {
        .memory_limit = 0,
        .time_budget = 0,
        .optimization = 1,
        .debug = 1,
        .coverage = 0,
        .libraries = 0xFFF,
        .collect_garbage = FBL_COLLECT_COUNT,
        .load_string = false,
        .environments = false,
        .unused = {},
    };
}

// The bytes of a string, as C takes them.
inline const uint8_t* bytes_of(std::string_view text) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): chars, as C's bytes.
    return reinterpret_cast<const uint8_t*>(text.data());
}

inline std::string text_of(const uint8_t* bytes, int64_t length) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): C's bytes, as chars.
    return { reinterpret_cast<const char*>(bytes), static_cast<size_t>(length) };
}

struct test_state_t {
    uint64_t handle = 0;

    explicit test_state_t(const fbl_state_options_t& options = default_options()) {
        int32_t status = FBL_OTHER;
        handle = fbl_state_new(&options, &status);
        REQUIRE(status == FBL_OK);
        REQUIRE(handle != 0);
    }

    test_state_t(const test_state_t&) = delete;
    test_state_t& operator=(const test_state_t&) = delete;
    test_state_t(test_state_t&&) = delete;
    test_state_t& operator=(test_state_t&&) = delete;

    ~test_state_t() {
        if (handle != 0) {
            fbl_state_close(handle);
        }
    }

    // Compiles and runs `source` in a frame of its own, as `name`: the status.
    [[nodiscard]] int32_t run(std::string_view source, const char* name = "=test") const {
        const int32_t frame = fbl_frame_open(handle);
        REQUIRE(frame >= 0);
        int32_t status = fbl_load_source(handle, frame, bytes_of(source),
                                         static_cast<int64_t>(source.size()), name);
        if (status == FBL_OK) {
            status = fbl_call_function(handle, frame, 0, 0);
        }
        REQUIRE(fbl_frame_close(handle, frame));
        return status;
    }

    [[nodiscard]] std::string error_message() const {
        int64_t length = 0;
        const uint8_t* bytes = fbl_state_error_message(handle, &length);
        return bytes == nullptr ? std::string() : text_of(bytes, length);
    }

    [[nodiscard]] std::string error_traceback() const {
        int64_t length = 0;
        const uint8_t* bytes = fbl_state_error_traceback(handle, &length);
        return bytes == nullptr ? std::string() : text_of(bytes, length);
    }
};
