#include "state/state.hpp"

#include "core/core.hpp"
#include "fbl.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <format>
#include <limits>
#include <luacode.h>
#include <lualib.h>
#include <Luau/CodeGen.h>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

// A handle's kind, its low byte, so a handle of another kind is never taken for a State's.
constexpr uint64_t state_kind = 1;

// Every State the process has open, by the slot its handle names; a slot's generation counts the
// States it has held, so a closed one's handle is refused even once the slot holds another.
struct slot_t {
    fbl::state_t* state = nullptr;
    uint32_t generation = 0;
    std::thread::id owner;
};

// The most generations a slot's handle tells apart, its 24 bits: a slot that's held this many
// States is retired, so no handle ever names two.
constexpr uint32_t most_generations = 0xFFFFFFU;

std::atomic<int64_t> all_memory{ 0 };

std::mutex slots_mutex;
std::vector<slot_t> slots;
std::vector<uint32_t> free_slots;

uint64_t handle_of(uint32_t slot, uint32_t generation) {
    return (static_cast<uint64_t>(slot) << 32U) |
           (static_cast<uint64_t>(generation & 0xFFFFFFU) << 8U) | state_kind;
}

// The State's allocator, which keeps to its limit: an allocation past it fails, as one the system
// can't make does, and Luau raises "not enough memory".
// Counts what the code generator takes and gives back, its blocks of native code.
void count_native_memory(void* context, void* /*old_pointer*/, size_t old_size,
                         void* /*new_pointer*/, size_t new_size) {
    auto* state = static_cast<fbl::state_t*>(context);
    state->native_memory_used += static_cast<int64_t>(new_size) - static_cast<int64_t>(old_size);
}

void* allocate(void* data, void* block, size_t old_size, size_t new_size) {
    auto* state = static_cast<fbl::state_t*>(data);
    const auto had = static_cast<int64_t>(block == nullptr ? 0 : old_size);
    if (new_size == 0) {
        std::free(block);
        state->memory_used -= had;
        all_memory -= had;
        return nullptr;
    }
    const auto wants = static_cast<int64_t>(new_size);
    if (state->failing_allocation >= 0 && wants > had) {
        if (state->failing_allocation-- == 0) {
            return nullptr;
        }
    }
    if (state->memory_limit > 0 && wants > had &&
        state->memory_used + (wants - had) > state->memory_limit) {
        return nullptr;
    }
    void* made = std::realloc(block, new_size);
    if (made == nullptr) {
        return nullptr;
    }
    state->memory_used += wants - had;
    all_memory += wants - had;
    return made;
}

// What protect() calls: the work it left in the State, which it takes, so the work's own code, or
// a script that reaches this function as it runs, finds none. Its caller isn't trusted: called
// with no work pending, it raises.
int run_work(lua_State* stack) {
    const auto* state = static_cast<const fbl::state_t*>(lua_callbacks(stack)->userdata);
    if (state == nullptr || state->pending_work.function == nullptr) {
        luaL_error(stack, "the shim's own function can't be called");
    }
    const fbl::work_t work = state->pending_work;
    state->pending_work = {};
    try {
        return work.function(stack, work.data);
    } catch (const std::exception&) {
        // Luau takes these as errors itself.
        throw;
    } catch (...) {
        luaL_error(stack, "a C++ exception that isn't a std::exception");
    }
}

// collectgarbage, as Luau's own tests have it, or only its "count".
int collect_garbage(lua_State* stack) {
    static constexpr std::array<const char*, 10> options = {
        "stop", "restart", "collect",    "count",       "isrunning",
        "step", "setgoal", "setstepmul", "setstepsize", nullptr,
    };
    static constexpr std::array<int, 9> operations = {
        LUA_GCSTOP, LUA_GCRESTART, LUA_GCCOLLECT,    LUA_GCCOUNT,       LUA_GCISRUNNING,
        LUA_GCSTEP, LUA_GCSETGOAL, LUA_GCSETSTEPMUL, LUA_GCSETSTEPSIZE,
    };
    const auto* state = static_cast<const fbl::state_t*>(lua_callbacks(stack)->userdata);
    const int option = luaL_checkoption(stack, 1, "collect", options.data());
    const int operation = operations.at(static_cast<size_t>(option));
    if (state != nullptr && state->collect_garbage != FBL_COLLECT_FULL &&
        operation != LUA_GCCOUNT) {
        luaL_error(stack, "collectgarbage only gives the memory in use, with 'count'");
    }
    if (state != nullptr && state->memory_limit > 0 &&
        (operation == LUA_GCSETGOAL || operation == LUA_GCSETSTEPMUL ||
         operation == LUA_GCSETSTEPSIZE || operation == LUA_GCSTOP)) {
        luaL_error(stack, "with a memory limit, the collector's settings are the State's");
    }
    const int result = lua_gc(stack, operation, luaL_optinteger(stack, 2, 0));
    if (operation == LUA_GCSTEP || operation == LUA_GCISRUNNING) {
        lua_pushboolean(stack, result);
    } else {
        lua_pushnumber(stack, result);
    }
    return 1;
}

