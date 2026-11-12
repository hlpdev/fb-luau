#include "core/core.hpp"
#include "fbl.h"
#include "require/paths.hpp"
#include "state/state.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <lua.h>
#include <luacode.h>
#include <lualib.h>
#include <Luau/Common.h>
#include <Luau/Require.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// NOLINTNEXTLINE(readability-identifier-naming): Luau names its flags.
LUAU_FASTFLAG(LuauCyclicRequireShortCircuit)

namespace {

// The module a chunk that isn't named for a path requires as: one at the root, so "./x" is
// the root's "x".
constexpr std::string_view root_module = "=";

fbl::state_t& state_of(void* context) {
    return *static_cast<fbl::state_t*>(context);
}

// Raises when the walk's path is past what a require's may be.
void check_length(lua_State* stack, const fbl::require_walk_t& walk) {
    if (fbl::is_too_long(walk.parts)) {
        luaL_error(stack, "a require's path is at most %d parts and %d bytes",
                   static_cast<int>(fbl::most_path_parts), static_cast<int>(fbl::most_path_bytes));
    }
}

// Keeps a walk while the reader runs, which can run a script's code that requires, and so walks
// on its own; puts it back however the reader ends.
class walk_kept_t {
public:
    explicit walk_kept_t(fbl::state_t& state) : m_state(state) {
        swapped();
    }
    ~walk_kept_t() {
        swapped();
    }
    walk_kept_t(const walk_kept_t&) = delete;
    walk_kept_t& operator=(const walk_kept_t&) = delete;
    walk_kept_t(walk_kept_t&&) = delete;
    walk_kept_t& operator=(walk_kept_t&&) = delete;

private:
    // Swaps the State's walk and the one kept, which needs no memory.
    void swapped() noexcept {
        m_state.require_walk.parts.swap(m_kept.parts);
        m_state.require_walk.file.swap(m_kept.file);
        m_state.require_walk.found.swap(m_kept.found);
    }

    fbl::state_t& m_state;
    fbl::require_walk_t m_kept;
};

// The file at `path`, as the reader gives it; one it found is kept for the walk, one it didn't
// isn't. It raises the reader's errors, and one for what isn't text.
std::optional<std::string> read_file(fbl::state_t& state, lua_State* stack,
                                     const std::string& path) {
    const auto found = state.require_walk.found.find(path);
    if (found != state.require_walk.found.end()) {
        return found->second;
    }
    luaL_checkstack(stack, 2, "reading a module");
    std::optional<std::string> text;
    {
        const walk_kept_t kept(state);
        lua_getref(stack, state.require_reader);
        lua_pushlstring(stack, path.data(), path.size());
        lua_call(stack, 1, 1);
        if (lua_type(stack, -1) == LUA_TSTRING) {
            size_t length = 0;
            const char* bytes = lua_tolstring(stack, -1, &length);
            text.emplace(bytes, length);
        } else if (lua_type(stack, -1) != LUA_TNIL) {
            luaL_error(stack, "the modules' reader gave a %s for '%s', not a string or nil",
                       luaL_typename(stack, -1), path.c_str());
        }
        lua_pop(stack, 1);
    }
    if (text) {
        state.require_walk.found.emplace(path, *text);
    }
    return text;
}

// Finds the module the walk is at: how many of its files there are, and the first's path in the
// walk's file, or "" for none. The time budget holds here, as the reader's calls take time.
int resolve(fbl::state_t& state, lua_State* stack) {
    fbl::check_deadline(stack);
    state.require_walk.file.clear();
    if (state.require_walk.parts.empty()) {
        return 0;
    }
    const std::string base = fbl::joined(state.require_walk.parts);
    int count = 0;
    for (const std::string_view suffix : fbl::module_suffixes) {
        std::string file = base + std::string(suffix);
        if (read_file(state, stack, file).has_value()) {
            if (count == 0) {
                state.require_walk.file = std::move(file);
            }
            count++;
        }
    }
    return count;
}

// Writes `text` into Luau.Require's buffer, as its write functions do.
luarequire_WriteResult write(std::string_view text, char* buffer, size_t size, size_t* size_out) {
    const size_t needed = text.size() + 1;
    *size_out = needed;
    if (size < needed) {
        return WRITE_BUFFER_TOO_SMALL;
    }
    std::memcpy(buffer, text.data(), text.size());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): Luau.Require's buffer.
    buffer[text.size()] = '\0';
    return WRITE_SUCCESS;
}

