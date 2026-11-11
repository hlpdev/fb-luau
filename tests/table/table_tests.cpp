#include "fbl.h"
#include "support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>

namespace {

// A frame with a script's global table `name` in slot 0.
int32_t with_global(const test_state_t& lua, std::string_view name) {
    const int32_t frame = fbl_frame_open(lua.handle);
    REQUIRE(frame >= 0);
    REQUIRE(fbl_frame_get_global(lua.handle, frame, bytes_of(name),
                                 static_cast<int64_t>(name.size())) == FBL_OK);
    return frame;
}

std::string string_at(const test_state_t& lua, int32_t frame, int32_t slot) {
    int64_t length = 0;
    const uint8_t* bytes = fbl_frame_string(lua.handle, frame, slot, &length);
    REQUIRE(bytes != nullptr);
    return text_of(bytes, length);
}

double number_at(const test_state_t& lua, int32_t frame, int32_t slot) {
    double value = 0;
    REQUIRE(fbl_frame_number(lua.handle, frame, slot, &value));
    return value;
}

} // namespace

TEST_CASE("a table is made, written, and read back") {
    const test_state_t lua;
    const int32_t frame = fbl_frame_open(lua.handle);
    REQUIRE(fbl_table_new(lua.handle, frame, 4, 4) == FBL_OK);
    REQUIRE(fbl_frame_push_string(lua.handle, frame, bytes_of("name"), 4) == FBL_OK);
    REQUIRE(fbl_frame_push_string(lua.handle, frame, bytes_of("ada"), 3) == FBL_OK);
    REQUIRE(fbl_table_set(lua.handle, frame, 0, 1, 2) == FBL_OK);
    REQUIRE(fbl_frame_truncate(lua.handle, frame, 2));
    REQUIRE(fbl_table_get(lua.handle, frame, 0, 1) == FBL_OK);
    CHECK(string_at(lua, frame, 2) == "ada");
    REQUIRE(fbl_frame_push_number(lua.handle, frame, 1) == FBL_OK);
    REQUIRE(fbl_frame_push_number(lua.handle, frame, 10) == FBL_OK);
    REQUIRE(fbl_table_raw_set(lua.handle, frame, 0, 3, 4) == FBL_OK);
    CHECK(fbl_table_length(lua.handle, frame, 0) == 1);
    REQUIRE(fbl_table_raw_get_index(lua.handle, frame, 0, 1) == FBL_OK);
    CHECK(number_at(lua, frame, 5) == 10);
    CHECK(fbl_frame_close(lua.handle, frame));
}

TEST_CASE("get and set go through the metatable, and the raw ones don't") {
    const test_state_t lua;
    REQUIRE(lua.run("t = setmetatable({}, { __index = function(_, k) return k .. '!' end, "
                    "__newindex = function(t, k, v) rawset(t, k, v * 2) end })") == FBL_OK);
    const int32_t frame = with_global(lua, "t");
    REQUIRE(fbl_frame_push_string(lua.handle, frame, bytes_of("x"), 1) == FBL_OK);
    REQUIRE(fbl_table_get(lua.handle, frame, 0, 1) == FBL_OK);
    CHECK(string_at(lua, frame, 2) == "x!");
    REQUIRE(fbl_table_raw_get(lua.handle, frame, 0, 1) == FBL_OK);
    CHECK(fbl_frame_type(lua.handle, frame, 3) == FBL_TYPE_NIL);
    REQUIRE(fbl_frame_push_number(lua.handle, frame, 5) == FBL_OK);
    REQUIRE(fbl_table_set(lua.handle, frame, 0, 1, 4) == FBL_OK);
    REQUIRE(fbl_table_raw_get(lua.handle, frame, 0, 1) == FBL_OK);
    CHECK(number_at(lua, frame, 5) == 10);
    CHECK(fbl_frame_close(lua.handle, frame));
}

TEST_CASE("a nil or NaN key, and a frozen table, are errors") {
    const test_state_t lua;
    const int32_t frame = fbl_frame_open(lua.handle);
    REQUIRE(fbl_table_new(lua.handle, frame, 0, 0) == FBL_OK);
    REQUIRE(fbl_frame_push_nil(lua.handle, frame) == FBL_OK);
    REQUIRE(fbl_frame_push_number(lua.handle, frame, 1) == FBL_OK);
    CHECK(fbl_table_set(lua.handle, frame, 0, 1, 2) == FBL_RUNTIME);
    CHECK(fbl_table_raw_set(lua.handle, frame, 0, 1, 2) == FBL_RUNTIME);
    CHECK(lua.error_message().contains("nil"));
    REQUIRE(fbl_frame_push_number(lua.handle, frame, 0.0 / 0.0) == FBL_OK);
    CHECK(fbl_table_raw_set(lua.handle, frame, 0, 3, 2) == FBL_RUNTIME);
    CHECK(fbl_table_set(lua.handle, frame, 0, 3, 2) == FBL_RUNTIME);
    CHECK(fbl_table_is_frozen(lua.handle, frame, 0) == 0);
    REQUIRE(fbl_table_freeze(lua.handle, frame, 0) == FBL_OK);
    CHECK(fbl_table_is_frozen(lua.handle, frame, 0) == 1);
    CHECK(fbl_table_raw_set(lua.handle, frame, 0, 2, 2) == FBL_RUNTIME);
    CHECK(lua.error_message().contains("readonly"));
    CHECK(fbl_table_set(lua.handle, frame, 0, 2, 2) == FBL_RUNTIME);
    CHECK(fbl_table_set_metatable(lua.handle, frame, 0, 1) == FBL_RUNTIME);
    CHECK(fbl_frame_count(lua.handle, frame) == 4);
    CHECK(fbl_frame_close(lua.handle, frame));
}