// Pushes the environment of the function at `level` above the C function running, or the
// globals' for a C function's, as a C function's own is its creator's globals.
// Pushes the environment of the nearest Luau function up the stack from the C function running,
// past C functions like pcall, which have no environment of a script's: true, or false, pushing
// nothing, when there's none.
bool push_caller_environment(lua_State* stack) {
    lua_Debug info{};
    for (int level = 1; lua_getinfo(stack, level, "f", &info) != 0; level++) {
        if (lua_type(stack, -1) == LUA_TFUNCTION && lua_iscfunction(stack, -1) == 0) {
            lua_getfenv(stack, -1);
            lua_remove(stack, -2);
            return true;
        }
        lua_pop(stack, 1);
    }
    return false;
}

// loadstring: compiles source with the State's options, never loading bytecode, into a function of
// its caller's environment, which it marks unsafe for Luau's fast paths, as Luau's own does; nil
// and the error for source that doesn't compile. The chunk loads on a thread whose globals are
// that environment, as Luau resolves a chunk's globals from the loading thread's.
int load_string(lua_State* stack) {
    const auto* state = static_cast<const fbl::state_t*>(lua_callbacks(stack)->userdata);
    size_t length = 0;
    const char* source = luaL_checklstring(stack, 1, &length);
    // Named by its source when it isn't given a name, as Luau's and Lua's loadstring name it,
    // which its errors show as [string "..."]. A path's name a script gives, "@...", is shown as
    // Luau shows one; require from it goes through the same checks as any chunk's, so it
    // reaches nothing above the root, and no further than a require's path may.
    const std::string name = luaL_optstring(stack, 2, source);
    // The compiler's memory isn't the State's, so a source bigger than what the limit leaves is
    // refused before it's compiled.
    if (state != nullptr && state->memory_limit > 0 &&
        std::cmp_greater(length, state->memory_limit - state->memory_used)) {
        luaL_error(stack, "a source of %llu bytes is more than the State's memory has left",
                   static_cast<unsigned long long>(length));
    }
    lua_settop(stack, 2);
    luaL_checkstack(stack, 4, "loadstring");
    if (!push_caller_environment(stack)) {
        luaL_error(stack, "loadstring loads into its caller's environment, and has no Luau caller");
    }
    lua_setsafeenv(stack, -1, 0);
    lua_CompileOptions options = state == nullptr ? lua_CompileOptions{} : state->compile_options;
    size_t size = 0;
    const std::unique_ptr<char, decltype(&std::free)> bytecode(
        luau_compile(source, length, &options, &size), &std::free);
    if (bytecode == nullptr) {
        luaL_error(stack, "not enough memory to compile");
    }
    // A compile can't be interrupted, so the budget's looked at once it's done.
    fbl::check_deadline(stack);
    lua_State* loader = lua_newthread(stack);
    lua_pushvalue(stack, 3);
    lua_xmove(stack, loader, 1);
    lua_replace(loader, LUA_GLOBALSINDEX);
    const int failed = luau_load(loader, name.c_str(), bytecode.get(), size, 0);
    lua_xmove(loader, stack, 1);
    if (failed == 0) {
        if (state != nullptr) {
            fbl::native_compile(*const_cast<fbl::state_t*>(state), stack, -1);
        }
        return 1;
    }
    lua_pushnil(stack);
    lua_insert(stack, -2);
    return 2;
}

// The function getfenv or setfenv names by its first argument, pushed: a function given, or the
// one at a level from 1; a thread's globals, level 0, are refused, as their table is the shared
// one, or a runner's, which every call reuses.
void environment_function(lua_State* stack, bool optional) {
    if (lua_type(stack, 1) == LUA_TFUNCTION) {
        lua_pushvalue(stack, 1);
    } else {
        const int level = optional ? luaL_optinteger(stack, 1, 1) : luaL_checkinteger(stack, 1);
        luaL_argcheck(stack, level > 0, 1,
                      "a thread's globals can't be reached; give a level from 1");
        lua_Debug info{};
        if (lua_getinfo(stack, level, "f", &info) == 0) {
            luaL_argerror(stack, 1, "invalid level");
        }
        if (lua_type(stack, -1) == LUA_TNIL) {
            luaL_error(stack, "no function environment for tail call at level %d", level);
        }
    }
    // A C function's environment is its creator's globals, which aren't a script's to reach.
    if (lua_iscfunction(stack, -1) != 0) {
        luaL_error(stack, "a C function's environment can't be reached");
    }
    // A function given, or the one at a level, is one of the caller's own environment, as another
    // script's function, a module's, or one that called a shared module's function that called
    // the caller, would give that environment away.
    {
        lua_getfenv(stack, -1);
        if (!push_caller_environment(stack) || lua_rawequal(stack, -1, -2) == 0) {
            luaL_error(stack, "a function of another environment than the caller's can't be "
                              "reached");
        }
        lua_pop(stack, 2);
    }
}

// getfenv, where the options turn it on: a Luau function's environment, marked unsafe for Luau's
// fast paths, as Luau's own does.
int get_environment(lua_State* stack) {
    environment_function(stack, true);
    lua_getfenv(stack, -1);
    lua_setsafeenv(stack, -1, 0);
    return 1;
}

