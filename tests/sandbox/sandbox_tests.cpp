#include "fbl.h"
#include "state/state.hpp"
#include "support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <stdexcept>
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
constexpr std::string_view probe = "refused = 0\n"
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

TEST_CASE("a script's metamethod the program's own get or set runs has the time budget") {
    fbl_state_options_t options = default_options();
    options.time_budget = 20000;
    const test_state_t lua(options);
    REQUIRE(lua.run("spin = setmetatable({}, { __index = function() while true do end end, "
                    "__newindex = function() while true do end end })") == FBL_OK);
    const int32_t frame = fbl_frame_open(lua.handle);
    REQUIRE(fbl_frame_get_global(lua.handle, frame, bytes_of("spin"), 4) == FBL_OK);
    REQUIRE(fbl_frame_push_string(lua.handle, frame, bytes_of("x"), 1) == FBL_OK);
    const auto started = std::chrono::steady_clock::now();
    CHECK(fbl_table_get(lua.handle, frame, 0, 1) == FBL_TIMEOUT);
    REQUIRE(fbl_frame_push_number(lua.handle, frame, 1) == FBL_OK);
    CHECK(fbl_table_set(lua.handle, frame, 0, 1, 2) == FBL_TIMEOUT);
    fbl_frame_truncate(lua.handle, frame, 0);
    // The globals' too.
    REQUIRE(lua.run("setmetatable(_G, getmetatable(spin))") == FBL_OK);
    CHECK(fbl_frame_get_global(lua.handle, frame, bytes_of("missing"), 7) == FBL_TIMEOUT);
    REQUIRE(fbl_frame_push_number(lua.handle, frame, 1) == FBL_OK);
    CHECK(fbl_frame_set_global(lua.handle, frame, bytes_of("late"), 4, 0) == FBL_TIMEOUT);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
    CHECK(fbl_frame_close(lua.handle, frame));
    // The next call runs.
    CHECK(lua.run("local n = 0 for i = 1, 100 do n += i end") == FBL_OK);
}

TEST_CASE("a call's depth and deadline are put back however it ends") {
    fbl_state_options_t options = default_options();
    options.time_budget = 1000000;
    const test_state_t lua(options);
    fbl::state_t* state = fbl::find_state(lua.handle);
    REQUIRE(state != nullptr);
    try {
        const fbl::entered_t entered(*state);
        CHECK(state->depth == 1);
        CHECK(state->deadline != std::chrono::steady_clock::time_point{});
        throw std::runtime_error("as Luau's resume can, as it starts");
    } catch (const std::runtime_error& thrown) {
        CHECK(std::string_view(thrown.what()).contains("as it starts"));
    }
    CHECK(state->depth == 0);
    CHECK(state->deadline == std::chrono::steady_clock::time_point{});
}

TEST_CASE("a time budget past what the clock holds is no overflow") {
    fbl_state_options_t options = default_options();
    options.time_budget = INT64_MAX;
    const test_state_t lua(options);
    CHECK(lua.run("local n = 0 for i = 1, 1000 do n += i end") == FBL_OK);
}

TEST_CASE("loadstring refuses a source past the memory left, and keeps to the budget") {
    fbl_state_options_t options = default_options();
    options.load_string = true;
    options.memory_limit = int64_t{ 2 } * 1024 * 1024;
    options.time_budget = 20000;
    const test_state_t lua(options);
    CHECK(lua.run("loadstring(string.rep('x', 1200000))") == FBL_RUNTIME);
    CHECK(lua.run("loadstring(string.rep('local a = 1 ', 2))") == FBL_OK);
    CHECK(lua.run("while true do loadstring('return 1') end") == FBL_TIMEOUT);
}

TEST_CASE("freezing reaches every table in the globals' tables") {
    const test_state_t lua;
    REQUIRE(lua.run("Config = { Graphics = { Shadows = true, Modes = { 'low' } } } "
                    "Config.Self = Config") == FBL_OK);
    REQUIRE(fbl_sandbox_freeze(lua.handle) == FBL_OK);
    CHECK(lua.run("Config.Graphics.Shadows = false") == FBL_RUNTIME);
    CHECK(lua.run("table.insert(Config.Graphics.Modes, 'high')") == FBL_RUNTIME);
    CHECK(lua.run("assert(Config.Graphics.Shadows and Config.Self == Config)") == FBL_OK);
    // A script's own tables are its own.
    CHECK(lua.run("local t = {} t.x = 1") == FBL_OK);
}

