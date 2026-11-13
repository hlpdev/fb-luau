#include "fbl.h"
#include "state/state.hpp"
#include "support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

// A frame on a test's State, closed at the test's end.
struct test_frame_t {
    const test_state_t& lua;
    int32_t number;

    explicit test_frame_t(const test_state_t& state)
        : lua(state), number(fbl_frame_open(state.handle)) {
        REQUIRE(number >= 0);
    }

    test_frame_t(const test_frame_t&) = delete;
    test_frame_t& operator=(const test_frame_t&) = delete;
    test_frame_t(test_frame_t&&) = delete;
    test_frame_t& operator=(test_frame_t&&) = delete;

    ~test_frame_t() {
        fbl_frame_close(lua.handle, number);
    }

    [[nodiscard]] int32_t load(std::string_view source, const char* name = "=test") const {
        return fbl_load_source(lua.handle, number, bytes_of(source),
                               static_cast<int64_t>(source.size()), name);
    }

    [[nodiscard]] int32_t count() const {
        return fbl_frame_count(lua.handle, number);
    }

    [[nodiscard]] double number_at(int32_t slot) const {
        double value = 0;
        REQUIRE(fbl_frame_number(lua.handle, number, slot, &value));
        return value;
    }

    [[nodiscard]] std::string string_at(int32_t slot) const {
        int64_t length = 0;
        const uint8_t* bytes = fbl_frame_string(lua.handle, number, slot, &length);
        REQUIRE(bytes != nullptr);
        return text_of(bytes, length);
    }
};

} // namespace

TEST_CASE("a call gives its results, as many as are asked for") {
    const test_state_t lua;
    const test_frame_t frame(lua);
    REQUIRE(frame.load("local a, b = ... return a + b, a * b, 'third'") == FBL_OK);
    REQUIRE(fbl_frame_push_number(lua.handle, frame.number, 3) == FBL_OK);
    REQUIRE(fbl_frame_push_number(lua.handle, frame.number, 4) == FBL_OK);
    REQUIRE(fbl_call_function(lua.handle, frame.number, 0, -1) == FBL_OK);
    REQUIRE(frame.count() == 3);
    CHECK(frame.number_at(0) == 7);
    CHECK(frame.number_at(1) == 12);
    CHECK(frame.string_at(2) == "third");

    REQUIRE(fbl_frame_truncate(lua.handle, frame.number, 0));
    REQUIRE(frame.load("return 1, 2, 3") == FBL_OK);
    REQUIRE(fbl_call_function(lua.handle, frame.number, 0, 1) == FBL_OK);
    CHECK(frame.count() == 1);
    REQUIRE(frame.load("return 1") == FBL_OK);
    REQUIRE(fbl_call_function(lua.handle, frame.number, 1, 3) == FBL_OK);
    CHECK(frame.count() == 4);
    CHECK(fbl_frame_type(lua.handle, frame.number, 3) == FBL_TYPE_NIL);
}

TEST_CASE("an error gives its message, and the traceback of where it was raised") {
    const test_state_t lua;
    CHECK(lua.run("local function inner() error('boom') end\n"
                  "local function outer() inner() end\n"
                  "outer()",
                  "@mods/doors.luau") == FBL_RUNTIME);
    CHECK(lua.error_message() == "mods/doors.luau:1: boom");
    const std::string traceback = lua.error_traceback();
    CHECK(traceback.contains("mods/doors.luau:1 function inner"));
    CHECK(traceback.contains("mods/doors.luau:2 function outer"));
    CHECK(traceback.contains("mods/doors.luau:3"));
    CHECK(fbl_state_error_kind(lua.handle) == FBL_RUNTIME);
    // The State goes on.
    CHECK(lua.run("x = 1") == FBL_OK);
}

TEST_CASE("an error that isn't a string says what it is") {
    const test_state_t lua;
    CHECK(lua.run("error({ code = 5 })") == FBL_RUNTIME);
    CHECK(lua.error_message() == "(an error that's a table value)");
    CHECK(lua.run("error(42)") == FBL_RUNTIME);
    // A number is a message, which error() puts where it was raised before, as Lua does.
    CHECK(lua.error_message() == "test:1: 42");
}

TEST_CASE("source that doesn't compile is a syntax error with its line") {
    const test_state_t lua;
    const test_frame_t frame(lua);
    CHECK(frame.load("local x = 1\nlocal y = = 2", "=broken") == FBL_SYNTAX);
    CHECK(lua.error_message().starts_with("broken:2:"));
    CHECK(frame.count() == 0);
    CHECK(fbl_load_source(lua.handle, frame.number, bytes_of("x"), 1, nullptr) == FBL_MISUSE);
    CHECK(fbl_load_source(lua.handle, frame.number, nullptr, 5, "=x") == FBL_MISUSE);
    CHECK(frame.load("") == FBL_OK);
}

TEST_CASE("bit32's shifts by the ends of an int give numbers") {
    // Luau's b_shift negates a count of -2147483648, which compilers wrap, and the sanitizers'
    // builds don't check in lbitlib.cpp (cmake/luau.cmake); a count past 31 bits is Luau's own.
    const test_state_t lua;
    const int32_t status = lua.run(
        "for _, x in { 0, 1, 0x80000000, 0xFFFFFFFF } do\n"
        "    for _, f in { bit32.rshift, bit32.lshift, bit32.arshift } do\n"
        "        assert(type(f(x, -2147483648)) == 'number')\n"
        "    end\n"
        "    assert(bit32.rshift(x, 2147483647) == 0)\n"
        "    assert(bit32.lshift(x, 2147483647) == 0)\n"
        "    assert(bit32.arshift(x, 2147483647) == (x >= 0x80000000 and 0xFFFFFFFF or 0))\n"
        "end\n");
    INFO(lua.error_message());
    CHECK(status == FBL_OK);
}