bool is_require_allowed(lua_State* stack, void* context, const char* /*requirer*/) {
    if (state_of(context).require_reader == LUA_NOREF) {
        luaL_error(stack, "require: the program gives no modules");
    }
    // The path, require's argument, is refused before anything's read when it's too long.
    size_t length = 0;
    const char* path = lua_tolstring(stack, 1, &length);
    if (path != nullptr) {
        const std::string_view given(path, length);
        const size_t parts = static_cast<size_t>(std::ranges::count(given, '/')) + 1;
        if (length > fbl::most_path_bytes || parts > fbl::most_path_parts) {
            luaL_error(stack, "a require's path is at most %d parts and %d bytes",
                       static_cast<int>(fbl::most_path_parts),
                       static_cast<int>(fbl::most_path_bytes));
        }
        // Luau's require reads it as C text, to its first NUL, which would hide the rest.
        if (given.contains('\0')) {
            luaL_error(stack, "a require's path can't hold a NUL");
        }
    }
    return true;
}

luarequire_NavigateResult reset(lua_State* /*stack*/, void* context, const char* requirer) {
    fbl::state_t& state = state_of(context);
    state.require_walk = {};
    const std::string_view name = requirer;
    if (!name.starts_with('@')) {
        // A chunk of another name, the program's own code's, or one loadstring made, whose name a
        // script picks, requires from the root, as a module there; its name is never a path.
        state.require_walk.parts = { std::string(root_module) };
        return NAVIGATE_SUCCESS;
    }
    // A path's parts are checked, as require's argument's are, before anything's read.
    if (name.size() > fbl::most_path_bytes) {
        return NAVIGATE_NOT_FOUND;
    }
    std::optional<std::vector<std::string>> parts = fbl::module_parts(name.substr(1));
    if (!parts || fbl::is_too_long(*parts)) {
        return NAVIGATE_NOT_FOUND;
    }
    state.require_walk.parts = std::move(*parts);
    return NAVIGATE_SUCCESS;
}

luarequire_NavigateResult jump_to_alias(lua_State* stack, void* context, const char* path) {
    fbl::state_t& state = state_of(context);
    // An alias's path that isn't relative to its .luaurc is from the root.
    const std::string_view given = path;
    if (!given.starts_with('/')) {
        return NAVIGATE_NOT_FOUND;
    }
    std::optional<std::vector<std::string>> parts = fbl::module_parts(given);
    if (!parts) {
        return NAVIGATE_NOT_FOUND;
    }
    state.require_walk.parts = std::move(*parts);
    check_length(stack, state.require_walk);
    return resolve(state, stack) > 1 ? NAVIGATE_AMBIGUOUS : NAVIGATE_SUCCESS;
}

luarequire_NavigateResult to_parent(lua_State* /*stack*/, void* context) {
    fbl::state_t& state = state_of(context);
    // Nothing is above the root: a module reaches only what the program gives.
    if (state.require_walk.parts.empty()) {
        return NAVIGATE_NOT_FOUND;
    }
    state.require_walk.parts.pop_back();
    return NAVIGATE_SUCCESS;
}

luarequire_NavigateResult to_child(lua_State* stack, void* context, const char* name) {
    fbl::state_t& state = state_of(context);
    const std::string_view child = name;
    if (child == "." || child == ".." || child == ".config" || child.contains('/')) {
        return NAVIGATE_NOT_FOUND;
    }
    if (!fbl::is_safe_part(child)) {
        luaL_error(stack, "a require's path can't have an empty part, a ':', a '\\', or a "
                          "device's name");
    }
    state.require_walk.parts.emplace_back(child);
    check_length(stack, state.require_walk);
    return resolve(state, stack) > 1 ? NAVIGATE_AMBIGUOUS : NAVIGATE_SUCCESS;
}

bool is_module_present(lua_State* stack, void* context) {
    fbl::state_t& state = state_of(context);
    return resolve(state, stack) > 0;
}

luarequire_WriteResult get_chunkname(lua_State* /*stack*/, void* context, char* buffer, size_t size,
                                     size_t* size_out) {
    return write("@" + state_of(context).require_walk.file, buffer, size, size_out);
}

luarequire_WriteResult get_loadname(lua_State* /*stack*/, void* context, char* buffer, size_t size,
                                    size_t* size_out) {
    return write(state_of(context).require_walk.file, buffer, size, size_out);
}

