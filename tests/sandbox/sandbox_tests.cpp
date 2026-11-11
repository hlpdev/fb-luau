#include "fbl.h"
#include "support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

// Runs `source` in a new environment of the State's: the status.
int32_t run_in_environment(const test_state_t& lua, std::string_view source) {
    const int32_t frame = fbl_frame_open(lua.handle);
    REQUIRE(fbl_sandbox_environment(lua.handle, frame) == FBL_OK);
    int32_t status = fbl_load_source_in(lua.handle, frame, bytes_of(source),
                                        static_cast<int64_t>(source.size()), "=script", 0);
    if (status == FBL_OK) {
        status = fbl_call_function(lua.handle, frame, 1, 0);
    }
    REQUIRE(fbl_frame_close(lua.handle, frame));
    return status;
}

} // namespace

TEST_CASE("freezing makes the globals and libraries read-only, once") {
    const test_state_t lua;
    CHECK(fbl_sandbox_is_frozen(lua.handle) == 0);
    REQUIRE(fbl_sandbox_freeze(lua.handle) == FBL_OK);
    REQUIRE(fbl_sandbox_freeze(lua.handle) == FBL_OK);
    CHECK(fbl_sandbox_is_frozen(lua.handle) == 1);
    CHECK(lua.run("x = 1") == FBL_RUNTIME);
    CHECK(lua.run("string.upper = nil") == FBL_RUNTIME);
    CHECK(lua.run("assert(string.upper('a') == 'A')") == FBL_OK);
}

TEST_CASE("an environment is a script's own, over the frozen globals") {
    const test_state_t lua;
    REQUIRE(lua.run("shared = 5") == FBL_OK);
    CHECK(run_in_environment(lua, "mine = 1 assert(shared == 5) assert(_G.mine == 1)") == FBL_OK);
    CHECK(fbl_sandbox_is_frozen(lua.handle) == 1);
    CHECK(run_in_environment(lua, "assert(mine == nil)") == FBL_OK);
    CHECK(run_in_environment(lua, "shared = 6 assert(shared == 6)") == FBL_OK);
    CHECK(run_in_environment(lua, "assert(shared == 5)") == FBL_OK);
    CHECK(run_in_environment(lua, "setmetatable(_G, nil)") == FBL_RUNTIME);
    CHECK(run_in_environment(lua, "math.pi = 1") == FBL_RUNTIME);
}

TEST_CASE("a call past its time budget is a timeout, which a pcall can't keep") {
    fbl_state_options_t options = default_options();
    options.time_budget = 20000;
    const test_state_t lua(options);
    const auto started = std::chrono::steady_clock::now();
    CHECK(lua.run("while true do pcall(function() while true do end end) end") == FBL_TIMEOUT);
    CHECK(lua.error_message().contains("time budget of 20000 microseconds"));
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
    CHECK(fbl_state_error_kind(lua.handle) == FBL_TIMEOUT);
    // The next call has its own.
    CHECK(lua.run("local n = 0 for i = 1, 1000 do n += i end") == FBL_OK);
}

TEST_CASE("the libraries, collectgarbage, loadstring, and environments are the options'") {
    fbl_state_options_t options = default_options();
    options.libraries = static_cast<int32_t>(static_cast<uint32_t>(FBL_LIBRARY_BASE) |
                                             static_cast<uint32_t>(FBL_LIBRARY_STRING));
    options.collect_garbage = FBL_COLLECT_NONE;
    const test_state_t narrow(options);
    CHECK(narrow.run("assert(math == nil and os == nil and collectgarbage == nil)") == FBL_OK);
    CHECK(narrow.run("assert(getfenv == nil and loadstring == nil)") == FBL_OK);
    options = default_options();
    options.collect_garbage = FBL_COLLECT_FULL;
    options.load_string = true;
    options.environments = true;
    const test_state_t wide(options);
    CHECK(wide.run("collectgarbage() assert(loadstring('return 1')() == 1) assert(getfenv)") ==
          FBL_OK);
}

TEST_CASE("a State's options out of range are refused") {
    int32_t status = FBL_OK;
    fbl_state_options_t options = default_options();
    options.time_budget = -1;
    CHECK(fbl_state_new(&options, &status) == 0);
    CHECK(status == FBL_MISUSE);
    options = default_options();
    options.libraries = 0x1000;
    CHECK(fbl_state_new(&options, &status) == 0);
    options = default_options();
    options.collect_garbage = 3;
    CHECK(fbl_state_new(&options, &status) == 0);
    CHECK(status == FBL_MISUSE);
}

TEST_CASE("a load into what isn't a table is refused") {
    const test_state_t lua;
    const int32_t frame = fbl_frame_open(lua.handle);
    REQUIRE(fbl_frame_push_number(lua.handle, frame, 1) == FBL_OK);
    CHECK(fbl_load_source_in(lua.handle, frame, bytes_of("x"), 1, "=x", 0) == FBL_MISUSE);
    CHECK(fbl_load_source_in(lua.handle, frame, bytes_of("x"), 1, "=x", -1) == FBL_MISUSE);
    CHECK(fbl_load_source_in(lua.handle, frame, bytes_of("x"), 1, "=x", 5) == FBL_MISUSE);
    CHECK(fbl_frame_count(lua.handle, frame) == 1);
    CHECK(fbl_frame_close(lua.handle, frame));
}