TEST_CASE("os.time and os.date at the ends of an int give the same answer each time") {
    // Luau's os_time takes 1 from a script's month and 1900 from its year, its os_timegm adds
    // and multiplies them as ints, and os_date adds 1900 to gmtime's year, which overflow at the
    // ends of an int; loslib.cpp is built with -fwrapv, so each is defined (cmake/luau.cmake).
    const test_state_t lua;
    const int32_t status =
        lua.run("local function same(f, ...)\n"
                "    local a, b = f(...), f(...)\n"
                "    assert(a == nil or type(a) == 'number' or type(a) == 'table')\n"
                "    assert(type(a) == type(b))\n"
                "    if type(a) == 'table' then assert(a.year == b.year and a.day == b.day)\n"
                "    else assert(a == b) end\n"
                "end\n"
                "for _, field in { 'year', 'month', 'day', 'hour', 'min', 'sec' } do\n"
                "    for _, value in { -2147483648, 2147483647 } do\n"
                "        local date = { year = 2000, month = 1, day = 1 }\n"
                "        date[field] = value\n"
                "        same(os.time, date)\n"
                "    end\n"
                "end\n"
                "-- A time whose year is past an int's largest less 1900.\n"
                "same(os.date, '!*t', 67767987341548800)\n");
    INFO(lua.error_message());
    CHECK(status == FBL_OK);
}

TEST_CASE("Luau's libraries given the ends of an int give the same answer each time") {
    // Each call has Luau add to or take from a script's int, past an int's ends: table.concat's
    // index less 1, table.find's next index, buffer.copy's and buffer.fill's sizes, utf8.codes'
    // and ipairs' iterators' next, and bit32's negated counts and fields. Their files are built
    // with -fwrapv, so each is defined (cmake/luau.cmake).
    const test_state_t lua;
    const int32_t status =
        lua.run("local function same(f, ...)\n"
                "    local a = table.pack(pcall(f, ...))\n"
                "    local b = table.pack(pcall(f, ...))\n"
                "    assert(a.n == b.n)\n"
                "    for i = 1, a.n do assert(a[i] == b[i], tostring(a[i])) end\n"
                "end\n"
                "local low, high = -2147483648, 2147483647\n"
                "same(table.concat, {}, '', low, low)\n"
                "local t = {}\n"
                "t[high] = 1\n"
                "same(table.find, t, 0, high)\n"
                "local b = buffer.create(4)\n"
                "same(buffer.copy, b, 0, b, low)\n"
                "same(buffer.copy, b, 0, b, low, 1)\n"
                "same(buffer.fill, b, low, 0)\n"
                "same(utf8.codes('x'), 'x', low)\n"
                "same(ipairs({}), {}, high)\n"
                "same(bit32.rrotate, 1, low)\n"
                "same(bit32.arshift, 0x80000000, low)\n");
    INFO(lua.error_message());
    CHECK(status == FBL_OK);
}

TEST_CASE("a yield where nothing can wait for it is an error") {
    const test_state_t lua;
    CHECK(lua.run("coroutine.yield(1)") == FBL_YIELD);
    CHECK(lua.error_message().contains("nothing can wait"));
    // A coroutine of the script's own yields as it always does.
    CHECK(lua.run("local co = coroutine.wrap(function() coroutine.yield(1) return 2 end)\n"
                  "assert(co() == 1 and co() == 2)") == FBL_OK);
    CHECK(lua.run("x = 1") == FBL_OK);
}

TEST_CASE("calling what isn't a function, or past a frame, is refused") {
    const test_state_t lua;
    const test_frame_t frame(lua);
    REQUIRE(fbl_frame_push_number(lua.handle, frame.number, 1) == FBL_OK);
    CHECK(fbl_call_function(lua.handle, frame.number, 0, 0) == FBL_MISUSE);
    CHECK(fbl_call_function(lua.handle, frame.number, 1, 0) == FBL_MISUSE);
    CHECK(fbl_call_function(lua.handle, frame.number, 0, -2) == FBL_MISUSE);
    CHECK(fbl_call_function(lua.handle, frame.number + 1, 0, 0) == FBL_MISUSE);
}

TEST_CASE("work under protection that throws is an error, never an unwinding") {
    const test_state_t lua;
    fbl::state_t* state = fbl::find_state(lua.handle);
    REQUIRE(state != nullptr);
    const int32_t frame = fbl_frame_open(lua.handle);
    lua_State* thread = state->frames.back().thread;
    auto standard = [](lua_State*) -> int { throw std::runtime_error("a standard one"); };
    CHECK(fbl::settle(*state, thread, fbl::protect(*state, thread, 0, 0, standard)) == FBL_RUNTIME);
    CHECK(lua.error_message() == "a standard one");
    // A throw of what isn't a std::exception, which only a bug would make, on purpose.
    // NOLINTNEXTLINE(bugprone-std-exception-baseclass)
    auto other = [](lua_State*) -> int { throw 42; };
    CHECK(fbl::settle(*state, thread, fbl::protect(*state, thread, 0, 0, other)) == FBL_RUNTIME);
    CHECK(lua.error_message().contains("isn't a std::exception"));
    CHECK(fbl_frame_count(lua.handle, frame) == 0);
    fbl_frame_close(lua.handle, frame);
}
