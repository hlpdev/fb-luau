#include "fbl.h"
#include "support.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string_view>

TEST_CASE("a buffer is made, written, and read within its bounds") {
    const test_state_t lua;
    const int32_t frame = fbl_frame_open(lua.handle);
    REQUIRE(fbl_buffer_new(lua.handle, frame, 8) == FBL_OK);
    CHECK(fbl_frame_type(lua.handle, frame, 0) == FBL_TYPE_BUFFER);
    CHECK(fbl_buffer_length(lua.handle, frame, 0) == 8);
    const std::array<uint8_t, 3> written{ 1, 2, 3 };
    CHECK(fbl_buffer_write(lua.handle, frame, 0, 5, written.data(), 3));
    std::array<uint8_t, 4> read{};
    CHECK(fbl_buffer_read(lua.handle, frame, 0, 4, read.data(), 4));
    CHECK(read == std::array<uint8_t, 4>{ 0, 1, 2, 3 });
    // Nothing past its end, or before its start, either way.
    CHECK_FALSE(fbl_buffer_write(lua.handle, frame, 0, 6, written.data(), 3));
    CHECK_FALSE(fbl_buffer_read(lua.handle, frame, 0, 5, read.data(), 4));
    CHECK_FALSE(fbl_buffer_read(lua.handle, frame, 0, -1, read.data(), 1));
    CHECK_FALSE(fbl_buffer_read(lua.handle, frame, 0, 0, read.data(), -1));
    CHECK_FALSE(fbl_buffer_read(lua.handle, frame, 0, INT64_MAX, read.data(), 2));
    CHECK(fbl_buffer_read(lua.handle, frame, 0, 8, read.data(), 0));
    CHECK(fbl_buffer_read(lua.handle, frame, 0, 0, nullptr, 0));
    CHECK_FALSE(fbl_buffer_read(lua.handle, frame, 0, 0, nullptr, 1));
    CHECK(fbl_frame_close(lua.handle, frame));
}

TEST_CASE("a buffer's size is from 0 to 1 GiB, and what isn't one is refused") {
    const test_state_t lua;
    const int32_t frame = fbl_frame_open(lua.handle);
    CHECK(fbl_buffer_new(lua.handle, frame, -1) == FBL_MISUSE);
    CHECK(fbl_buffer_new(lua.handle, frame, (int64_t{ 1024 } * 1024 * 1024) + 1) == FBL_MISUSE);
    REQUIRE(fbl_buffer_new(lua.handle, frame, 0) == FBL_OK);
    CHECK(fbl_buffer_length(lua.handle, frame, 0) == 0);
    REQUIRE(fbl_frame_push_number(lua.handle, frame, 1) == FBL_OK);
    CHECK(fbl_buffer_length(lua.handle, frame, 1) == -1);
    CHECK(std::string_view(fbl_core_error()).contains("isn't a buffer"));
    CHECK(fbl_frame_close(lua.handle, frame));
}

TEST_CASE("a buffer past the memory limit isn't made") {
    fbl_state_options_t options = default_options();
    options.memory_limit = int64_t{ 1024 } * 1024;
    const test_state_t lua(options);
    const int32_t frame = fbl_frame_open(lua.handle);
    CHECK(fbl_buffer_new(lua.handle, frame, int64_t{ 4 } * 1024 * 1024) == FBL_MEMORY);
    CHECK(fbl_frame_count(lua.handle, frame) == 0);
    CHECK(fbl_frame_close(lua.handle, frame));
}

TEST_CASE("a vector crosses as its 3 floats") {
    const test_state_t lua;
    const int32_t frame = fbl_frame_open(lua.handle);
    const fbl_vector_t up{ .x = 0, .y = 1.5F, .z = -2 };
    REQUIRE(fbl_frame_push_vector(lua.handle, frame, &up) == FBL_OK);
    CHECK(fbl_frame_type(lua.handle, frame, 0) == FBL_TYPE_VECTOR);
    fbl_vector_t read{};
    CHECK(fbl_frame_vector(lua.handle, frame, 0, &read));
    CHECK(read.y == 1.5F);
    CHECK(read.z == -2);
    REQUIRE(fbl_frame_push_number(lua.handle, frame, 1) == FBL_OK);
    CHECK_FALSE(fbl_frame_vector(lua.handle, frame, 1, &read));
    CHECK(fbl_frame_push_vector(lua.handle, frame, nullptr) == FBL_MISUSE);
    CHECK(fbl_frame_close(lua.handle, frame));
}