luarequire_WriteResult get_cache_key(lua_State* /*stack*/, void* context, char* buffer, size_t size,
                                     size_t* size_out) {
    return write(state_of(context).require_walk.file, buffer, size, size_out);
}

// Where the .luaurc of the place the walk is at would be.
std::string config_file(const fbl::state_t& state) {
    return fbl::config_file(state.require_walk.parts);
}

luarequire_ConfigStatus get_config_status(lua_State* stack, void* context) {
    fbl::state_t& state = state_of(context);
    // The search for a .luaurc climbs a folder at a time, which the budget holds to.
    fbl::check_deadline(stack);
    const std::string file = config_file(state);
    return read_file(state, stack, file).has_value() ? CONFIG_PRESENT_JSON : CONFIG_ABSENT;
}

luarequire_WriteResult get_config(lua_State* stack, void* context, char* buffer, size_t size,
                                  size_t* size_out) {
    fbl::state_t& state = state_of(context);
    const std::string file = config_file(state);
    const std::optional<std::string> text = read_file(state, stack, file);
    if (!text) {
        return WRITE_FAILURE;
    }
    return write(*text, buffer, size, size_out);
}

// A module loading: the State counts it, so its modules aren't forgotten meanwhile, and the
// walk's files go when it's done, however it ends.
class loading_t {
public:
    explicit loading_t(fbl::state_t& state) : m_state(state) {
        m_state.requires_running++;
    }
    ~loading_t() {
        m_state.requires_running--;
        m_state.require_walk.found.clear();
    }
    loading_t(const loading_t&) = delete;
    loading_t& operator=(const loading_t&) = delete;
    loading_t(loading_t&&) = delete;
    loading_t& operator=(loading_t&&) = delete;

private:
    fbl::state_t& m_state;
};

// Raises the error on top of the requiring thread's stack, first clearing the placeholder Luau's
// require made for the module `key`, when it made one, so the next require loads it anew.
[[noreturn]] void failed(lua_State* stack, bool placeholder, const char* key) {
    if (placeholder) {
        for (const char* table : { "_MODULES", "_CYCLIC_PLACEHOLDER_PROVIDED" }) {
            lua_getfield(stack, LUA_REGISTRYINDEX, table);
            if (lua_type(stack, -1) == LUA_TTABLE) {
                lua_pushnil(stack);
                lua_setfield(stack, -2, key);
            }
            lua_pop(stack, 1);
        }
    }
    lua_error(stack);
}

// Runs a module, on a thread of its own whose globals are an environment of its own, and leaves
// its one value on the requiring thread's stack, a table frozen through its fields; its errors are
// the require's.
int load(lua_State* stack, void* context, const char* /*path*/, const char* chunkname,
         const char* loadname) {
    fbl::state_t& state = state_of(context);
    const loading_t loading(state);
    const std::optional<std::string> text = read_file(state, stack, loadname);
    if (!text) {
        luaL_error(stack, "module '%s' can't be read", loadname);
    }
    if (fbl::frozen(state) != FBL_OK) {
        luaL_error(stack, "not enough memory");
    }
    size_t size = 0;
    const std::unique_ptr<char, decltype(&std::free)> bytecode(
        luau_compile(text->data(), text->size(), &state.compile_options, &size), &std::free);
    if (bytecode == nullptr) {
        luaL_error(stack, "not enough memory to compile");
    }
    luaL_checkstack(stack, 4, "loading a module");
    if (lua_checkstack(state.main, 1) == 0) {
        luaL_error(stack, "not enough memory");
    }
    lua_State* module = lua_newthread(stack);
    // It runs to its end, so nothing in it waits.
    fbl::mark_runner(module);
    fbl::push_environment(stack, state.main);
    lua_xmove(stack, module, 1);
    lua_replace(module, LUA_GLOBALSINDEX);
    if (luau_load(module, chunkname, bytecode.get(), size, 0) != 0) {
        lua_xmove(module, stack, 1);
        lua_error(stack);
    }
    fbl::native_compile(state, module, -1);
    // As Luau's CLI does, so a module that exports can be required in a cycle.
    bool placeholder = false;
    if (FFlag::LuauCyclicRequireShortCircuit && lua_usesexport(module, -1) != 0) {
        luarequire_createplaceholder(stack);
        placeholder = true;
    }
    const int status = lua_resume(module, stack, 0);
    if (status == LUA_YIELD) {
        lua_resetthread(module);
        lua_pushfstring(stack, "%.*s: module '%s' yielded as it loaded",
                        static_cast<int>(fbl::yield_refused.size()), fbl::yield_refused.data(),
                        loadname);
        failed(stack, placeholder, loadname);
    }
    if (status != LUA_OK) {
        // Its error, as it raised it.
        lua_xmove(module, stack, 1);
        failed(stack, placeholder, loadname);
    }
    if (lua_gettop(module) != 1) {
        lua_pushfstring(stack, "module '%s' gave %d values, not one", loadname, lua_gettop(module));
        failed(stack, placeholder, loadname);
    }
    lua_xmove(module, stack, 1);
    // A module that exports gives a table, which Luau's require fills its placeholder from.
    if (placeholder && lua_type(stack, -1) != LUA_TTABLE) {
        lua_pushfstring(stack, "module '%s' exports, so it gives a table, not a %s", loadname,
                        luaL_typename(stack, -1));
        failed(stack, placeholder, loadname);
    }
    // What a module gives every script that requires it is frozen, so none changes another's.
    if (lua_type(stack, -1) == LUA_TTABLE) {
        fbl::freeze_tables(stack, lua_gettop(stack));
    }
    lua_remove(stack, -2);
    return 1;
}

