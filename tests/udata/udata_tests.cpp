#include "fbl.h"
#include "state/state.hpp"
#include "support.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>

namespace {

// The test's type's __index, __newindex, and __namecall: each gives its slots' count, so a test
// sees what Luau called it with.
int32_t udata_callback(uint64_t state, int32_t frame, int64_t function) {
    const int32_t count = fbl_frame_count(state, frame);
    if (function == 4) {
        // Makes object `n`'s userdata, which it lets go at once.
        double number = 0;
        fbl_frame_number(state, frame, 0, &number);
        fbl_frame_truncate(state, frame, 0);
        if (fbl_udata_new(state, frame, static_cast<int64_t>(number), 1) != FBL_OK) {
            return FBL_OTHER;
        }
        fbl_frame_truncate(state, frame, 0);
        return FBL_OK;
    }
    if (function == 3) {
        // __namecall: the method's name is first.
        int64_t length = 0;
        const uint8_t* name = fbl_frame_string(state, frame, 0, &length);
        const std::string text = name == nullptr ? "?" : text_of(name, length);
        fbl_frame_truncate(state, frame, 0);
        fbl_frame_push_string(state, frame, bytes_of(text), static_cast<int64_t>(text.size()));
        return FBL_OK;
    }
    fbl_frame_truncate(state, frame, 0);
    fbl_frame_push_number(state, frame, count);
    return FBL_OK;
}

// Registers type 1, "Thing", whose metamethods are udata_callback's functions 1 to 3.
void register_thing(const test_state_t& lua) {
    REQUIRE(fbl_callback_set(udata_callback));
    const int32_t frame = fbl_frame_open(lua.handle);
    REQUIRE(fbl_callback_function(lua.handle, frame, 1, "index") == FBL_OK);
    REQUIRE(fbl_callback_function(lua.handle, frame, 2, "newindex") == FBL_OK);
    REQUIRE(fbl_callback_namecall(lua.handle, frame, 3, "namecall") == FBL_OK);
    REQUIRE(fbl_udata_type(lua.handle, frame, 1, "Thing", 0, 1, 2) == FBL_OK);
    REQUIRE(fbl_frame_close(lua.handle, frame));
}

// Sets the global `name` to object `object`'s userdata, a new one when Luau hasn't one.
void set_object(const test_state_t& lua, std::string_view name, int64_t object) {
    const int32_t frame = fbl_frame_open(lua.handle);
    if (fbl_udata_push(lua.handle, frame, object) != FBL_OK) {
        REQUIRE(fbl_udata_new(lua.handle, frame, object, 1) == FBL_OK);
    }
    REQUIRE(fbl_frame_set_global(lua.handle, frame, bytes_of(name),
                                 static_cast<int64_t>(name.size()), 0) == FBL_OK);
    REQUIRE(fbl_frame_close(lua.handle, frame));
}

std::set<int64_t> released_objects(const test_state_t& lua) {
    std::set<int64_t> released;
    std::array<int64_t, 8> taken{};
    while (true) {
        const int32_t count = fbl_udata_released(lua.handle, taken.data(), 8);
        REQUIRE(count >= 0);
        for (int32_t index = 0; index < count; index++) {
            released.insert(taken.at(static_cast<size_t>(index)));
        }
        if (count < 8) {
            return released;
        }
    }
}

} // namespace

TEST_CASE("an object's metamethods are the type's, and its metatable is locked") {
    const test_state_t lua;
    register_thing(lua);
    set_object(lua, "thing", 7);
    CHECK(lua.run("assert(thing.anything == 2)") == FBL_OK);
    CHECK(lua.run("thing.x = 1") == FBL_OK);
    CHECK(lua.run("assert(thing:greet(1, 2) == 'greet')") == FBL_OK);
    CHECK(lua.run("assert(typeof(thing) == 'Thing')") == FBL_OK);
    CHECK(lua.run("assert(getmetatable(thing) == 'The metatable is locked')") == FBL_OK);
    CHECK(lua.run("setmetatable(thing, {})") == FBL_RUNTIME);
    CHECK(lua.run("rawset(thing, 'x', 1)") == FBL_RUNTIME);
}

