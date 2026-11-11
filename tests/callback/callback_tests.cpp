#include "fbl.h"
#include "support.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>

namespace {

// The program's functions, as a test's callback has them, by number.
constexpr int64_t add = 1;
constexpr int64_t boom = 2;
constexpr int64_t nested = 3;
constexpr int64_t where = 4;
constexpr int64_t silent = 5;

// What the test's callback has called, for the nesting test.
int depth_reached = 0;

int32_t test_callback(uint64_t state, int32_t frame, int64_t function) {
    switch (function) {
    case add: {
        double a = 0;
        double b = 0;
        if (!fbl_frame_number(state, frame, 0, &a) || !fbl_frame_number(state, frame, 1, &b)) {
            fbl_frame_truncate(state, frame, 0);
            fbl_frame_push_string(state, frame, bytes_of("add takes numbers"), 17);
            return FBL_RUNTIME;
        }
        fbl_frame_truncate(state, frame, 0);
        fbl_frame_push_number(state, frame, a + b);
        return FBL_OK;
    }
    case boom:
        fbl_frame_truncate(state, frame, 0);
        fbl_frame_push_string(state, frame, bytes_of("boom"), 4);
        return FBL_RUNTIME;
    case nested: {
        // Calls back into Luau's `recurse`, one deeper each time, from a frame of its own.
        double n = 0;
        fbl_frame_number(state, frame, 0, &n);
        depth_reached = static_cast<int>(n);
        const int32_t inner = fbl_frame_open(state);
        int32_t status = fbl_frame_get_global(state, inner, bytes_of("recurse"), 7);
        if (status == FBL_OK) {
            fbl_frame_push_number(state, inner, n + 1);
            status = fbl_call_function(state, inner, 0, 1);
        }
        fbl_frame_close(state, inner);
        fbl_frame_truncate(state, frame, 0);
        if (status != FBL_OK) {
            fbl_frame_push_string(state, frame, bytes_of("too deep"), 8);
            return FBL_RUNTIME;
        }
        return FBL_OK;
    }
    case where:
        fbl_frame_truncate(state, frame, 0);
        fbl_callback_where(state, frame);
        return FBL_RUNTIME;
    case silent:
        fbl_frame_truncate(state, frame, 0);
        return FBL_OK;
    default:
        return FBL_OTHER;
    }
}

// Sets the global `name` to the program's function `function`.
void set_function(const test_state_t& lua, std::string_view name, int64_t function) {
    const int32_t frame = fbl_frame_open(lua.handle);
    REQUIRE(fbl_callback_function(lua.handle, frame, function, std::string(name).c_str()) ==
            FBL_OK);
    REQUIRE(fbl_frame_set_global(lua.handle, frame, bytes_of(name),
                                 static_cast<int64_t>(name.size()), 0) == FBL_OK);
    REQUIRE(fbl_frame_close(lua.handle, frame));
}

} // namespace

TEST_CASE("a Null callback is refused") {
    CHECK_FALSE(fbl_callback_set(nullptr));
}

TEST_CASE("Luau calls the program's function, which gives its results") {
    REQUIRE(fbl_callback_set(test_callback));
    const test_state_t lua;
    set_function(lua, "add", add);
    set_function(lua, "silent", silent);
    CHECK(lua.run("assert(add(2, 3) == 5) assert(select('#', silent()) == 0)") == FBL_OK);
}

TEST_CASE("the program's function raises its error in the script, which a pcall catches") {
    REQUIRE(fbl_callback_set(test_callback));
    const test_state_t lua;
    set_function(lua, "boom", boom);
    set_function(lua, "add", add);
    CHECK(lua.run("local ok, err = pcall(boom) assert(not ok and err == 'boom')") == FBL_OK);
    CHECK(lua.run("boom()") == FBL_RUNTIME);
    CHECK(lua.error_message() == "boom");
    CHECK(lua.error_traceback().contains("test:1"));
    CHECK(lua.run("add('x')") == FBL_RUNTIME);
    CHECK(lua.error_message() == "add takes numbers");
}

TEST_CASE("the program's function says where it was called from") {
    REQUIRE(fbl_callback_set(test_callback));
    const test_state_t lua;
    set_function(lua, "where", where);
    CHECK(lua.run("local x = 1\nwhere()", "@mods/a.luau") == FBL_RUNTIME);
    CHECK(lua.error_message() == "mods/a.luau:2: ");
}

TEST_CASE("calls into Luau and back nest, to the limit, and past it are an error") {
    REQUIRE(fbl_callback_set(test_callback));
    const test_state_t lua;
    set_function(lua, "nested", nested);
    REQUIRE(lua.run("function recurse(n) if n >= 10 then return n end return nested(n) end") ==
            FBL_OK);
    CHECK(lua.run("recurse(0)") == FBL_OK);
    CHECK(depth_reached == 9);
    REQUIRE(lua.run("function recurse(n) return nested(n) end") == FBL_OK);
    CHECK(lua.run("recurse(0)") == FBL_RUNTIME);
    CHECK(lua.error_message() == "too deep");
    CHECK(depth_reached < 70);
    // The State goes on.
    CHECK(lua.run("x = 1") == FBL_OK);
}

TEST_CASE("functions Luau collects are given back, each once") {
    REQUIRE(fbl_callback_set(test_callback));
    const test_state_t lua;
    for (int64_t function = 100; function < 150; function++) {
        const int32_t frame = fbl_frame_open(lua.handle);
        REQUIRE(fbl_callback_function(lua.handle, frame, function, "dropped") == FBL_OK);
        REQUIRE(fbl_frame_close(lua.handle, frame));
    }
    set_function(lua, "kept", add);
    REQUIRE(fbl_state_collect(lua.handle) == FBL_OK);
    std::set<int64_t> released;
    std::array<int64_t, 16> taken{};
    while (true) {
        const int32_t count = fbl_callback_released(lua.handle, taken.data(), 16);
        REQUIRE(count >= 0);
        for (int32_t index = 0; index < count; index++) {
            released.insert(taken.at(static_cast<size_t>(index)));
        }
        if (count < 16) {
            break;
        }
    }
    CHECK(released.size() == 50);
    CHECK_FALSE(released.contains(add));
    CHECK(*released.begin() == 100);
    CHECK(fbl_callback_released(lua.handle, taken.data(), 16) == 0);
    CHECK(fbl_callback_released(lua.handle, nullptr, 3) == -1);
}

TEST_CASE("an error's value is kept, whatever it is") {
    const test_state_t lua;
    CHECK(lua.run("error({ code = 5 })") == FBL_RUNTIME);
    int32_t frame = fbl_frame_open(lua.handle);
    REQUIRE(fbl_state_error_value(lua.handle, frame) == FBL_OK);
    CHECK(fbl_frame_type(lua.handle, frame, 0) == FBL_TYPE_TABLE);
    REQUIRE(fbl_frame_close(lua.handle, frame));
    // A syntax error's is its message.
    frame = fbl_frame_open(lua.handle);
    CHECK(fbl_load_source(lua.handle, frame, bytes_of("local = 1"), 9, "=x") == FBL_SYNTAX);
    REQUIRE(fbl_state_error_value(lua.handle, frame) == FBL_OK);
    CHECK(fbl_frame_type(lua.handle, frame, 0) == FBL_TYPE_STRING);
    REQUIRE(fbl_frame_close(lua.handle, frame));
}