// setfenv, where the options turn it on: sets a Luau function's environment, and gives the
// function, as Luau's own does.
int set_environment(lua_State* stack) {
    luaL_checktype(stack, 2, LUA_TTABLE);
    environment_function(stack, false);
    lua_pushvalue(stack, 2);
    lua_setsafeenv(stack, -1, 0);
    if (lua_setfenv(stack, -2) == 0) {
        luaL_error(stack, "'setfenv' cannot change environment of given object");
    }
    return 1;
}

} // namespace

namespace fbl {

void check_deadline(lua_State* stack) {
    auto* state = static_cast<fbl::state_t*>(lua_callbacks(stack)->userdata);
    if (state == nullptr || state->deadline == std::chrono::steady_clock::time_point{} ||
        std::chrono::steady_clock::now() < state->deadline) {
        return;
    }
    state->timed_out = true;
    luaL_error(stack, "the call went past its time budget of %lld microseconds",
               static_cast<long long>(state->time_budget.count()));
}

} // namespace fbl

namespace {

// The interrupt, at Luau's safe points: past the running call's deadline, it raises the timeout,
// and again at each one after, so a pcall can't keep the script going, until the call returns.
void interrupt(lua_State* stack, int gc) {
    if (gc >= 0) {
        return;
    }
    // Every Luau call is a safe point, so a script nesting C calls is stopped by its native stack
    // before the thread's runs out, which would end the process.
    const auto* state = static_cast<const fbl::state_t*>(lua_callbacks(stack)->userdata);
    if (state != nullptr && state->stack_floor != 0) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the stack's place, as a
        // number.
        const auto here = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
        if (here < state->stack_floor) {
            luaL_error(stack, "stack overflow: the script's calls nest deeper than the program's "
                              "native stack holds");
        }
    }
    fbl::check_deadline(stack);
}

// What debug.info and debug.traceback are: Luau's own, its upvalue, refusing a thread argument
// that's a runner, of a call of the program's, or a module's, unless it's the caller's own, as a
// script that kept one would read another script's frames on it. A level of the caller's own is
// one more to Luau's, past this function.
int guarded_debug(lua_State* stack) {
    const bool traceback = lua_toboolean(stack, lua_upvalueindex(2)) != 0;
    const bool threaded = lua_type(stack, 1) == LUA_TTHREAD;
    lua_State* target = threaded ? lua_tothread(stack, 1) : stack;
    if (target != stack && fbl::is_runner(target)) {
        luaL_error(stack, "another call's thread can't be read");
    }
    // The level: info's is its first argument after a thread, and traceback's its second, after
    // the message, 1 when it's left out. One given as a numeric string, which Luau takes, is made
    // the integer, so it's checked and shifted as one.
    const int first = threaded ? 2 : 1;
    const int index = traceback ? first + 1 : first;
    if (lua_type(stack, index) == LUA_TSTRING && lua_isnumber(stack, index) != 0) {
        lua_pushinteger(stack, lua_tointeger(stack, index));
        lua_replace(stack, index);
    }
    const bool own = target == stack;
    // info's function, at a level of any thread's, has to be of the caller's environment, as
    // getfenv's does: a level up from a shared module's function reaches whatever script called
    // it, and another script's thread, kept, has that script's functions on it.
    if (!traceback && lua_type(stack, index) == LUA_TNUMBER &&
        lua_tointeger(stack, index) >= (own ? 1 : 0)) {
        const char* options = lua_tostring(stack, index + 1);
        lua_Debug info{};
        if (options != nullptr && std::strchr(options, 'f') != nullptr &&
            lua_checkstack(target, 1) != 0 &&
            lua_getinfo(target, lua_tointeger(stack, index), "f", &info) != 0) {
            if (!own) {
                lua_xmove(target, stack, 1);
            }
            if (lua_type(stack, -1) == LUA_TFUNCTION && lua_iscfunction(stack, -1) == 0) {
                lua_getfenv(stack, -1);
                if (!push_caller_environment(stack) || lua_rawequal(stack, -1, -2) == 0) {
                    luaL_error(stack, "a function of another environment than the caller's "
                                      "can't be reached");
                }
                lua_pop(stack, 2);
            }
            lua_pop(stack, 1);
        }
    }
    if (own) {
        // A negative level stays as it is, which Luau refuses, as it does its own.
        if (lua_type(stack, index) == LUA_TNUMBER && lua_tointeger(stack, index) >= 0 &&
            lua_tointeger(stack, index) < std::numeric_limits<int>::max()) {
            lua_pushinteger(stack, lua_tointeger(stack, index) + 1);
            lua_replace(stack, index);
        } else if (traceback && lua_type(stack, index) == LUA_TNONE) {
            lua_settop(stack, index - 1);
            lua_pushinteger(stack, 2);
        }
    }
    lua_pushvalue(stack, lua_upvalueindex(1));
    lua_insert(stack, 1);
    lua_call(stack, lua_gettop(stack) - 1, LUA_MULTRET);
    return lua_gettop(stack);
}

