#pragma once

#include "core/fbl_core.h"

#include <chrono>
#include <cstdint>
#include <lua.h>
#include <luacode.h>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace fbl {

// How many calls deep the program and Luau can go into each other, a call into Luau from a
// function Luau called counting as one more, before a call is refused: each is a C++ frame and a
// Luau thread, and Luau's own limit, LUAI_MAXCCALLS, is 200.
constexpr int max_depth = 64;

// Where a frame's values are: the thread whose stack has them, and how many values that stack had
// below them, so its slot 0 is at stack index base + 1.
struct frame_t {
    lua_State* thread = nullptr;
    int base = 0;
};

// A State: a Luau VM of its own, with its allocator's count and limit, the options its code
// compiles with, and what the program has open on it.
struct state_t {
    lua_State* main = nullptr;
    // The thread the program's calls run on now: the main thread, or the one a function of the
    // program's was called on, while it runs.
    lua_State* current = nullptr;
    uint64_t handle = 0;
    std::thread::id owner;
    int64_t memory_limit = 0;
    int64_t memory_used = 0;
    // Each outermost call's time budget, and when the one running ends, or none.
    std::chrono::microseconds time_budget{ 0 };
    std::chrono::steady_clock::time_point deadline;
    bool timed_out = false;
    // What it was made with, and whether its globals are frozen.
    uint32_t libraries = 0;
    int32_t collect_garbage = 0;
    bool load_string = false;
    bool environments = false;
    bool frozen = false;
    lua_CompileOptions compile_options{};
    // The registry's reference to the C function protect() runs work in, made with the State.
    int protect_ref = LUA_NOREF;
    std::vector<frame_t> frames;
    // A thread for each depth of calls, which the registry keeps.
    std::vector<int> runner_refs;
    int depth = 0;
    // The registry's references the program holds, by their numbers, so a stale one is refused.
    std::unordered_set<int> refs;
    // The program's functions Luau has, and the numbers of those it's collected, which the
    // program takes; there's always room for each live one's number, so a destructor never needs
    // memory.
    int64_t live_functions = 0;
    std::vector<int64_t> released;
    // Their names, which Luau keeps pointers to.
    std::unordered_set<std::string> names;
    // The program's objects Luau has, as for its functions; the registry's references to each
    // registered type's metatable, by type from 1; and the registry's weak table of each object's
    // userdata, by its number.
    int64_t live_objects = 0;
    std::vector<int64_t> released_objects;
    std::vector<int> type_refs;
    int objects_ref = LUA_NOREF;
    // The last error a call met, its value kept by the registry at error_ref.
    int error_ref = LUA_NOREF;
    int32_t error_kind = FBL_OK;
    std::string error_message;
    std::string error_traceback;
};

// The bytes every State's VM holds, the process's whole, which the tests and the soak watch come
// back to what it was.
int64_t memory_of_all_states() noexcept;

// The live State a handle names, if it's this thread's; or null, saying why as this thread's last
// error.
state_t* find_state(uint64_t handle) noexcept;

// The State's frame by its number, if it's the State's newest, the only one the program can use;
// or null, saying why.
frame_t* find_frame(state_t& state, int32_t frame) noexcept;

// A slot's stack index in its frame, or 0, saying why, when the frame has no such slot.
int slot_index(const frame_t& frame, int32_t slot) noexcept;

// Says why as this thread's last error, and gives FBL_MISUSE.
int32_t misuse(std::string_view why) noexcept;

// Keeps an error the State met: its kind, its message, and where it was raised.
int32_t record_error(state_t& state, int32_t kind, std::string_view message,
                     std::string_view traceback = {}) noexcept;

// The kind of error a Luau status is.
int32_t kind_of(int status) noexcept;

// Runs `work` under Luau's protection, on `thread`, with a call of a C function made once for the
// State, so any Luau error, or C++ exception, in it is a status, not an unwinding. The top
// `arguments` values of `thread`'s stack are its arguments, at stack indexes 1 and up, which the
// call takes off; it gives LUA_OK, with the `results` values `work` pushes left in their place, or
// an error, with its value there instead; or no_room, with the arguments still there and nothing
// pushed, when the stack can't grow. `work` is called with the thread and gives how many values it
// pushed, as a C function does.
constexpr int no_room = -1;
using work_function_t = int (*)(lua_State*, void*);
int protect_erased(const state_t& state, lua_State* thread, int arguments, int results,
                   work_function_t function, void* data) noexcept;

template <typename Work>
int protect(const state_t& state, lua_State* thread, int arguments, int results,
            Work& work) noexcept {
    return protect_erased(
        state, thread, arguments, results,
        [](lua_State* stack, void* data) { return (*static_cast<Work*>(data))(stack); }, &work);
}

// What a protect()'s status is to the program: FBL_OK, or the error, kept as the State's, its
// value taken off `thread`'s stack.
int32_t settle(state_t& state, lua_State* thread, int status) noexcept;

// The userdata tag of a program's function's number, whose destructor gives it back.
constexpr int function_tag = 1;

// Sets a new State up for the program's functions: their destructor.
void prepare_callbacks(lua_State* thread) noexcept;

// The userdata tag of a program's object, whose destructor gives its number back.
constexpr int object_tag = 2;

// Sets a new State up for the program's objects: their destructor, and their weak table.
void prepare_objects(lua_State* thread, state_t& state);

// Freezes the State's globals, unless they are: a status.
int32_t frozen(state_t& state) noexcept;

// Takes the error value on top of `thread`'s stack as the State's last error, of `kind`, with
// `traceback`, and pops it.
int32_t take_error(state_t& state, lua_State* thread, int32_t kind,
                   std::string_view traceback = {}) noexcept;

} // namespace fbl
