#include "fbl.h"
#include "support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <string_view>

TEST_CASE("each kind of value a frame takes reads back as it went in") {
    const test_state_t lua;
    const int32_t frame = fbl_frame_open(lua.handle);
    REQUIRE(frame >= 0);
    const std::string_view text("with a \0 inside", 15);
    REQUIRE(fbl_frame_push_nil(lua.handle, frame) == FBL_OK);
    REQUIRE(fbl_frame_push_boolean(lua.handle, frame, true) == FBL_OK);
    REQUIRE(fbl_frame_push_number(lua.handle, frame, 2.5) == FBL_OK);
    REQUIRE(fbl_frame_push_string(lua.handle, frame, bytes_of(text),
                                  static_cast<int64_t>(text.size())) == FBL_OK);
    REQUIRE(fbl_frame_push_string(lua.handle, frame, nullptr, 0) == FBL_OK);
    REQUIRE(fbl_frame_push_copy(lua.handle, frame, 2) == FBL_OK);
    CHECK(fbl_frame_count(lua.handle, frame) == 6);
    CHECK(fbl_frame_type(lua.handle, frame, 0) == FBL_TYPE_NIL);
    CHECK(fbl_frame_type(lua.handle, frame, 1) == FBL_TYPE_BOOLEAN);
    CHECK(fbl_frame_type(lua.handle, frame, 2) == FBL_TYPE_NUMBER);
    CHECK(fbl_frame_type(lua.handle, frame, 3) == FBL_TYPE_STRING);
    bool flag = false;
    CHECK(fbl_frame_boolean(lua.handle, frame, 1, &flag));
    CHECK(flag);
    double number = 0;
    CHECK(fbl_frame_number(lua.handle, frame, 5, &number));
    CHECK(number == 2.5);
    int64_t length = 0;
    const uint8_t* bytes = fbl_frame_string(lua.handle, frame, 3, &length);
    REQUIRE(bytes != nullptr);
    CHECK(text_of(bytes, length) == text);
    bytes = fbl_frame_string(lua.handle, frame, 4, &length);
    REQUIRE(bytes != nullptr);
    CHECK(length == 0);
    CHECK(fbl_frame_close(lua.handle, frame));
}

TEST_CASE("a value read as another type, or a slot past the frame, is refused") {
    const test_state_t lua;
    const int32_t frame = fbl_frame_open(lua.handle);
    REQUIRE(fbl_frame_push_number(lua.handle, frame, 1) == FBL_OK);
    bool flag = false;
    CHECK_FALSE(fbl_frame_boolean(lua.handle, frame, 0, &flag));
    int64_t length = 0;
    // A number isn't made a string, which would need memory and change the slot.
    CHECK(fbl_frame_string(lua.handle, frame, 0, &length) == nullptr);
    CHECK(fbl_frame_type(lua.handle, frame, 0) == FBL_TYPE_NUMBER);
    CHECK(fbl_frame_type(lua.handle, frame, 1) == -1);
    CHECK(fbl_frame_type(lua.handle, frame, -1) == -1);
    CHECK(std::string_view(fbl_core_error()).contains("has no slot -1"));
    CHECK(fbl_frame_push_string(lua.handle, frame, nullptr, 3) == FBL_MISUSE);
    CHECK(fbl_frame_push_string(lua.handle, frame, bytes_of("x"), -1) == FBL_MISUSE);
    CHECK_FALSE(fbl_frame_truncate(lua.handle, frame, 2));
    CHECK(fbl_frame_close(lua.handle, frame));
}

TEST_CASE("only the newest frame is used, and closing takes its values away") {
    const test_state_t lua;
    const int32_t outer = fbl_frame_open(lua.handle);
    REQUIRE(fbl_frame_push_number(lua.handle, outer, 1) == FBL_OK);
    const int32_t inner = fbl_frame_open(lua.handle);
    REQUIRE(inner == outer + 1);
    CHECK(fbl_frame_count(lua.handle, inner) == 0);
    CHECK(fbl_frame_push_nil(lua.handle, outer) == FBL_MISUSE);
    CHECK(std::string_view(fbl_core_error()).contains("isn't the State's newest"));
    CHECK_FALSE(fbl_frame_close(lua.handle, outer));
    REQUIRE(fbl_frame_push_nil(lua.handle, inner) == FBL_OK);
    CHECK(fbl_frame_close(lua.handle, inner));
    CHECK(fbl_frame_count(lua.handle, outer) == 1);
    CHECK(fbl_frame_close(lua.handle, outer));
    CHECK_FALSE(fbl_frame_close(lua.handle, outer));
}

