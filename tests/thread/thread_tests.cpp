#include "fbl.h"
#include "support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

// The program's functions, as the test's callback has them, by number.
constexpr int64_t pause = 1;
constexpr int64_t can_yield = 2;
constexpr int64_t current = 3;
constexpr int64_t resume_it = 4;

// What `can_yield` last said, and the id of the thread `current` last saw.
int32_t yield_said = -2;
uint64_t current_id = 0;
// What `resume_it`'s resume of the thread it was given gave, and the thread's status after.
int32_t resumed_status = -1;
int32_t resumed_after = -1;
std::string resumed_error;

int32_t test_callback(uint64_t state, int32_t frame, int64_t function) {
    switch (function) {
    case pause:
        // Yields its arguments, as coroutine.yield does.
        return FBL_YIELD;
    case can_yield:
        yield_said = fbl_thread_can_yield(state, frame);
        fbl_frame_truncate(state, frame, 0);
        return FBL_OK;
    case current:
        fbl_frame_truncate(state, frame, 0);
        if (fbl_thread_current(state, frame) != FBL_OK) {
            return FBL_OTHER;
        }
        current_id = fbl_thread_id(state, frame, 0);
        fbl_frame_truncate(state, frame, 0);
        return FBL_OK;
    case resume_it: {
        // Resumes the thread its first argument is, from where it's called.
        int32_t ended = -1;
        fbl_frame_truncate(state, frame, 1);
        if (fbl_frame_push_copy(state, frame, 0) != FBL_OK) {
            return FBL_OTHER;
        }
        resumed_status = fbl_thread_resume(state, frame, 1, 0, &ended);
        resumed_after = fbl_thread_status(state, frame, 0);
        int64_t length = 0;
        const uint8_t* message = fbl_state_error_message(state, &length);
        resumed_error = message == nullptr ? std::string()
                                           : std::string(reinterpret_cast<const char*>(message),
                                                         static_cast<size_t>(length));
        fbl_frame_truncate(state, frame, 0);
        return FBL_OK;
    }
    default:
        return FBL_OTHER;
    }
}

// A State with the test's functions as globals of the same names.
struct thread_state_t : test_state_t {
    thread_state_t() : thread_state_t(default_options()) {}

    explicit thread_state_t(const fbl_state_options_t& options) : test_state_t(options) {
        REQUIRE(fbl_callback_set(test_callback));
        set("pause", pause);
        set("can_yield", can_yield);
        set("current", current);
        set("resume_it", resume_it);
    }

    void set(std::string_view name, int64_t function) const {
        const int32_t frame = fbl_frame_open(handle);
        REQUIRE(fbl_callback_function(handle, frame, function, std::string(name).c_str()) ==
                FBL_OK);
        REQUIRE(fbl_frame_set_global(handle, frame, bytes_of(name),
                                     static_cast<int64_t>(name.size()), 0) == FBL_OK);
        REQUIRE(fbl_frame_close(handle, frame));
    }

    // Opens a frame with a new thread for the global function `name` in slot 0.
    [[nodiscard]] int32_t spawned(std::string_view name) const {
        const int32_t frame = fbl_frame_open(handle);
        REQUIRE(fbl_frame_get_global(handle, frame, bytes_of(name),
                                     static_cast<int64_t>(name.size())) == FBL_OK);
        REQUIRE(fbl_thread_new(handle, frame, 0) == FBL_OK);
        fbl_frame_remove(handle, frame, 0);
        REQUIRE(fbl_frame_type(handle, frame, 0) == FBL_TYPE_THREAD);
        return frame;
    }

    // Resumes the thread in slot 0, which it keeps, with `argument` unless it's nil's: the status,
    // whether it finished, and what it gave is left in the frame from slot 1.
    int32_t resume(int32_t frame, double argument, bool& finished) const {
        fbl_frame_truncate(handle, frame, 1);
        REQUIRE(fbl_frame_push_copy(handle, frame, 0) == FBL_OK);
        REQUIRE(fbl_frame_push_number(handle, frame, argument) == FBL_OK);
        int32_t ended = -1;
        const int32_t status = fbl_thread_resume(handle, frame, 1, 0, &ended);
        finished = ended == 1;
        return status;
    }

