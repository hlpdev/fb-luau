#include "fbl.h"
#include "state/state.hpp"
#include "support.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <vector>
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

TEST_CASE("a State with a frame the program left open closes, the frame with it") {
    int32_t status = FBL_OTHER;
    const fbl_state_options_t options = default_options();
    const uint64_t handle = fbl_state_new(&options, &status);
    REQUIRE(handle != 0);
    const int32_t frame = fbl_frame_open(handle);
    REQUIRE(frame == 0);
    REQUIRE(fbl_frame_push_number(handle, frame, 1) == FBL_OK);
    CHECK(fbl_state_close(handle));
    CHECK_FALSE(fbl_frame_close(handle, frame));
    CHECK(fbl_frame_count(handle, frame) == -1);
}

TEST_CASE("a slot whose generations are spent is retired, so no handle names two States") {
    int32_t status = FBL_OTHER;
    const fbl_state_options_t options = default_options();
    const uint64_t first = fbl_state_new(&options, &status);
    REQUIRE(first != 0);
    fbl::set_generation(first, 0xFFFFFFU);
    REQUIRE(fbl::find_state(first) == nullptr);
    // The State's handle is its new one now: its slot, its last generation, and a State's kind.
    const uint64_t last = ((first >> 32U) << 32U) | (uint64_t{ 0xFFFFFFU } << 8U) | 1U;
    REQUIRE(fbl::find_state(last) != nullptr);
    REQUIRE(fbl_state_close(last));
    // Its slot isn't used again, so the next State's handle is another slot's.
    const uint64_t next = fbl_state_new(&options, &status);
    REQUIRE(next != 0);
    CHECK((next >> 32U) != (last >> 32U));
    CHECK(fbl::find_state(last) == nullptr);
    CHECK(fbl_state_close(next));
}

TEST_CASE("a stale handle another thread asks with is refused while States close and open") {
    int32_t status = FBL_OTHER;
    const fbl_state_options_t options = default_options();
    const uint64_t stale = fbl_state_new(&options, &status);
    REQUIRE(fbl_state_close(stale));
    std::atomic<bool> done{ false };
    std::atomic<int64_t> answered{ 0 };
    std::thread asker([&] {
        while (!done.load()) {
            if (fbl_state_memory(stale) == -1) {
                answered++;
            }
        }
    });
    for (int round = 0; round < 200; round++) {
        const uint64_t made = fbl_state_new(&options, &status);
        REQUIRE(made != 0);
        CHECK(fbl_state_close(made));
    }
    done.store(true);
    asker.join();
    CHECK(answered.load() > 0);
}

TEST_CASE("what a frame's functions give for a slot or frame that isn't there is -1") {
    const test_state_t lua;
    CHECK(fbl_frame_count(lua.handle, 3) == -1);
    CHECK(fbl_frame_type(lua.handle, 3, 0) == -1);
    CHECK(fbl_frame_open(0) == -1);
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

TEST_CASE("an error with no value never gives its slot to another reference") {
    const test_state_t lua;
    // Before any error, there's no value.
    int32_t frame = fbl_frame_open(lua.handle);
    REQUIRE(fbl_state_error_value(lua.handle, frame) == FBL_OK);
    CHECK(fbl_frame_type(lua.handle, frame, 0) == FBL_TYPE_NIL);
    CHECK(fbl_frame_close(lua.handle, frame));
    CHECK(lua.run("error(nil)") == FBL_RUNTIME);
    frame = fbl_frame_open(lua.handle);
    std::vector<int32_t> refs;
    for (int index = 0; index < 100; index++) {
        REQUIRE(fbl_frame_push_number(lua.handle, frame, index) == FBL_OK);
        int32_t ref = 0;
        REQUIRE(fbl_ref_new(lua.handle, frame, 0, &ref) == FBL_OK);
        refs.push_back(ref);
        fbl_frame_truncate(lua.handle, frame, 0);
    }
    CHECK(fbl_frame_close(lua.handle, frame));
    CHECK(lua.run("error('another')") == FBL_RUNTIME);
    frame = fbl_frame_open(lua.handle);
    for (size_t index = 0; index < refs.size(); index++) {
        REQUIRE(fbl_ref_push(lua.handle, frame, refs[index]) == FBL_OK);
        double value = -1;
        CHECK(fbl_frame_number(lua.handle, frame, 0, &value));
        CHECK(value == static_cast<double>(index));
        fbl_frame_truncate(lua.handle, frame, 0);
    }
    CHECK(fbl_frame_close(lua.handle, frame));
}

TEST_CASE("with a memory limit, garbage alone doesn't reach it") {
    fbl_state_options_t options = default_options();
    options.memory_limit = int64_t{ 4 } * 1024 * 1024;
    const test_state_t lua(options);
    // What's live is 60% of the limit, and a loop makes ten times the limit in garbage.
    REQUIRE(lua.run("kept = buffer.create(2500000)") == FBL_OK);
    CHECK(lua.run("for i = 1, 4000 do local t = table.create(1000, i) end") == FBL_OK);
}

TEST_CASE("after a call meets the memory limit, a call that needs little runs") {
    fbl_state_options_t options = default_options();
    options.memory_limit = int64_t{ 2 } * 1024 * 1024;
    const test_state_t lua(options);
    CHECK(lua.run("local t = {} for i = 1, 1e7 do t[i] = { i } end") == FBL_MEMORY);
    CHECK(lua.run("local t = { 1, 2, 3 } assert(#t == 3)") == FBL_OK);
}