TEST_CASE("globals are read and set as a script does") {
    const test_state_t lua;
    REQUIRE(lua.run("speed = 4.5 setmetatable(_G, { __index = function(_, key) return key .. '!' "
                    "end })") == FBL_OK);
    const int32_t frame = fbl_frame_open(lua.handle);
    REQUIRE(fbl_frame_get_global(lua.handle, frame, bytes_of("speed"), 5) == FBL_OK);
    double speed = 0;
    CHECK(fbl_frame_number(lua.handle, frame, 0, &speed));
    CHECK(speed == 4.5);
    // Through its metatable.
    REQUIRE(fbl_frame_get_global(lua.handle, frame, bytes_of("missing"), 7) == FBL_OK);
    int64_t length = 0;
    const uint8_t* bytes = fbl_frame_string(lua.handle, frame, 1, &length);
    REQUIRE(bytes != nullptr);
    CHECK(text_of(bytes, length) == "missing!");
    REQUIRE(fbl_frame_push_number(lua.handle, frame, 7) == FBL_OK);
    REQUIRE(fbl_frame_set_global(lua.handle, frame, bytes_of("lives"), 5, 2) == FBL_OK);
    CHECK(fbl_frame_count(lua.handle, frame) == 3);
    CHECK(fbl_frame_close(lua.handle, frame));
    CHECK(lua.run("assert(lives == 7)") == FBL_OK);
}

TEST_CASE("a global whose metatable raises gives the error") {
    const test_state_t lua;
    REQUIRE(lua.run("setmetatable(_G, { __index = function() error('no such global') end, "
                    "__newindex = function() error('read-only') end })") == FBL_OK);
    const int32_t frame = fbl_frame_open(lua.handle);
    CHECK(fbl_frame_get_global(lua.handle, frame, bytes_of("x"), 1) == FBL_RUNTIME);
    CHECK(lua.error_message().contains("no such global"));
    REQUIRE(fbl_frame_push_number(lua.handle, frame, 1) == FBL_OK);
    CHECK(fbl_frame_set_global(lua.handle, frame, bytes_of("x"), 1, 0) == FBL_RUNTIME);
    CHECK(lua.error_message().contains("read-only"));
    CHECK(fbl_frame_count(lua.handle, frame) == 1);
    CHECK(fbl_frame_close(lua.handle, frame));
}

TEST_CASE("a string past the memory limit isn't made, and the frame is as it was") {
    fbl_state_options_t options = default_options();
    options.memory_limit = int64_t{ 512 } * 1024;
    const test_state_t lua(options);
    const int32_t frame = fbl_frame_open(lua.handle);
    const std::string big(int64_t{ 1024 } * 1024, 'x');
    CHECK(fbl_frame_push_string(lua.handle, frame, bytes_of(big),
                                static_cast<int64_t>(big.size())) == FBL_MEMORY);
    CHECK(fbl_frame_count(lua.handle, frame) == 0);
    CHECK(fbl_frame_push_string(lua.handle, frame, bytes_of("small"), 5) == FBL_OK);
    CHECK(fbl_frame_close(lua.handle, frame));
}

TEST_CASE("references keep values past their frame, and a freed one is refused") {
    const test_state_t lua;
    int32_t frame = fbl_frame_open(lua.handle);
    REQUIRE(fbl_frame_push_string(lua.handle, frame, bytes_of("kept"), 4) == FBL_OK);
    REQUIRE(fbl_frame_push_nil(lua.handle, frame) == FBL_OK);
    int32_t ref = 0;
    REQUIRE(fbl_ref_new(lua.handle, frame, 0, &ref) == FBL_OK);
    CHECK(ref > 0);
    int32_t none = 0;
    CHECK(fbl_ref_new(lua.handle, frame, 1, &none) == FBL_MISUSE);
    CHECK(fbl_ref_new(lua.handle, frame, 0, nullptr) == FBL_MISUSE);
    CHECK(fbl_frame_close(lua.handle, frame));
    CHECK(fbl_state_collect(lua.handle) == FBL_OK);

    frame = fbl_frame_open(lua.handle);
    REQUIRE(fbl_ref_push(lua.handle, frame, ref) == FBL_OK);
    int64_t length = 0;
    const uint8_t* bytes = fbl_frame_string(lua.handle, frame, 0, &length);
    REQUIRE(bytes != nullptr);
    CHECK(text_of(bytes, length) == "kept");
    CHECK(fbl_ref_free(lua.handle, ref));
    CHECK_FALSE(fbl_ref_free(lua.handle, ref));
    CHECK(fbl_ref_push(lua.handle, frame, ref) == FBL_MISUSE);
    CHECK(fbl_ref_push(lua.handle, frame, 12345) == FBL_MISUSE);
    CHECK(fbl_frame_close(lua.handle, frame));
}