// Puts guarded_debug in place of debug.info and debug.traceback, when the State has the library.
void guard_debug(lua_State* stack) {
    lua_getglobal(stack, LUA_DBLIBNAME);
    if (lua_type(stack, -1) != LUA_TTABLE) {
        lua_pop(stack, 1);
        return;
    }
    for (const char* name : { "info", "traceback" }) {
        lua_getfield(stack, -1, name);
        lua_pushboolean(stack, std::string_view(name) == "traceback" ? 1 : 0);
        lua_pushcclosurek(stack, guarded_debug, name, 2, nullptr);
        lua_setfield(stack, -2, name);
    }
    lua_pop(stack, 1);
}

// Opens one of Luau's libraries, `name` by `function`, when the State's options have its bit.
void open(lua_State* stack, const fbl::state_t& state, int32_t bit, const char* name,
          lua_CFunction function) {
    if ((state.libraries & static_cast<uint32_t>(bit)) == 0U) {
        return;
    }
    lua_pushcfunction(stack, function, nullptr);
    lua_pushstring(stack, name);
    lua_call(stack, 1, 0);
}

// What a State is set up with, under lua_cpcall: Luau's libraries its options say, and the
// function protect() runs work in.
int set_up(lua_State* stack) {
    auto* state = static_cast<fbl::state_t*>(lua_touserdata(stack, 1));
    open(stack, *state, FBL_LIBRARY_BASE, "", luaopen_base);
    open(stack, *state, FBL_LIBRARY_COROUTINE, LUA_COLIBNAME, luaopen_coroutine);
    open(stack, *state, FBL_LIBRARY_TABLE, LUA_TABLIBNAME, luaopen_table);
    open(stack, *state, FBL_LIBRARY_OS, LUA_OSLIBNAME, luaopen_os);
    open(stack, *state, FBL_LIBRARY_STRING, LUA_STRLIBNAME, luaopen_string);
    open(stack, *state, FBL_LIBRARY_MATH, LUA_MATHLIBNAME, luaopen_math);
    open(stack, *state, FBL_LIBRARY_DEBUG, LUA_DBLIBNAME, luaopen_debug);
    guard_debug(stack);
    open(stack, *state, FBL_LIBRARY_UTF8, LUA_UTF8LIBNAME, luaopen_utf8);
    open(stack, *state, FBL_LIBRARY_BIT32, LUA_BITLIBNAME, luaopen_bit32);
    open(stack, *state, FBL_LIBRARY_BUFFER, LUA_BUFFERLIBNAME, luaopen_buffer);
    open(stack, *state, FBL_LIBRARY_VECTOR, LUA_VECLIBNAME, luaopen_vector);
    open(stack, *state, FBL_LIBRARY_INTEGER, LUA_INTLIBNAME, luaopen_integer);
    if ((state->libraries & static_cast<uint32_t>(FBL_LIBRARY_BASE)) != 0U) {
        if (state->environments) {
            lua_pushcfunction(stack, get_environment, "getfenv");
        } else {
            lua_pushnil(stack);
        }
        lua_setglobal(stack, "getfenv");
        if (state->environments) {
            lua_pushcfunction(stack, set_environment, "setfenv");
        } else {
            lua_pushnil(stack);
        }
        lua_setglobal(stack, "setfenv");
    }
    if (state->collect_garbage != FBL_COLLECT_NONE) {
        lua_pushcfunction(stack, collect_garbage, "collectgarbage");
        lua_setglobal(stack, "collectgarbage");
    }
    if (state->load_string) {
        lua_pushcfunction(stack, load_string, "loadstring");
        lua_setglobal(stack, "loadstring");
    }
    lua_callbacks(stack)->interrupt = interrupt;
    lua_pushcfunction(stack, run_work, "fbl");
    state->protect_ref = lua_ref(stack, -1);
    lua_pop(stack, 1);
    // A slot of the registry's for the last error's value, which only ever changes in place.
    lua_pushboolean(stack, 0);
    state->error_ref = lua_ref(stack, -1);
    lua_pop(stack, 1);
    fbl::prepare_callbacks(stack);
    fbl::prepare_objects(stack, *state);
    // Each thread's number, by the thread, which Luau's collector clears.
    lua_newtable(stack);
    lua_newtable(stack);
    lua_pushstring(stack, "k");
    lua_setfield(stack, -2, "__mode");
    lua_setmetatable(stack, -2);
    state->thread_ids_ref = lua_ref(stack, -1);
    lua_pop(stack, 1);
    return 0;
}

// The text a Luau error value says: a string's, a number's, or what kind of value it is.
std::string text_of(lua_State* stack, int index) {
    switch (lua_type(stack, index)) {
    case LUA_TSTRING: {
        size_t length = 0;
        const char* text = lua_tolstring(stack, index, &length);
        return { text, length };
    }
    case LUA_TNUMBER:
        return fbl::formatted("{}", lua_tonumber(stack, index));
    default:
        return fbl::formatted("(an error that's a {} value)", luaL_typename(stack, index));
    }
}

void close_state(fbl::state_t* state) {
    if (state->main != nullptr) {
        lua_close(state->main);
        state->main = nullptr;
    }
    delete state;
}

} // namespace