void configure(luarequire_Configuration* config) {
    config->is_require_allowed = is_require_allowed;
    config->reset = reset;
    config->jump_to_alias = jump_to_alias;
    config->to_parent = to_parent;
    config->to_child = to_child;
    config->is_module_present = is_module_present;
    config->get_chunkname = get_chunkname;
    config->get_loadname = get_loadname;
    config->get_cache_key = get_cache_key;
    config->get_config_status = get_config_status;
    config->get_config = get_config;
    config->load = load;
}

} // namespace

int32_t fbl_require_open(uint64_t state, int32_t frame, int32_t reader_slot) try {
    fbl::state_t* found = fbl::find_state(state);
    const fbl::frame_t* in = found == nullptr ? nullptr : fbl::find_frame(*found, frame);
    if (in == nullptr) {
        return FBL_MISUSE;
    }
    int reader = 0;
    if (reader_slot != -1) {
        reader = fbl::slot_index(*in, reader_slot);
        if (reader == 0) {
            return FBL_MISUSE;
        }
        if (lua_type(in->thread, reader) != LUA_TFUNCTION) {
            return fbl::misuse(fbl::formatted("slot {} isn't a function", reader_slot));
        }
    }
    if (found->require_open) {
        return fbl::misuse("the State has require already");
    }
    const bool has_reader = reader != 0;
    if (has_reader) {
        if (lua_checkstack(in->thread, 1) == 0) {
            return fbl::record_error(*found, FBL_MEMORY, "not enough memory");
        }
        lua_pushvalue(in->thread, reader);
    }
    fbl::state_t* context = found;
    int made = LUA_NOREF;
    auto work = [context, has_reader, &made](lua_State* stack) {
        if (has_reader) {
            made = lua_ref(stack, 1);
        }
        lua_pushvalue(stack, LUA_GLOBALSINDEX);
        luarequire_pushrequire(stack, configure, context);
        lua_setfield(stack, -2, "require");
        return 0;
    };
    const int arguments = has_reader ? 1 : 0;
    lua_State* const thread = in->thread;
    const int status = fbl::protect(*found, thread, arguments, 0, work);
    if (status == fbl::no_room) {
        lua_pop(thread, arguments);
    }
    const int32_t settled = fbl::settle(*found, thread, status);
    if (settled == FBL_OK) {
        found->require_reader = made;
        found->require_open = true;
    } else if (made != LUA_NOREF) {
        lua_unref(thread, made);
    }
    return settled;
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_require_forget(uint64_t state) try {
    fbl::state_t* found = fbl::find_state(state);
    if (found == nullptr) {
        return FBL_MISUSE;
    }
    // A module loading has a placeholder its require goes on with.
    if (found->requires_running > 0) {
        return fbl::record_error(*found, FBL_RUNTIME,
                                 "the modules can't be forgotten while one of them loads");
    }
    auto work = [](lua_State* stack) {
        luarequire_clearcache(stack);
        lua_pushnil(stack);
        lua_setfield(stack, LUA_REGISTRYINDEX, "_CYCLIC_PLACEHOLDER_PROVIDED");
        return 0;
    };
    return fbl::run_protected(*found, found->current, 0, 0, work);
} catch (...) {
    return fbl::escaped();
}
