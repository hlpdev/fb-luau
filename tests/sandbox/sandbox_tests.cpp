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

namespace {

// A script, run inside the shim's protected work by a table's __index, that finds the shim's own
// C function on the stack, and calls it with nothing, a number, a table, and a proxy: each call an
// error, as it trusts no caller. It counts the calls that failed in `refused`.
constexpr std::string_view probe =
    "refused = 0\n"
    "probe = setmetatable({}, { __index = function()\n"
    "  for level = 1, 50 do\n"
    "    local f, n = debug.info(level, 'fn')\n"
    "    if f == nil then break end\n"
    "    if n == 'fbl' then\n"
    "      for _, arg in { false, 1, {}, newproxy() } do\n"
    "        if arg == false then arg = nil end\n"
    "        if not pcall(f, arg) then refused += 1 end\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  return 7\n"
    "end })";

} // namespace

TEST_CASE("the shim's own function, reached by a script, runs nothing") {
    const test_state_t lua;
    REQUIRE(lua.run(probe) == FBL_OK);
    const int32_t frame = fbl_frame_open(lua.handle);
    REQUIRE(fbl_frame_get_global(lua.handle, frame, bytes_of("probe"), 5) == FBL_OK);
    REQUIRE(fbl_frame_push_string(lua.handle, frame, bytes_of("x"), 1) == FBL_OK);
    REQUIRE(fbl_table_get(lua.handle, frame, 0, 1) == FBL_OK);
    double got = 0;
    CHECK(fbl_frame_number(lua.handle, frame, 2, &got));
    CHECK(got == 7);
    CHECK(fbl_frame_close(lua.handle, frame));
    // It was found and called four times, each refused, and the State goes on.
    CHECK(lua.run("assert(refused == 4, refused)") == FBL_OK);
}

TEST_CASE("getfenv and setfenv reach no thread's globals and no C function's environment") {
    fbl_state_options_t options = default_options();
    options.environments = true;
    options.load_string = true;
    const test_state_t lua(options);
    CHECK(lua.run("setfenv(0, {})") == FBL_RUNTIME);
    CHECK(lua.error_message().contains("a thread's globals can't be reached"));
    CHECK(lua.run("getfenv(0)") == FBL_RUNTIME);
    CHECK(lua.run("getfenv(print)") == FBL_RUNTIME);
    CHECK(lua.error_message().contains("a C function's environment can't be reached"));
    CHECK(lua.run("setfenv(print, {})") == FBL_RUNTIME);
    // A Luau function's, and the caller's, as Luau's own.
    CHECK(lua.run("local function f() return x end setfenv(f, { x = 5 }) assert(f() == 5)\n"
                  "assert(getfenv(1) == _G and getfenv() == _G)") == FBL_OK);
}

TEST_CASE("one script can't take over another's loadstring through a reused runner") {
    fbl_state_options_t options = default_options();
    options.environments = true;
    options.load_string = true;
    const test_state_t lua(options);
    // Script A tries to put its own table in as the runner's globals.
    CHECK(run_in_environment(lua, "_spy = setmetatable({}, { __index = getfenv(0) }) "
                                  "setfenv(0, _spy)") == FBL_RUNTIME);
    CHECK(run_in_environment(lua, "spy = {} setfenv(1, spy)") == FBL_OK);
    // Script B's loaded code writes and reads its own environment.
    CHECK(run_in_environment(lua, "loadstring('token = 1')() assert(token == 1)") == FBL_OK);
    CHECK(run_in_environment(lua, "assert(token == nil)") == FBL_OK);
}

TEST_CASE("loadstring loads into its caller's environment") {
    fbl_state_options_t options = default_options();
    options.load_string = true;
    const test_state_t lua(options);
    CHECK(lua.run("x = 1 assert(loadstring('return x')() == 1) loadstring('y = 2')() "
                  "assert(y == 2)") == FBL_OK);
    CHECK(run_in_environment(lua, "x = 3 assert(loadstring('return x')() == 3) "
                                  "loadstring('z = 4')() assert(z == 4)") == FBL_OK);
    CHECK(run_in_environment(lua, "assert(z == nil and x == 1)") == FBL_OK);
    CHECK(lua.run("local f, e = loadstring('return +') assert(f == nil and e)") == FBL_OK);
}
