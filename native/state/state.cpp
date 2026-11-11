#include "state/state.hpp"

#include "core/core.hpp"
#include "fbl.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <format>
#include <lualib.h>
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
};

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

// What protect() calls: the work its light userdata names.
struct work_t {
    fbl::work_function_t function;
    void* data;
};

int run_work(lua_State* stack) {
    const auto* work = static_cast<const work_t*>(lua_touserdata(stack, 1));
    lua_remove(stack, 1);
    try {
        return work->function(stack, work->data);
    } catch (const std::exception&) {
        // Luau takes these as errors itself.
        throw;
    } catch (...) {
        luaL_error(stack, "a C++ exception that isn't a std::exception");
    }
}

// What a State is set up with, under lua_cpcall: Luau's libraries, and the function protect()
// runs work in.
int set_up(lua_State* stack) {
    auto* state = static_cast<fbl::state_t*>(lua_touserdata(stack, 1));
    luaL_openlibs(stack);
    lua_pushcfunction(stack, run_work, "fbl");
    state->protect_ref = lua_ref(stack, -1);
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
    {
        const std::scoped_lock lock(slots_mutex);
        if (slot < slots.size() && slots[slot].state != nullptr &&
            handle_of(slot, slots[slot].generation) == handle) {
            state = slots[slot].state;
        }
    }
    if (state == nullptr) {
        misuse("the State is closed");
        return nullptr;
    }
    if (state->owner != std::this_thread::get_id()) {
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

int protect_erased(const state_t& state, lua_State* thread, int arguments, int results,
                   work_function_t function, void* data) noexcept {
    // Room for the function, its own argument, and what it gives, which lua_checkstack makes
    // without raising, or says it couldn't.
    if (lua_checkstack(thread, 2 + results) == 0) {
        return no_room;
    }
    work_t work{ .function = function, .data = data };
    lua_rawgeti(thread, LUA_REGISTRYINDEX, state.protect_ref);
    lua_insert(thread, -(arguments + 1));
    lua_pushlightuserdata(thread, &work);
    lua_insert(thread, -(arguments + 1));
    return lua_pcall(thread, arguments + 1, results, 0);
}

int32_t settle(state_t& state, lua_State* thread, int status) noexcept {
    if (status == LUA_OK) {
        return FBL_OK;
    }
    if (status == no_room) {
        return record_error(state, FBL_MEMORY, "not enough memory");
    }
    return take_error(state, thread, kind_of(status));
}

int32_t take_error(state_t& state, lua_State* thread, int32_t kind,
                   std::string_view traceback) noexcept {
    try {
        record_error(state, kind, text_of(thread, -1), traceback);
    } catch (const std::exception&) {
        record_error(state, kind, "", traceback);
    }
    lua_pop(thread, 1);
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
        if (options->memory_limit < 0) {
            fbl::fail(std::format("a memory limit of {} bytes is below 0", options->memory_limit));
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
        state->compile_options.optimizationLevel = options->optimization;
        state->compile_options.debugLevel = options->debug;
        state->compile_options.coverageLevel = options->coverage;
        state->main = lua_newstate(allocate, state.get());
        if (state->main == nullptr ||
            lua_cpcall(state->main, set_up, static_cast<void*>(state.get())) != LUA_OK) {
            said = FBL_MEMORY;
            fbl::fail(std::format("not enough memory for a State within its limit of {} bytes",
                                  options->memory_limit));
            return 0;
        }
        lua_callbacks(state->main)->userdata = state.get();
        try {
            const std::scoped_lock lock(slots_mutex);
            uint32_t slot = 0;
            if (free_slots.empty()) {
                slot = static_cast<uint32_t>(slots.size());
                slots.emplace_back();
            } else {
                slot = free_slots.back();
                free_slots.pop_back();
            }
            // So closing it never needs memory to free its slot.
            free_slots.reserve(slots.size());
            slots[slot].generation++;
            slots[slot].state = state.get();
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
    if (found->depth > 0 || !found->frames.empty()) {
        fbl::fail("a State can't be closed while it runs a call");
        return false;
    }
    {
        const std::scoped_lock lock(slots_mutex);
        const auto slot = static_cast<uint32_t>(state >> 32U);
        slots[slot].state = nullptr;
        // Its slot is free for another State, whose handle its generation tells apart; there's
        // room for it, which making the State reserved.
        free_slots.push_back(slot);
    }
    close_state(found);
    return true;
} catch (...) {
    fbl::escaped();
    return false;
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