    [[nodiscard]] double number(int32_t frame, int32_t slot) const {
        double value = 0;
        REQUIRE(fbl_frame_number(handle, frame, slot, &value));
        return value;
    }
};

} // namespace

TEST_CASE("a thread runs its function, yields, and goes on where it yielded") {
    const thread_state_t lua;
    REQUIRE(lua.run("function steps(a) local b = coroutine.yield(a + 1) return a + b end") ==
            FBL_OK);
    const int32_t frame = lua.spawned("steps");
    CHECK(fbl_thread_status(lua.handle, frame, 0) == FBL_THREAD_SUSPENDED);
    bool finished = true;
    REQUIRE(lua.resume(frame, 10, finished) == FBL_OK);
    CHECK_FALSE(finished);
    CHECK(fbl_frame_count(lua.handle, frame) == 2);
    CHECK(lua.number(frame, 1) == 11);
    CHECK(fbl_thread_status(lua.handle, frame, 0) == FBL_THREAD_SUSPENDED);
    REQUIRE(lua.resume(frame, 5, finished) == FBL_OK);
    CHECK(finished);
    CHECK(lua.number(frame, 1) == 15);
    CHECK(fbl_thread_status(lua.handle, frame, 0) == FBL_THREAD_FINISHED);
    CHECK(lua.resume(frame, 0, finished) == FBL_RUNTIME);
    CHECK(lua.error_message() == "cannot resume a thread that's dead");
    CHECK(fbl_frame_count(lua.handle, frame) == 1);
    CHECK(fbl_frame_close(lua.handle, frame));
}

TEST_CASE("a function of the program's yields its thread with what it leaves in its frame") {
    const thread_state_t lua;
    REQUIRE(lua.run("function waits(a) local b, c = pause(a, 'x') return b * c end") == FBL_OK);
    const int32_t frame = lua.spawned("waits");
    bool finished = true;
    REQUIRE(lua.resume(frame, 3, finished) == FBL_OK);
    CHECK_FALSE(finished);
    CHECK(fbl_frame_count(lua.handle, frame) == 3);
    CHECK(lua.number(frame, 1) == 3);
    CHECK(fbl_frame_type(lua.handle, frame, 2) == FBL_TYPE_STRING);
    // It goes on with what it's resumed with as the function's results.
    fbl_frame_truncate(lua.handle, frame, 1);
    REQUIRE(fbl_frame_push_copy(lua.handle, frame, 0) == FBL_OK);
    REQUIRE(fbl_frame_push_number(lua.handle, frame, 6) == FBL_OK);
    REQUIRE(fbl_frame_push_number(lua.handle, frame, 7) == FBL_OK);
    int32_t ended = 0;
    REQUIRE(fbl_thread_resume(lua.handle, frame, 1, 0, &ended) == FBL_OK);
    CHECK(ended == 1);
    CHECK(lua.number(frame, 1) == 42);
    CHECK(fbl_frame_close(lua.handle, frame));
}

TEST_CASE("only a thread something waits for can yield") {
    const thread_state_t lua;
    // On a call of the program's, which runs to its end, a yield is YIELD.
    CHECK(lua.run("can_yield()") == FBL_OK);
    CHECK(yield_said == 0);
    CHECK(lua.run("pause()") == FBL_YIELD);
    CHECK(lua.run("coroutine.yield()") == FBL_YIELD);
    CHECK(lua.error_message().contains("nothing can wait for it"));
    // In a thread, it can, but not in a metamethod.
    REQUIRE(lua.run("function free() can_yield() end\n"
                    "function inside() local t = setmetatable({}, { __index = function() "
                    "can_yield() return pause() end }) return t.x end") == FBL_OK);
    bool finished = false;
    int32_t frame = lua.spawned("free");
    REQUIRE(lua.resume(frame, 0, finished) == FBL_OK);
    CHECK(yield_said == 1);
    CHECK(fbl_frame_close(lua.handle, frame));
    // The shim's refusal is a yield's error.
    frame = lua.spawned("inside");
    CHECK(lua.resume(frame, 0, finished) == FBL_YIELD);
    CHECK(yield_said == 0);
    CHECK(lua.error_message().contains("attempt to yield where nothing can wait for it"));
    CHECK(fbl_thread_status(lua.handle, frame, 0) == FBL_THREAD_FAILED);
    CHECK(fbl_frame_close(lua.handle, frame));
    CHECK(fbl_thread_can_yield(lua.handle, 9) == -1);
}