TEST_CASE("getfenv and setfenv take a function only of the caller's own environment") {
    fbl_state_options_t options = default_options();
    options.environments = true;
    const test_state_t lua(options);
    // Another environment's function, a script's as a module's would be.
    REQUIRE(lua.run("function shared() return 1 end") == FBL_OK);
    CHECK(run_in_environment(lua, "local ok = pcall(getfenv, shared) assert(not ok)") == FBL_OK);
    CHECK(run_in_environment(lua, "local ok = pcall(setfenv, shared, {}) assert(not ok)") ==
          FBL_OK);
    CHECK(run_in_environment(lua, "local function mine() end assert(getfenv(mine) == _G)") ==
          FBL_OK);
}

TEST_CASE("loadstring through pcall loads into the nearest script's environment") {
    fbl_state_options_t options = default_options();
    options.load_string = true;
    const test_state_t lua(options);
    CHECK(run_in_environment(lua, "x = 7 local ok, f = pcall(loadstring, 'return x') "
                                  "assert(ok and f() == 7)") == FBL_OK);
}

TEST_CASE("with a memory limit, a script can't change the collector's settings") {
    fbl_state_options_t options = default_options();
    options.memory_limit = int64_t{ 8 } * 1024 * 1024;
    options.collect_garbage = FBL_COLLECT_FULL;
    const test_state_t lua(options);
    for (const char* setting : { "setgoal", "setstepmul", "setstepsize", "stop" }) {
        CHECK(lua.run(std::string("collectgarbage('") + setting + "', 400)") == FBL_RUNTIME);
    }
    CHECK(lua.run("collectgarbage('collect') collectgarbage('step')") == FBL_OK);
    const test_state_t unlimited([] {
        fbl_state_options_t full = default_options();
        full.collect_garbage = FBL_COLLECT_FULL;
        return full;
    }());
    CHECK(unlimited.run("collectgarbage('setgoal', 150)") == FBL_OK);
}

TEST_CASE("a call whose resume throws as it starts puts its depth back, and the State goes on") {
    fbl_state_options_t options = default_options();
    options.time_budget = 1000000;
    const test_state_t lua(options);
    fbl::state_t* state = fbl::find_state(lua.handle);
    REQUIRE(state != nullptr);
    state->failing_resume = true;
    CHECK(lua.run("local x = 1") == FBL_MEMORY);
    CHECK(state->depth == 0);
    CHECK(state->deadline == std::chrono::steady_clock::time_point{});
    CHECK(lua.run("local x = 1") == FBL_OK);
    CHECK(fbl_state_close(lua.handle));
}

TEST_CASE("a compile past the time budget is a timeout once it returns") {
    fbl_state_options_t options = default_options();
    options.load_string = true;
    options.time_budget = 1000;
    const test_state_t lua(options);
    // The compile's own call raises it, so a pcall around it sees it, before any safe point after
    // it would.
    std::string source;
    for (int line = 0; line < 200000; line++) {
        source += "local a = 1 ";
    }
    REQUIRE(lua.run("function probe(source) loaded = pcall(loadstring, source) end") == FBL_OK);
    const int32_t frame = fbl_frame_open(lua.handle);
    REQUIRE(fbl_frame_get_global(lua.handle, frame, bytes_of("probe"), 5) == FBL_OK);
    REQUIRE(fbl_frame_push_string(lua.handle, frame, bytes_of(source),
                                  static_cast<int64_t>(source.size())) == FBL_OK);
    CHECK(fbl_call_function(lua.handle, frame, 0, 0) == FBL_TIMEOUT);
    CHECK(fbl_frame_close(lua.handle, frame));
    CHECK(lua.run("assert(loaded == false)") == FBL_OK);
}