namespace fbl {

entered_t::entered_t(state_t& state) noexcept : m_state(state), m_outermost(state.depth == 0) {
    if (m_outermost) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the stack's place, as a
        // number.
        const auto base = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
        uintptr_t floor = fbl::stack_floor(native_stack_reserve);
        if (floor == 0 || floor >= base) {
            floor = base > most_native_stack ? base - most_native_stack : 1;
        }
        m_state.stack_floor = floor;
        m_state.timed_out = false;
        if (m_state.time_budget.count() > 0) {
            // now + budget, or the clock's last time point when that's past it.
            const auto now = std::chrono::steady_clock::now();
            const auto left = std::chrono::steady_clock::time_point::max() - now;
            const auto budget = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::min(m_state.time_budget,
                         std::chrono::duration_cast<std::chrono::microseconds>(left)));
            m_state.deadline = now + budget;
        }
    }
    m_state.depth++;
}

entered_t::~entered_t() {
    m_state.depth--;
    if (m_outermost) {
        m_state.deadline = {};
        m_state.stack_floor = 0;
    }
}

void set_generation(uint64_t handle, uint32_t generation) noexcept {
    const std::scoped_lock lock(slots_mutex);
    const auto slot = static_cast<uint32_t>(handle >> 32U);
    if (slot >= slots.size() || slots[slot].state == nullptr) {
        return;
    }
    slots[slot].generation = generation;
    slots[slot].state->handle = handle_of(slot, generation);
}

void native_compile(state_t& state, lua_State* stack, int index) noexcept {
    if (!state.code_generator) {
        return;
    }
    try {
        Luau::CodeGen::CompilationOptions options;
        options.flags = state.native_code == FBL_NATIVE_MARKED
                            ? static_cast<unsigned int>(Luau::CodeGen::CodeGen_OnlyNativeModules)
                            : static_cast<unsigned int>(Luau::CodeGen::CodeGen_ColdFunctions);
        Luau::CodeGen::CompilationStats stats;
        Luau::CodeGen::compile(stack, index, options, &stats);
        state.native_functions += stats.functionsCompiled;
    } catch (...) {
        // What the generator couldn't compile runs in the interpreter; nothing's counted.
        return;
    }
}

void failure_point(lua_State* stack, const state_t& state) {
    if (state.failing_point < 0) {
        return;
    }
    const bool fails = state.failing_point == 0;
    state.failing_point--;
    if (fails) {
        luaL_error(stack, "not enough memory");
    }
}

void make_room(std::vector<int64_t>& queue, int64_t live) {
    queue.reserve(queue.size() + static_cast<size_t>(live) + 1);
}

// NOLINTNEXTLINE(bugprone-exception-escape): push_back within the capacity doesn't allocate.
void keep_released(std::vector<int64_t>& queue, int64_t number) noexcept {
    assert(queue.size() < queue.capacity() && "no room kept for a collected number");
    if (queue.size() < queue.capacity()) {
        queue.push_back(number);
    }
}

int64_t memory_of_all_states() noexcept {
    return all_memory.load();
}

state_t* find_state(uint64_t handle) noexcept {
    if ((handle & 0xFFU) != state_kind) {
        misuse(formatted("{:#x} isn't a State's handle", handle));
        return nullptr;
    }
    const auto slot = static_cast<uint32_t>(handle >> 32U);
    state_t* state = nullptr;
    bool owned = false;
    {
        // Its owner is read under the lock, as the owner can close it, and free it, meanwhile.
        const std::scoped_lock lock(slots_mutex);
        if (slot < slots.size() && slots[slot].state != nullptr &&
            handle_of(slot, slots[slot].generation) == handle) {
            state = slots[slot].state;
            owned = slots[slot].owner == std::this_thread::get_id();
        }
    }
    if (state == nullptr) {
        misuse("the State is closed");
        return nullptr;
    }
    if (!owned) {
        misuse("the State is another thread's; only the thread that made it uses it");
        return nullptr;
    }
    return state;
}

frame_t* find_frame(state_t& state, int32_t frame) noexcept {
    if (frame < 0 || std::cmp_greater_equal(frame, state.frames.size())) {
        misuse(formatted("the State has no frame {}", frame));
        return nullptr;
    }
    if (std::cmp_not_equal(frame, state.frames.size() - 1)) {
        misuse(formatted("frame {} isn't the State's newest, which only can be used", frame));
        return nullptr;
    }
    return &state.frames[static_cast<size_t>(frame)];
}

int slot_index(const frame_t& frame, int32_t slot) noexcept {
    const int count = lua_gettop(frame.thread) - frame.base;
    if (slot < 0 || slot >= count) {
        misuse(formatted("the frame has no slot {}; it has {}", slot, count));
        return 0;
    }
    return frame.base + 1 + slot;
}

int32_t misuse(std::string_view why) noexcept {
    fail(why);
    return FBL_MISUSE;
}

