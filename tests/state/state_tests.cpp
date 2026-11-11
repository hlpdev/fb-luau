#include "fbl.h"
#include "state/state.hpp"
#include "support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

TEST_CASE("a State is made, used, and closed, and every byte it held comes back") {
    const int64_t before = fbl::memory_of_all_states();
    {
        const test_state_t lua;
        CHECK(fbl_state_memory(lua.handle) > 0);
        CHECK(fbl::memory_of_all_states() > before);
        CHECK(lua.run("x = string.rep('a', 100000)") == FBL_OK);
    }
    CHECK(fbl::memory_of_all_states() == before);
}

TEST_CASE("a State's options are checked") {
    fbl_state_options_t options = default_options();
    int32_t status = FBL_OK;
    CHECK(fbl_state_new(nullptr, &status) == 0);
    CHECK(status == FBL_MISUSE);
    options.memory_limit = -1;
    CHECK(fbl_state_new(&options, &status) == 0);
    CHECK(status == FBL_MISUSE);
    CHECK(std::string_view(fbl_core_error()).contains("below 0"));
    options = default_options();
    options.optimization = 3;
    CHECK(fbl_state_new(&options, &status) == 0);
    CHECK(status == FBL_MISUSE);
}

TEST_CASE("a State too small for its limit isn't made") {
    fbl_state_options_t options = default_options();
    options.memory_limit = 1000;
    int32_t status = FBL_OK;
    const int64_t before = fbl::memory_of_all_states();
    CHECK(fbl_state_new(&options, &status) == 0);
    CHECK(status == FBL_MEMORY);
    CHECK(fbl::memory_of_all_states() == before);
}

TEST_CASE("a closed State's handle is refused, even once its slot holds another") {
    test_state_t first;
    const uint64_t handle = first.handle;
    REQUIRE(fbl_state_close(handle));
    first.handle = 0;
    CHECK_FALSE(fbl_state_close(handle));
    CHECK(fbl_state_memory(handle) == -1);
    CHECK(std::string_view(fbl_core_error()) == "the State is closed");
    const test_state_t second;
    CHECK(second.handle != handle);
    CHECK(fbl_state_memory(handle) == -1);
    CHECK(fbl_state_memory(second.handle) > 0);
}

TEST_CASE("a handle of another kind, or none, is refused") {
    CHECK(fbl_state_memory(0) == -1);
    CHECK(fbl_state_memory(0x100000002ULL) == -1);
    CHECK(std::string_view(fbl_core_error()).contains("isn't a State's handle"));
}

TEST_CASE("a State is only its own thread's") {
    const test_state_t lua;
    int64_t memory = 0;
    std::string error;
    std::thread other([&] {
        memory = fbl_state_memory(lua.handle);
        error = fbl_core_error();
    });
    other.join();
    CHECK(memory == -1);
    CHECK(error.contains("another thread's"));
}

TEST_CASE("a State isn't closed while a frame is open on it") {
    const test_state_t lua;
    const int32_t frame = fbl_frame_open(lua.handle);
    REQUIRE(frame == 0);
    CHECK_FALSE(fbl_state_close(lua.handle));
    CHECK(fbl_frame_close(lua.handle, frame));
}

TEST_CASE("a script past the memory limit gets an error, and the State goes on") {
    fbl_state_options_t options = default_options();
    options.memory_limit = int64_t{ 1024 } * 1024;
    const test_state_t lua(options);
    CHECK(lua.run("local parts = {} for i = 1, 1e7 do parts[i] = string.rep('x', 100) .. i end") ==
          FBL_MEMORY);
    CHECK(lua.error_message() == "not enough memory");
    CHECK(fbl_state_memory(lua.handle) <= options.memory_limit);
    CHECK(fbl_state_collect(lua.handle) == FBL_OK);
    CHECK(lua.run("small = 1 + 1") == FBL_OK);
}

TEST_CASE("Luau's own libraries are open, the flags' among them") {
    const test_state_t lua;
    CHECK(lua.run("assert(type(string.format) == 'function')") == FBL_OK);
    CHECK(lua.run("assert(type(integer) == 'table')") == FBL_OK);
}