TEST_CASE("one object is one userdata while Luau has it, and its number comes back after") {
    const test_state_t lua;
    register_thing(lua);
    set_object(lua, "a", 1);
    set_object(lua, "b", 1);
    set_object(lua, "c", 2);
    CHECK(lua.run("assert(a == b and a ~= c)") == FBL_OK);
    int32_t frame = fbl_frame_open(lua.handle);
    REQUIRE(fbl_frame_get_global(lua.handle, frame, bytes_of("c"), 1) == FBL_OK);
    int64_t object = 0;
    int32_t type = 0;
    CHECK(fbl_udata_object(lua.handle, frame, 0, &object, &type));
    CHECK(object == 2);
    CHECK(type == 1);
    REQUIRE(fbl_frame_close(lua.handle, frame));
    CHECK(lua.run("a = nil b = nil") == FBL_OK);
    REQUIRE(fbl_state_collect(lua.handle) == FBL_OK);
    CHECK(released_objects(lua) == std::set<int64_t>{ 1 });
    frame = fbl_frame_open(lua.handle);
    CHECK(fbl_udata_push(lua.handle, frame, 1) == FBL_OTHER);
    CHECK(fbl_udata_push(lua.handle, frame, 2) == FBL_OK);
    REQUIRE(fbl_frame_close(lua.handle, frame));
}

TEST_CASE("what isn't an object of the program's is refused") {
    const test_state_t lua;
    register_thing(lua);
    const int32_t frame = fbl_frame_open(lua.handle);
    REQUIRE(fbl_frame_push_number(lua.handle, frame, 1) == FBL_OK);
    int64_t object = 0;
    int32_t type = 0;
    CHECK_FALSE(fbl_udata_object(lua.handle, frame, 0, &object, &type));
    REQUIRE(fbl_frame_close(lua.handle, frame));
    CHECK(lua.run("proxy = newproxy()") == FBL_OK);
    const int32_t other = fbl_frame_open(lua.handle);
    REQUIRE(fbl_frame_get_global(lua.handle, other, bytes_of("proxy"), 5) == FBL_OK);
    CHECK_FALSE(fbl_udata_object(lua.handle, other, 0, &object, &type));
    CHECK(fbl_udata_new(lua.handle, other, 9, 2) == FBL_MISUSE);
    REQUIRE(fbl_frame_close(lua.handle, other));
}

TEST_CASE("a type is registered once, and its own fields stay its own") {
    const test_state_t lua;
    register_thing(lua);
    const int32_t frame = fbl_frame_open(lua.handle);
    REQUIRE(fbl_callback_function(lua.handle, frame, 1, "f") == FBL_OK);
    CHECK(fbl_udata_type(lua.handle, frame, 1, "Again", 0, 0, 0) == FBL_MISUSE);
    CHECK(fbl_udata_type(lua.handle, frame, 0, "Zero", 0, 0, 0) == FBL_MISUSE);
    CHECK(fbl_udata_type_set(lua.handle, frame, 1, "__index", 0) == FBL_MISUSE);
    CHECK(fbl_udata_type_set(lua.handle, frame, 1, "__metatable", 0) == FBL_MISUSE);
    CHECK(fbl_udata_type_set(lua.handle, frame, 5, "__add", 0) == FBL_MISUSE);
    CHECK(fbl_udata_type_set(lua.handle, frame, 1, "__len", 0) == FBL_OK);
    REQUIRE(fbl_frame_close(lua.handle, frame));
    set_object(lua, "thing", 3);
    // __len is the callback's function 1, which gives its count of arguments.
    CHECK(lua.run("assert(#thing >= 1)") == FBL_OK);
}

