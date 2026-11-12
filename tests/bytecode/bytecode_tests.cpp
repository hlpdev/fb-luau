#include "fbl.h"
#include "support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

// `source` compiled with the default options, as bytes the test owns.
std::vector<uint8_t> compiled(std::string_view source) {
    int64_t size = 0;
    int32_t status = FBL_OTHER;
    uint8_t* made = fbl_bytecode_compile(bytes_of(source), static_cast<int64_t>(source.size()), 1,
                                         1, 0, &size, &status);
    REQUIRE(status == FBL_OK);
    REQUIRE(made != nullptr);
    std::vector<uint8_t> bytes(made, made + size);
    fbl_bytecode_free(made);
    return bytes;
}

// Loads `bytecode` into a frame and calls it, giving its first result as a number in `result`:
// the status.
int32_t run(const test_state_t& lua, const std::vector<uint8_t>& bytecode, double& result,
            int32_t environment = -1) {
    const int32_t frame = fbl_frame_open(lua.handle);
    int32_t status = FBL_OK;
    if (environment >= 0) {
        REQUIRE(fbl_sandbox_environment(lua.handle, frame) == FBL_OK);
    }
    status = fbl_bytecode_load(lua.handle, frame, bytecode.data(),
                               static_cast<int64_t>(bytecode.size()), "=compiled", environment);
    if (status == FBL_OK) {
        status = fbl_call_function(lua.handle, frame, fbl_frame_count(lua.handle, frame) - 1, 1);
    }
    if (status == FBL_OK) {
        fbl_frame_number(lua.handle, frame, fbl_frame_count(lua.handle, frame) - 1, &result);
    }
    REQUIRE(fbl_frame_close(lua.handle, frame));
    return status;
}

} // namespace

// First, as it's the one made before any State in the process, which Catch runs in order.
TEST_CASE("source compiled before any State is compiled as after one") {
    const std::string_view source =
        "local t = { 1, 2, 3 }\n"
        "local function sum(...) local n = 0 for _, v in ... do n += v end "
        "return n end\n"
        "return sum(ipairs(t)) + #`{t[1]}` + vector.create(1, 2, 3).x\n";
    const std::vector<uint8_t> before = compiled(source);
    const test_state_t lua;
    CHECK(compiled(source) == before);
}

TEST_CASE("source compiled ahead loads and runs in a State") {
    const test_state_t lua;
    const std::vector<uint8_t> bytecode =
        compiled("local n = 0 for i = 1, 10 do n += i end return n");
    double result = 0;
    CHECK(run(lua, bytecode, result) == FBL_OK);
    CHECK(result == 55);
    // Twice, as it's the program's own.
    CHECK(run(lua, bytecode, result) == FBL_OK);
}

TEST_CASE("compiled code loads into a script's environment") {
    const test_state_t lua;
    REQUIRE(lua.run("shared = 2") == FBL_OK);
    const std::vector<uint8_t> bytecode = compiled("mine = 3 return shared + mine");
    double result = 0;
    CHECK(run(lua, bytecode, result, 0) == FBL_OK);
    CHECK(result == 5);
    CHECK(lua.run("assert(mine == nil)") == FBL_OK);
}

TEST_CASE("source that doesn't compile is a syntax error, said as the thread's last error") {
    int64_t size = 0;
    int32_t status = FBL_OK;
    CHECK(fbl_bytecode_compile(bytes_of("return +"), 8, 1, 1, 0, &size, &status) == nullptr);
    CHECK(status == FBL_SYNTAX);
    CHECK(std::string(fbl_core_error()).contains(":1:"));
    CHECK(fbl_bytecode_compile(bytes_of("x"), 1, 3, 1, 0, &size, &status) == nullptr);
    CHECK(status == FBL_MISUSE);
    CHECK(fbl_bytecode_compile(bytes_of("x"), -1, 1, 1, 0, &size, &status) == nullptr);
    CHECK(fbl_bytecode_compile(bytes_of("x"), 1, 1, 1, 0, nullptr, &status) == nullptr);
    fbl_bytecode_free(nullptr);
}

TEST_CASE("bytecode that isn't a compile's, or of another version, isn't loaded") {
    const test_state_t lua;
    const int32_t frame = fbl_frame_open(lua.handle);
    const std::vector<uint8_t> error = { 0, 'b', 'a', 'd' };
    CHECK(fbl_bytecode_load(lua.handle, frame, error.data(), 4, "=x", -1) == FBL_MISUSE);
    CHECK(fbl_bytecode_load(lua.handle, frame, nullptr, 0, "=x", -1) == FBL_MISUSE);
    std::vector<uint8_t> bytecode = compiled("return 1");
    CHECK(fbl_bytecode_load(lua.handle, frame, bytecode.data(),
                            static_cast<int64_t>(bytecode.size()), nullptr, -1) == FBL_MISUSE);
    CHECK(fbl_bytecode_load(lua.handle, frame, bytecode.data(),
                            static_cast<int64_t>(bytecode.size()), "=x", 3) == FBL_MISUSE);
    bytecode[0] = 99;
    CHECK(fbl_bytecode_load(lua.handle, frame, bytecode.data(),
                            static_cast<int64_t>(bytecode.size()), "=x", -1) == FBL_RUNTIME);
    CHECK(lua.error_message().contains("version"));
    CHECK(fbl_frame_count(lua.handle, frame) == 0);
    CHECK(fbl_frame_close(lua.handle, frame));
}