int32_t record_error(state_t& state, int32_t kind, std::string_view message,
                     std::string_view traceback) noexcept {
    state.error_kind = kind;
    collect_after(state, kind);
    // An error that isn't a value of Luau's has none. The slot holds false then, never nil,
    // which would make it a border lua_ref could give another, and a dead key a set could need
    // memory for.
    state.error_has_value = false;
    if (state.error_ref != LUA_NOREF && lua_checkstack(state.main, 1) != 0) {
        lua_pushboolean(state.main, 0);
        lua_rawseti(state.main, LUA_REGISTRYINDEX, state.error_ref);
    }
    try {
        state.error_message = message;
        state.error_traceback = traceback;
    } catch (const std::exception&) {
        // What can't be kept leaves what's there, which says less.
        state.error_message.clear();
        state.error_traceback.clear();
    }
    return kind;
}

int32_t kind_of(int status) noexcept {
    switch (status) {
    case LUA_OK:
        return FBL_OK;
    case LUA_YIELD:
        return FBL_YIELD;
    case LUA_ERRMEM:
        return FBL_MEMORY;
    case LUA_ERRRUN:
    case LUA_ERRERR:
        return FBL_RUNTIME;
    default:
        return FBL_OTHER;
    }
}

int protect_erased(state_t& state, lua_State* thread, int arguments, int results,
                   work_function_t function, void* data) noexcept {
    // Room for the function and what it gives, which lua_checkstack makes without raising, or
    // says it couldn't.
    if (lua_checkstack(thread, 1 + results) == 0) {
        return no_room;
    }
    lua_rawgeti(thread, LUA_REGISTRYINDEX, state.protect_ref);
    lua_insert(thread, -(arguments + 1));
    // Pending until the function takes it; what was pending, for protect() in protect(), after.
    const work_t before = state.pending_work;
    state.pending_work = { .function = function, .data = data };
    // The work can run a script's code, a metamethod's, which the time budget holds to as it
    // does a call's when this is the outermost.
    const entered_t entered(state);
    const int status = lua_pcall(thread, arguments, results, 0);
    state.pending_work = before;
    return status;
}

int32_t settle(state_t& state, lua_State* thread, int status) noexcept {
    if (status == LUA_OK) {
        return FBL_OK;
    }
    if (status == no_room) {
        return record_error(state, FBL_MEMORY, "not enough memory");
    }
    // An error the interrupt raised, or any the timeout led to, is the timeout.
    const int32_t kind = state.timed_out && status == LUA_ERRRUN ? FBL_TIMEOUT : kind_of(status);
    return take_error(state, thread, kind);
}

void collect_after(const state_t& state, int32_t kind) noexcept {
    // Past the limit, the collector stops, and every allocation fails until it runs: an outermost
    // call that met the limit collects all of it before it returns.
    if (kind == FBL_MEMORY && state.depth == 0 && state.memory_limit > 0) {
        lua_gc(state.main, LUA_GCCOLLECT, 0);
    }
}

int32_t take_error(state_t& state, lua_State* thread, int32_t kind,
                   std::string_view traceback) noexcept {
    try {
        record_error(state, kind, text_of(thread, -1), traceback);
    } catch (const std::exception&) {
        record_error(state, kind, "", traceback);
    }
    // The value itself, in the registry's slot for it, which the slot's being there already lets
    // change without memory; error(nil)'s is none, as false is kept for it.
    if (lua_isnil(thread, -1)) {
        lua_pop(thread, 1);
        lua_pushboolean(thread, 0);
    } else {
        state.error_has_value = true;
    }
    lua_rawseti(thread, LUA_REGISTRYINDEX, state.error_ref);
    return kind;
}

} // namespace fbl

