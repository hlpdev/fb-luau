#include "fbl.h"
#include "support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

// Code with loops and arithmetic the code generator compiles, which gives the same either way.
constexpr std::string_view work = "local function sum(n) local s = 0 for i = 1, n do s += i * 2 "
                                  "end return s end result = sum(1000)";

// The State's options with `native` code.
fbl_state_options_t with(int32_t native) {
    fbl_state_options_t options = default_options();
    options.native_code = native;
    return options;
}

} // namespace

TEST_CASE("everything is compiled to native code where it's supported, with the same results") {
    const test_state_t native(with(FBL_NATIVE_ALL));
    const test_state_t interpreted(with(FBL_NATIVE_OFF));
    REQUIRE(native.run(work) == FBL_OK);
    REQUIRE(interpreted.run(work) == FBL_OK);
    CHECK(native.run("assert(result == 1001000)") == FBL_OK);
    CHECK(interpreted.run("assert(result == 1001000)") == FBL_OK);
    CHECK(fbl_state_native_functions(interpreted.handle) == 0);
    if (fbl_state_native_supported() == 1) {
        CHECK(fbl_state_native_functions(native.handle) > 0);
    } else {
        CHECK(fbl_state_native_functions(native.handle) == 0);
    }
}

TEST_CASE("marked, only chunks and functions marked native are compiled") {
    const test_state_t lua(with(FBL_NATIVE_MARKED));
    REQUIRE(lua.run(work) == FBL_OK);
    CHECK(fbl_state_native_functions(lua.handle) == 0);
    REQUIRE(lua.run("--!native\n" + std::string(work)) == FBL_OK);
    const int64_t module = fbl_state_native_functions(lua.handle);
    REQUIRE(lua.run("@native local function f(n) return n * 2 end result = f(21)") == FBL_OK);
    CHECK(lua.run("assert(result == 42)") == FBL_OK);
    if (fbl_state_native_supported() == 1) {
        CHECK(module > 0);
        CHECK(fbl_state_native_functions(lua.handle) > module);
    } else {
        CHECK(fbl_state_native_functions(lua.handle) == 0);
    }
}

TEST_CASE("native code is an option from off to all") {
    int32_t status = FBL_OK;
    const fbl_state_options_t options = with(3);
    CHECK(fbl_state_new(&options, &status) == 0);
    CHECK(status == FBL_MISUSE);
    CHECK(fbl_state_native_functions(0) == -1);
}