TEST_CASE("an error raised in a thread where it yielded is its pcall's, or ends it") {
    const thread_state_t lua;
    REQUIRE(lua.run("function caught() local ok, e = pcall(pause) return ok, e end\n"
                    "function uncaught() pause() end") == FBL_OK);
    int32_t frame = lua.spawned("caught");
    bool finished = false;
    REQUIRE(lua.resume(frame, 0, finished) == FBL_OK);
    fbl_frame_truncate(lua.handle, frame, 1);
    REQUIRE(fbl_frame_push_copy(lua.handle, frame, 0) == FBL_OK);
    REQUIRE(fbl_frame_push_string(lua.handle, frame, bytes_of("late"), 4) == FBL_OK);
    int32_t ended = 0;
    REQUIRE(fbl_thread_resume(lua.handle, frame, 1, 1, &ended) == FBL_OK);
    CHECK(ended == 1);
    CHECK(fbl_frame_type(lua.handle, frame, 1) == FBL_TYPE_BOOLEAN);
    CHECK(fbl_frame_type(lua.handle, frame, 2) == FBL_TYPE_STRING);
    CHECK(fbl_frame_close(lua.handle, frame));
    frame = lua.spawned("uncaught");
    REQUIRE(lua.resume(frame, 0, finished) == FBL_OK);
    fbl_frame_truncate(lua.handle, frame, 1);
    REQUIRE(fbl_frame_push_copy(lua.handle, frame, 0) == FBL_OK);
    REQUIRE(fbl_frame_push_string(lua.handle, frame, bytes_of("late"), 4) == FBL_OK);
    CHECK(fbl_thread_resume(lua.handle, frame, 1, 1, &ended) == FBL_RUNTIME);
    CHECK(lua.error_message() == "late");
    CHECK(lua.error_traceback().contains("uncaught"));
    CHECK(fbl_frame_count(lua.handle, frame) == 1);
    CHECK(fbl_thread_status(lua.handle, frame, 0) == FBL_THREAD_FAILED);
    CHECK(fbl_frame_close(lua.handle, frame));
}

TEST_CASE("an error can't be raised in a thread that hasn't started, or be two values") {
    const thread_state_t lua;
    REQUIRE(lua.run("function f() end") == FBL_OK);
    const int32_t frame = lua.spawned("f");
    REQUIRE(fbl_frame_push_copy(lua.handle, frame, 0) == FBL_OK);
    REQUIRE(fbl_frame_push_number(lua.handle, frame, 1) == FBL_OK);
    int32_t ended = 0;
    CHECK(fbl_thread_resume(lua.handle, frame, 1, 1, &ended) == FBL_RUNTIME);
    CHECK(lua.error_message().contains("hasn't started"));
    REQUIRE(fbl_frame_push_copy(lua.handle, frame, 0) == FBL_OK);
    REQUIRE(fbl_frame_push_number(lua.handle, frame, 1) == FBL_OK);
    REQUIRE(fbl_frame_push_number(lua.handle, frame, 2) == FBL_OK);
    CHECK(fbl_thread_resume(lua.handle, frame, 1, 1, &ended) == FBL_MISUSE);
    CHECK(fbl_thread_resume(lua.handle, frame, 2, 0, &ended) == FBL_MISUSE);
    CHECK(fbl_thread_resume(lua.handle, frame, 0, 0, nullptr) == FBL_MISUSE);
    CHECK(fbl_thread_status(lua.handle, frame, 2) == -1);
    CHECK(fbl_thread_id(lua.handle, frame, 2) == 0);
    CHECK(fbl_frame_close(lua.handle, frame));
}

