#pragma once

#include "core/fbl_core.h"

#include <chrono>
#include <cstdint>
#include <lua.h>
#include <luacode.h>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
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

// Work protect() runs: a function and its data, as C++ erases a lambda's type.
using work_function_t = int (*)(lua_State*, void*);
struct work_t {
    work_function_t function = nullptr;
    void* data = nullptr;
};

// A require's walk to its module: the module path's parts it's at, that module's first file, and
// the files it's found, by their paths, while it walks.
struct require_walk_t {
    std::vector<std::string> parts;
    std::string file;
    std::unordered_map<std::string, std::string> found;
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
    // How many functions its code generator has compiled.
    int64_t native_functions = 0;
    // For the tests: which allocation from now fails, counting from 0, once, or -1 for none; and
    // which of the shim's failure points from now raises, as an allocation's failing would, since
    // Luau's own pages hold small objects without asking the allocator.
    int64_t failing_allocation = -1;
    mutable int64_t failing_point = -1;
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
    // For the tests: whether the next resume throws as it starts, as Luau's can.
    bool failing_resume = false;
    // Whether it has Luau's code generator.
    bool code_generator = false;
    lua_CompileOptions compile_options{};
    // The registry's reference to the C function protect() runs work in, made with the State, and
    // the work it runs next, which only protect() sets, and which that function takes and clears
    // before it runs it, so a script that reaches the function finds nothing to run.
    int protect_ref = LUA_NOREF;
    // What it compiles to native code, an FBL_NATIVE_.
    int32_t native_code = 0;
    mutable work_t pending_work;
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
    // The registry's weak table of each thread's number, and the last number given.
    int thread_ids_ref = LUA_NOREF;
    uint64_t next_thread_id = 0;
    // Whether the program's driver has a turn open, with the time budget for all it resumes.
    bool turn_open = false;
    // require's: whether the State has it, the registry's reference to the program's reader of its
    // files, or none; how many modules are loading; and the walk of the require running.
    bool require_open = false;
    int require_reader = LUA_NOREF;
    int requires_running = 0;
    require_walk_t require_walk;
    // The last error a call met, its value kept by the registry at error_ref.
    int error_ref = LUA_NOREF;
    bool error_has_value = false;
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
int protect_erased(state_t& state, lua_State* thread, int arguments, int results,
                   work_function_t function, void* data) noexcept;

template <typename Work>
int protect(state_t& state, lua_State* thread, int arguments, int results, Work& work) noexcept {
    return protect_erased(
        state, thread, arguments, results,
        [](lua_State* stack, void* data) { return (*static_cast<Work*>(data))(stack); }, &work);
}

// After an error of `kind`, as record_error keeps one: a MEMORY error, when no call's running,
// collects everything, as the
// collector stops past the limit, and the State then has room again.
void collect_after(const state_t& state, int32_t kind) noexcept;

// What a protect()'s status is to the program: FBL_OK, or the error, kept as the State's, its
// value taken off `thread`'s stack.
int32_t settle(state_t& state, lua_State* thread, int status) noexcept;

// protect() then settle(), on `thread`, which is taken by value before the work runs: the work can
// run a script's code, which can call the program, which opens frames, so nothing in `frames` is
// read after it.
template <typename Work>
int32_t run_protected(state_t& state, lua_State* thread, int arguments, int results,
                      Work& work) noexcept {
    return settle(state, thread, protect(state, thread, arguments, results, work));
}

// Makes room in `queue`, the numbers of what Luau has collected that the program hasn't taken,
// for one more than those and the `live` ones, so a destructor never needs memory. It throws
// std::bad_alloc, or std::length_error.
void make_room(std::vector<int64_t>& queue, int64_t live);

// Queues a collected number, in room make_room() kept; that there's none is a bug, which aborts a
// Debug build, and loses the number in another.
void keep_released(std::vector<int64_t>& queue, int64_t number) noexcept;

// The userdata tag of a program's function's number, whose destructor gives it back.
constexpr int function_tag = 1;

// Sets a new State up for the program's functions: their destructor.
void prepare_callbacks(lua_State* thread) noexcept;

// The userdata tag of a program's object, whose destructor gives its number back.
constexpr int object_tag = 2;

// Sets a new State up for the program's objects: their destructor, and their weak table.
void prepare_objects(lua_State* thread, state_t& state);

// What the shim's errors for a yield nothing can wait for say first, by which a call's error is
// FBL_YIELD.
constexpr std::string_view yield_refused = "attempt to yield where nothing can wait for it";

// One more call into Luau while it lives: the outermost, from the program, starts the time
// budget's deadline, which can't overflow; and on every way out, the depth is put back and the
// outermost's deadline cleared.
class entered_t {
public:
    explicit entered_t(state_t& state) noexcept;
    ~entered_t();
    entered_t(const entered_t&) = delete;
    entered_t& operator=(const entered_t&) = delete;
    entered_t(entered_t&&) = delete;
    entered_t& operator=(entered_t&&) = delete;

private:
    state_t& m_state;
    bool m_outermost;
};

// Whether a thread is one the program's calls run on, which nothing else resumes, and which can't
// yield to anything that waits.
bool is_runner(lua_State* thread) noexcept;

// Marks a thread as one that runs to its end, as a runner does, like a module's as it loads.
void mark_runner(lua_State* thread) noexcept;

// Resumes `co` from `thread`, with the `arguments` on top of its stack, or raising the value on top
// of it, a call one deeper, which has the time budget when it's the outermost: Luau's status.
int resume_from(state_t& state, lua_State* thread, lua_State* co, int arguments,
                bool raise) noexcept;

// Keeps the error a resume of `co` met, the value on top of its stack, with where it was raised,
// and resets `co` for its next call, when `reset`: the error's kind. A yield is FBL_YIELD's, as
// nothing waits for a runner.
int32_t failed(state_t& state, lua_State* thread, lua_State* co, int status, bool reset) noexcept;

// Pushes a new environment for a script, or a module, onto `stack`: a table of its own, whose _G is
// itself, and whose metatable, frozen and locked, reads what it hasn't from the main thread's
// globals, which `main` has room on its stack to give. It raises when there's no memory.
void push_environment(lua_State* stack, lua_State* main);

// For the tests: sets the generation of the slot a State's handle names, which gives it a new
// handle, so a test reaches a slot's last generations.
void set_generation(uint64_t handle, uint32_t generation) noexcept;

// For the tests: raises "not enough memory" when it's the State's failing_point, in a work that
// makes something in steps, between them.
void failure_point(lua_State* stack, const state_t& state);

// The stack index of the environment table in `environment_slot` of the frame, 0 for -1, the
// thread's own globals, or -1, saying why, for a slot that isn't a table.
int environment_index(const frame_t& frame, int32_t environment_slot) noexcept;

// Loads compiled `code`, the chunk `name`, onto `thread` as a function, its environment the table
// at stack index `environment`, or the thread's globals for 0: a status, FBL_SYNTAX for an error
// when `syntax`, as source that didn't compile is a 0 and its error.
int32_t load_chunk(state_t& state, lua_State* thread, int environment, std::string_view code,
                   const char* name, bool syntax) noexcept;

// Compiles the function at `index` on `stack`, and those in it, to native code, as the State's
// options say, if it has the code generator: what can't be stays in the interpreter. It never
// raises or throws.
void native_compile(state_t& state, lua_State* stack, int index) noexcept;

// Raises the timeout when the running call is past its deadline.
void check_deadline(lua_State* stack);

// Freezes the table at `index` on `stack`, and every table reachable from it through tables'
// fields, each once, not through metatables or functions. It raises when there's no memory.
void freeze_tables(lua_State* stack, int index);

// Freezes the State's globals, unless they are: a status.
int32_t frozen(state_t& state) noexcept;

// Takes the error value on top of `thread`'s stack as the State's last error, of `kind`, with
// `traceback`, and pops it.
int32_t take_error(state_t& state, lua_State* thread, int32_t kind,
                   std::string_view traceback = {}) noexcept;

} // namespace fbl