TEST_CASE("every object made and collected inside one call is given back") {
    const test_state_t lua;
    register_thing(lua);
    const int32_t frame = fbl_frame_open(lua.handle);
    REQUIRE(fbl_callback_function(lua.handle, frame, 4, "make") == FBL_OK);
    REQUIRE(fbl_frame_set_global(lua.handle, frame, bytes_of("make"), 4, 0) == FBL_OK);
    REQUIRE(fbl_frame_close(lua.handle, frame));
    const fbl::state_t* state = fbl::find_state(lua.handle);
    REQUIRE(state != nullptr);
    CHECK(lua.run("for i = 1, 100000 do make(i) end") == FBL_OK);
    REQUIRE(fbl_state_collect(lua.handle) == FBL_OK);
    CHECK(state->live_objects == 0);
    CHECK(released_objects(lua).size() == 100000);
}

TEST_CASE("an object whose making fails at any allocation is nothing to the program") {
    const test_state_t lua;
    register_thing(lua);
    fbl::state_t* state = fbl::find_state(lua.handle);
    REQUIRE(state != nullptr);
    int64_t made = 0;
    int64_t failed = 0;
    int64_t number = 0;
    const int32_t frame = fbl_frame_open(lua.handle);
    for (int round = 0; round < 10; round++) {
        for (int64_t failing = 0; failing < 8; failing++) {
            // At each of its making's points, and each of the allocations it asks for.
            state->failing_point = failing % 4;
            state->failing_allocation = failing;
            const int32_t status = fbl_udata_new(lua.handle, frame, ++number, 1);
            state->failing_allocation = -1;
            state->failing_point = -1;
            if (status == FBL_OK) {
                made++;
            } else {
                CHECK((status == FBL_MEMORY || status == FBL_RUNTIME));
                failed++;
            }
            fbl_frame_truncate(lua.handle, frame, 0);
        }
    }
    CHECK(fbl_frame_close(lua.handle, frame));
    CHECK(failed > 0);
    REQUIRE(fbl_state_collect(lua.handle) == FBL_OK);
    CHECK(state->live_objects == 0);
    CHECK(released_objects(lua).size() == static_cast<size_t>(made));
    CHECK(state->released_objects.empty());
    set_object(lua, "late", 1000);
    CHECK(lua.run("assert(late.x == 2)") == FBL_OK);
}

TEST_CASE("an object's number past what Luau's numbers tell apart is refused") {
    const test_state_t lua;
    register_thing(lua);
    const int32_t frame = fbl_frame_open(lua.handle);
    constexpr int64_t most = 9007199254740992;
    CHECK(fbl_udata_new(lua.handle, frame, most + 1, 1) == FBL_MISUSE);
    CHECK(fbl_udata_push(lua.handle, frame, -most - 1) == FBL_MISUSE);
    CHECK(fbl_udata_new(lua.handle, frame, most, 1) == FBL_OK);
    CHECK(fbl_frame_count(lua.handle, frame) == 1);
    CHECK(fbl_frame_close(lua.handle, frame));
}

TEST_CASE("a type's metatable is frozen again when a set of it fails") {
    const test_state_t lua;
    register_thing(lua);
    fbl::state_t* state = fbl::find_state(lua.handle);
    REQUIRE(state != nullptr);
    const int32_t frame = fbl_frame_open(lua.handle);
    REQUIRE(fbl_frame_push_number(lua.handle, frame, 1) == FBL_OK);
    state->failing_point = 0;
    CHECK(fbl_udata_type_set(lua.handle, frame, 1, "__add", 0) != FBL_OK);
    state->failing_point = -1;
    CHECK(fbl_frame_close(lua.handle, frame));
    lua_getref(state->main, state->type_refs.at(0));
    CHECK(lua_getreadonly(state->main, -1) != 0);
    lua_pop(state->main, 1);
}