uint64_t fbl_state_new(const fbl_state_options_t* options, int32_t* status) try {
    int32_t ignored = FBL_OK;
    int32_t& said = status != nullptr ? *status : ignored;
    said = FBL_MISUSE;
    try {
        if (options == nullptr) {
            fbl::fail("a State needs its options");
            return 0;
        }
        if (options->time_budget < 0) {
            fbl::fail(fbl::formatted("a time budget of {} microseconds is below 0",
                                     options->time_budget));
            return 0;
        }
        if (options->native_code < FBL_NATIVE_OFF || options->native_code > FBL_NATIVE_ALL) {
            fbl::fail(
                fbl::formatted("{} isn't what native code a State has", options->native_code));
            return 0;
        }
        if (options->collect_garbage < FBL_COLLECT_NONE ||
            options->collect_garbage > FBL_COLLECT_FULL) {
            fbl::fail(fbl::formatted("{} isn't a collectgarbage", options->collect_garbage));
            return 0;
        }
        if ((static_cast<uint32_t>(options->libraries) & ~0xFFFU) != 0U) {
            fbl::fail(fbl::formatted("{:#x} has bits that aren't libraries", options->libraries));
            return 0;
        }
        if (options->memory_limit < 0) {
            fbl::fail(std::format("a memory limit of {} bytes is below 0", options->memory_limit));
            return 0;
        }
        if (options->native_memory < 0) {
            fbl::fail(
                std::format("a native code cap of {} bytes is below 0", options->native_memory));
            return 0;
        }
        for (const int32_t level : { options->optimization, options->debug, options->coverage }) {
            if (level < 0 || level > 2) {
                fbl::fail(std::format("a compile option of {} isn't from 0 to 2", level));
                return 0;
            }
        }
        fbl::set_flags();
        std::unique_ptr<fbl::state_t, decltype(&close_state)> state(new (std::nothrow) fbl::state_t,
                                                                    &close_state);
        if (state == nullptr) {
            said = FBL_MEMORY;
            fbl::fail("not enough memory for a State");
            return 0;
        }
        state->owner = std::this_thread::get_id();
        state->memory_limit = options->memory_limit;
        state->time_budget = std::chrono::microseconds(options->time_budget);
        state->libraries = static_cast<uint32_t>(options->libraries);
        state->collect_garbage = options->collect_garbage;
        state->native_code = options->native_code;
        state->load_string = options->load_string;
        state->environments = options->environments;
        state->compile_options.optimizationLevel = options->optimization;
        state->compile_options.debugLevel = options->debug;
        state->compile_options.coverageLevel = options->coverage;
        state->main = lua_newstate(allocate, state.get());
        if (state->main != nullptr) {
            lua_callbacks(state->main)->userdata = state.get();
        }
        if (state->main == nullptr ||
            lua_cpcall(state->main, set_up, static_cast<void*>(state.get())) != LUA_OK) {
            said = FBL_MEMORY;
            fbl::fail(std::format("not enough memory for a State within its limit of {} bytes",
                                  options->memory_limit));
            return 0;
        }
        state->current = state->main;
        // The code generator is made before any code loads, as Luau needs, with a small block and
        // the State's cap, rather than Luau's 4 MB committed a State and 256 MB in all; its
        // memory is counted as it's taken. It runs only where Luau could get executable memory,
        // which a hardened runtime or SELinux can refuse.
        if (state->native_code != FBL_NATIVE_OFF && Luau::CodeGen::isSupported()) {
            const auto cap = static_cast<size_t>(
                options->native_memory > 0 ? options->native_memory : int64_t{ 256 } * 1024 * 1024);
            const size_t block = std::min<size_t>(cap, size_t{ 256 } * 1024);
            Luau::CodeGen::create(state->main, block, cap, count_native_memory, state.get());
            state->code_generator = Luau::CodeGen::isNativeExecutionEnabled(state->main);
        }
        // Luau collects toward a heap of twice what's live, and never because an allocation
        // failed, so with a limit, garbage alone would reach it once what's live passed half of
        // it: the collector aims at a quarter more than what's live, and works twice as hard.
        if (state->memory_limit > 0) {
            lua_gc(state->main, LUA_GCSETGOAL, 125);
            lua_gc(state->main, LUA_GCSETSTEPMUL, 400);
        }
        try {
            const std::scoped_lock lock(slots_mutex);
            // Room first, so nothing's taken when it can't be had: a new slot, and a free one for
            // each, so closing it never needs memory to free its slot.
            slots.reserve(slots.size() + 1);
            free_slots.reserve(slots.size() + 1);
            uint32_t slot = 0;
            if (free_slots.empty()) {
                slot = static_cast<uint32_t>(slots.size());
                slots.emplace_back();
            } else {
                slot = free_slots.back();
                free_slots.pop_back();
            }
            slots[slot].generation++;
            slots[slot].state = state.get();
            slots[slot].owner = state->owner;
            state->handle = handle_of(slot, slots[slot].generation);
        } catch (const std::exception&) {
            said = FBL_MEMORY;
            fbl::fail("not enough memory for a State");
            return 0;
        }
        said = FBL_OK;
        return state.release()->handle;
    } catch (const std::exception&) {
        said = FBL_MEMORY;
        fbl::fail("not enough memory for a State");
        return 0;
    }
} catch (...) {
    fbl::escaped();
    return 0;
}

bool fbl_state_close(uint64_t state) try {
    fbl::state_t* found = fbl::find_state(state);
    if (found == nullptr) {
        return false;
    }
    // A frame the program left open goes with it; a call running, or a function of the program's
    // that Luau called, doesn't.
    if (found->depth > 0 || found->current != found->main) {
        fbl::fail("a State can't be closed while it runs a call");
        return false;
    }
    found->frames.clear();
    {
        const std::scoped_lock lock(slots_mutex);
        const auto slot = static_cast<uint32_t>(state >> 32U);
        slots[slot].state = nullptr;
        // Its slot is free for another State, whose handle its generation tells apart, unless its
        // generations are spent; there's room for it, which making the State reserved.
        if (slots[slot].generation < most_generations) {
            free_slots.push_back(slot);
        }
    }
    close_state(found);
    return true;
} catch (...) {
    fbl::escaped();
    return false;
}

int32_t fbl_state_step(uint64_t state, int32_t kilobytes) try {
    const fbl::state_t* found = fbl::find_state(state);
    if (found == nullptr) {
        return -1;
    }
    if (kilobytes < 0) {
        fbl::fail(fbl::formatted("a step of {} kilobytes isn't one", kilobytes));
        return -1;
    }
    return lua_gc(found->main, LUA_GCSTEP, kilobytes) != 0 ? 1 : 0;
} catch (...) {
    fbl::escaped();
    return -1;
}