TEST_CASE("a closed thread is finished, and a running one can't be closed") {
    const thread_state_t lua;
    REQUIRE(lua.run("function f() pause() end") == FBL_OK);
    const int32_t frame = lua.spawned("f");
    bool finished = false;
    REQUIRE(lua.resume(frame, 0, finished) == FBL_OK);
    REQUIRE(fbl_thread_close(lua.handle, frame, 0) == FBL_OK);
    CHECK(fbl_thread_status(lua.handle, frame, 0) == FBL_THREAD_FINISHED);
    CHECK(lua.resume(frame, 0, finished) == FBL_RUNTIME);
    REQUIRE(fbl_thread_current(lua.handle, frame) == FBL_OK);
    CHECK(fbl_thread_close(lua.handle, frame, fbl_frame_count(lua.handle, frame) - 1) ==
          FBL_RUNTIME);
    CHECK(fbl_frame_close(lua.handle, frame));
}

TEST_CASE("a resume that fails as it starts, past Luau's C calls, ends the thread") {
    // An error handler runs past Luau's 200 C calls, where a resume fails before the thread
    // runs; the thread is ended, as the header says an error ends it, not left suspended.
    const thread_state_t lua;
    resumed_status = -1;
    resumed_after = -1;
    REQUIRE(lua.run("co = coroutine.create(function() return 1 end) "
                    "local function deep() string.gsub('x', 'x', deep) end "
                    "ok = xpcall(deep, function() resume_it(co) return 'handled' end)") == FBL_OK);
    CHECK(resumed_status == FBL_RUNTIME);
    CHECK(resumed_error.contains("C stack overflow"));
    CHECK(resumed_after != FBL_THREAD_SUSPENDED);
    CHECK(lua.run("assert(coroutine.status(co) == 'dead') assert(not ok)") == FBL_OK);
}

TEST_CASE("a thread's id is the same for each of its values") {
    const thread_state_t lua;
    REQUIRE(lua.run("function f() current() end") == FBL_OK);
    const int32_t frame = lua.spawned("f");
    const uint64_t id = fbl_thread_id(lua.handle, frame, 0);
    CHECK(id != 0);
    bool finished = false;
    REQUIRE(lua.resume(frame, 0, finished) == FBL_OK);
    CHECK(current_id == id);
    CHECK(fbl_frame_close(lua.handle, frame));
}

TEST_CASE("a thread a call of the program's runs on can't be resumed") {
    const thread_state_t lua;
    REQUIRE(lua.run("runner = coroutine.running()") == FBL_OK);
    const int32_t frame = lua.spawned("print");
    REQUIRE(fbl_frame_get_global(lua.handle, frame, bytes_of("runner"), 6) == FBL_OK);
    int32_t ended = 0;
    CHECK(fbl_thread_resume(lua.handle, frame, 1, 0, &ended) == FBL_RUNTIME);
    CHECK(fbl_thread_close(lua.handle, frame, 0) == FBL_OK);
    REQUIRE(fbl_frame_get_global(lua.handle, frame, bytes_of("runner"), 6) == FBL_OK);
    CHECK(fbl_thread_close(lua.handle, frame, 1) == FBL_RUNTIME);
    CHECK(fbl_frame_close(lua.handle, frame));
}

TEST_CASE("each resume from the program has its own time budget") {
    fbl_state_options_t options = default_options();
    options.time_budget = 20000;
    const thread_state_t lua(options);
    REQUIRE(lua.run("function spin() pause() while true do end end") == FBL_OK);
    const int32_t frame = lua.spawned("spin");
    bool finished = false;
    REQUIRE(lua.resume(frame, 0, finished) == FBL_OK);
    const auto started = std::chrono::steady_clock::now();
    CHECK(lua.resume(frame, 0, finished) == FBL_TIMEOUT);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
    CHECK(fbl_frame_close(lua.handle, frame));
}
