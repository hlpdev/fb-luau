#pragma once

// What the fuzzers share: a State with small limits, made and closed around one input, and the
// checks that make a finding an abort.

#include "fbl.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

// Stops the run, saying why, which libFuzzer and replay.cpp take as a finding.
[[noreturn]] inline void finding(const char* what) noexcept {
    std::fputs("finding: ", stderr);
    std::fputs(what, stderr);
    std::fputs("\n", stderr);
    std::abort();
}

// Whether `status` is one the C API gives.
inline bool is_status(int32_t status) {
    return status >= FBL_OK && status <= FBL_MISUSE;
}

// The options a fuzzer's State has: every library, a small memory limit, and a short time budget,
// so an input can't take the run's memory or time.
inline fbl_state_options_t fuzz_options() {
    return {
        .memory_limit = int64_t{ 8 } * 1024 * 1024,
        .time_budget = 20000,
        .optimization = 1,
        .debug = 1,
        .coverage = 0,
        .libraries = 0xFFF,
        .collect_garbage = FBL_COLLECT_FULL,
        .native_code = FBL_NATIVE_OFF,
        .load_string = true,
        .environments = true,
        .unused = {},
    };
}

// Checks that no State of this thread's, nor its memory, outlived its close.
inline void expect_nothing_left() {
    if (fbl_state_count(1) != 0 || fbl_state_memory_all(1) != 0) {
        finding("a State, or its memory, outlived its close");
    }
}

// A State made for one input, and closed after it.
class fuzz_state_t {
public:
    explicit fuzz_state_t(const fbl_state_options_t& options = fuzz_options()) {
        int32_t status = FBL_OTHER;
        m_handle = fbl_state_new(&options, &status);
        if (!is_status(status) || (status == FBL_OK) != (m_handle != 0)) {
            finding("fbl_state_new gave a status that isn't one, or a handle with an error");
        }
    }
    ~fuzz_state_t() {
        if (m_handle != 0 && !fbl_state_close(m_handle)) {
            finding("a State didn't close");
        }
    }
    fuzz_state_t(const fuzz_state_t&) = delete;
    fuzz_state_t& operator=(const fuzz_state_t&) = delete;
    fuzz_state_t(fuzz_state_t&&) = delete;
    fuzz_state_t& operator=(fuzz_state_t&&) = delete;

    [[nodiscard]] uint64_t handle() const {
        return m_handle;
    }

private:
    uint64_t m_handle = 0;
};