int32_t fbl_state_native_supported(void) try {
    return Luau::CodeGen::isSupported() ? 1 : 0;
} catch (...) {
    fbl::escaped();
    return 0;
}

int64_t fbl_state_native_memory(uint64_t state) try {
    const fbl::state_t* found = fbl::find_state(state);
    return found == nullptr ? -1 : found->native_memory_used;
} catch (...) {
    fbl::escaped();
    return -1;
}

int64_t fbl_state_native_functions(uint64_t state) try {
    const fbl::state_t* found = fbl::find_state(state);
    return found == nullptr ? -1 : found->native_functions;
} catch (...) {
    fbl::escaped();
    return -1;
}

int32_t fbl_state_turn(uint64_t state, int32_t what) try {
    fbl::state_t* found = fbl::find_state(state);
    if (found == nullptr) {
        return what == 2 ? -1 : FBL_MISUSE;
    }
    switch (what) {
    case 1:
        if (found->depth != 0 || found->turn_open) {
            return fbl::misuse("a turn begins only when no call runs");
        }
        found->turn_open = true;
        found->timed_out = false;
        if (found->time_budget.count() > 0) {
            found->deadline = std::chrono::steady_clock::now() + found->time_budget;
        }
        found->depth++;
        return FBL_OK;
    case 0:
        if (!found->turn_open) {
            return fbl::misuse("no turn is open");
        }
        found->turn_open = false;
        found->depth--;
        found->deadline = {};
        return FBL_OK;
    case 3:
        return found->depth != 0 ? 1 : 0;
    case 2:
        return found->turn_open && found->deadline != std::chrono::steady_clock::time_point{} &&
                       std::chrono::steady_clock::now() >= found->deadline
                   ? 1
                   : 0;
    default:
        return fbl::misuse(fbl::formatted("{} isn't a turn's step", what));
    }
} catch (...) {
    return fbl::escaped();
}

int64_t fbl_state_count(int32_t here) try {
    const std::thread::id caller = std::this_thread::get_id();
    const std::scoped_lock lock(slots_mutex);
    return std::ranges::count_if(slots, [&](const slot_t& slot) {
        return slot.state != nullptr && (here == 0 || slot.owner == caller);
    });
} catch (...) {
    fbl::escaped();
    return -1;
}

int64_t fbl_state_memory_all(int32_t here) try {
    if (here == 0) {
        return fbl::memory_of_all_states();
    }
    // Only this thread changes what its States hold, so reading it here is safe.
    const std::thread::id caller = std::this_thread::get_id();
    const std::scoped_lock lock(slots_mutex);
    int64_t total = 0;
    for (const slot_t& slot : slots) {
        if (slot.state != nullptr && slot.owner == caller) {
            total += slot.state->memory_used;
        }
    }
    return total;
} catch (...) {
    fbl::escaped();
    return -1;
}

int64_t fbl_state_memory(uint64_t state) try {
    const fbl::state_t* found = fbl::find_state(state);
    return found == nullptr ? -1 : found->memory_used;
} catch (...) {
    fbl::escaped();
    return -1;
}

int32_t fbl_state_collect(uint64_t state) try {
    const fbl::state_t* found = fbl::find_state(state);
    if (found == nullptr) {
        return FBL_MISUSE;
    }
    lua_gc(found->main, LUA_GCCOLLECT, 0);
    return FBL_OK;
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_state_error_kind(uint64_t state) try {
    const fbl::state_t* found = fbl::find_state(state);
    return found == nullptr ? FBL_MISUSE : found->error_kind;
} catch (...) {
    return fbl::escaped();
}

const uint8_t* fbl_state_error_message(uint64_t state, int64_t* length) try {
    const fbl::state_t* found = fbl::find_state(state);
    if (found == nullptr || length == nullptr) {
        return nullptr;
    }
    *length = static_cast<int64_t>(found->error_message.size());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): its bytes, as C takes them.
    return reinterpret_cast<const uint8_t*>(found->error_message.data());
} catch (...) {
    fbl::escaped();
    return nullptr;
}

int32_t fbl_state_error_value(uint64_t state, int32_t frame) try {
    fbl::state_t* found = fbl::find_state(state);
    const fbl::frame_t* in = found == nullptr ? nullptr : fbl::find_frame(*found, frame);
    if (in == nullptr) {
        return FBL_MISUSE;
    }
    if (lua_checkstack(in->thread, 1) == 0) {
        return fbl::record_error(*found, FBL_MEMORY, "not enough memory");
    }
    if (found->error_has_value) {
        lua_getref(in->thread, found->error_ref);
    } else {
        lua_pushnil(in->thread);
    }
    return FBL_OK;
} catch (...) {
    return fbl::escaped();
}

const uint8_t* fbl_state_error_traceback(uint64_t state, int64_t* length) try {
    const fbl::state_t* found = fbl::find_state(state);
    if (found == nullptr || length == nullptr) {
        return nullptr;
    }
    *length = static_cast<int64_t>(found->error_traceback.size());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): its bytes, as C takes them.
    return reinterpret_cast<const uint8_t*>(found->error_traceback.data());
} catch (...) {
    fbl::escaped();
    return nullptr;
}