TEST_CASE("every entry is gone over once") {
    const test_state_t lua;
    REQUIRE(lua.run("t = { 10, 20, 30, a = 'x', b = 'y' }") == FBL_OK);
    const int32_t frame = with_global(lua, "t");
    std::set<std::string> seen;
    int32_t iterator = 0;
    while (true) {
        iterator = fbl_table_next(lua.handle, frame, 0, iterator);
        REQUIRE(iterator >= -1);
        if (iterator == -1) {
            break;
        }
        REQUIRE(fbl_frame_count(lua.handle, frame) == 3);
        if (fbl_frame_type(lua.handle, frame, 1) == FBL_TYPE_STRING) {
            seen.insert(string_at(lua, frame, 1) + "=" + string_at(lua, frame, 2));
        } else {
            seen.insert(std::to_string(static_cast<int>(number_at(lua, frame, 1))) + "=" +
                        std::to_string(static_cast<int>(number_at(lua, frame, 2))));
        }
        REQUIRE(fbl_frame_truncate(lua.handle, frame, 1));
    }
    CHECK(seen == std::set<std::string>{ "1=10", "2=20", "3=30", "a=x", "b=y" });
    CHECK(fbl_table_next(lua.handle, frame, 0, -5) == -2);
    CHECK(fbl_frame_close(lua.handle, frame));
}

TEST_CASE("a metatable is read and set, and taken away with nil") {
    const test_state_t lua;
    const int32_t frame = fbl_frame_open(lua.handle);
    REQUIRE(fbl_table_new(lua.handle, frame, 0, 0) == FBL_OK);
    REQUIRE(fbl_table_get_metatable(lua.handle, frame, 0) == FBL_OK);
    CHECK(fbl_frame_type(lua.handle, frame, 1) == FBL_TYPE_NIL);
    REQUIRE(fbl_table_new(lua.handle, frame, 0, 0) == FBL_OK);
    REQUIRE(fbl_table_set_metatable(lua.handle, frame, 0, 2) == FBL_OK);
    REQUIRE(fbl_table_get_metatable(lua.handle, frame, 0) == FBL_OK);
    CHECK(fbl_frame_type(lua.handle, frame, 3) == FBL_TYPE_TABLE);
    CHECK(fbl_table_set_metatable(lua.handle, frame, 0, 1) == FBL_OK);
    REQUIRE(fbl_frame_push_number(lua.handle, frame, 1) == FBL_OK);
    CHECK(fbl_table_set_metatable(lua.handle, frame, 0, 4) == FBL_MISUSE);
    CHECK(fbl_frame_close(lua.handle, frame));
}

TEST_CASE("a slot that isn't a table is refused") {
    const test_state_t lua;
    const int32_t frame = fbl_frame_open(lua.handle);
    REQUIRE(fbl_frame_push_number(lua.handle, frame, 1) == FBL_OK);
    CHECK(fbl_table_get(lua.handle, frame, 0, 0) == FBL_MISUSE);
    CHECK(std::string_view(fbl_core_error()).contains("isn't a table"));
    CHECK(fbl_table_length(lua.handle, frame, 0) == -1);
    CHECK(fbl_table_is_frozen(lua.handle, frame, 0) == -1);
    CHECK(fbl_table_freeze(lua.handle, frame, 0) == FBL_MISUSE);
    CHECK(fbl_table_new(lua.handle, frame, -1, 0) == FBL_MISUSE);
    CHECK(fbl_frame_close(lua.handle, frame));
}

TEST_CASE("integers cross as Luau's own, apart from numbers") {
    const test_state_t lua;
    const int32_t frame = fbl_frame_open(lua.handle);
    REQUIRE(fbl_frame_push_integer(lua.handle, frame, 9007199254740993) == FBL_OK);
    CHECK(fbl_frame_type(lua.handle, frame, 0) == FBL_TYPE_INTEGER);
    int64_t value = 0;
    CHECK(fbl_frame_integer(lua.handle, frame, 0, &value));
    CHECK(value == 9007199254740993);
    double number = 0;
    CHECK_FALSE(fbl_frame_number(lua.handle, frame, 0, &number));
    REQUIRE(fbl_frame_set_global(lua.handle, frame, bytes_of("big"), 3, 0) == FBL_OK);
    CHECK(fbl_frame_close(lua.handle, frame));
    CHECK(lua.run("assert(typeof(big) == 'integer') assert(big ~= 9007199254740993)") == FBL_OK);
}
