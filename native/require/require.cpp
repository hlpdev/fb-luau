#include "core/core.hpp"
#include "fbl.h"
#include "state/state.hpp"

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

// The files a module path names, in the order they're looked for, as Luau's CLI has them.
constexpr std::array<std::string_view, 4> module_suffixes = {
    ".luau",
    ".lua",
    "/init.luau",
    "/init.lua",
};

fbl::state_t& state_of(void* context) {
    return *static_cast<fbl::state_t*>(context);
}

// A module path's parts joined as a file path's are.
std::string joined(const std::vector<std::string>& parts) {
    std::string path;
    for (const std::string& part : parts) {
        if (!path.empty()) {
            path += '/';
        }
        path += part;
    }
    return path;
}

// A file's path as a module path's parts from the root: "." left out, ".." going up, and a
// module's file named as its module, or nothing for a path that goes above the root.
std::optional<std::vector<std::string>> module_parts(std::string_view path) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find_first_of("/\\", start);
        if (end == std::string_view::npos) {
            end = path.size();
        }
        const std::string_view part = path.substr(start, end - start);
        if (part == "..") {
            if (parts.empty()) {
                return std::nullopt;
            }
            parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.emplace_back(part);
        }
        start = end + 1;
    }
    if (!parts.empty()) {
        std::string& last = parts.back();
        for (const std::string_view suffix :
             { std::string_view(".luau"), std::string_view(".lua") }) {
            if (last.size() > suffix.size() && last.ends_with(suffix)) {
                last.resize(last.size() - suffix.size());
                break;
            }
        }
        if (last == "init") {
            parts.pop_back();
        }
    }
    return parts;
}

// The file at `path`, as the reader gives it, read once in each require's walk; it raises the
// reader's errors, and one for what isn't text.
const std::optional<std::string>& read_file(fbl::state_t& state, lua_State* stack,
                                            const std::string& path) {
    const auto found = state.module_files.find(path);
    if (found != state.module_files.end()) {
        return found->second;
    }
    luaL_checkstack(stack, 2, "reading a module");
    lua_getref(stack, state.require_reader);
    lua_pushlstring(stack, path.data(), path.size());
    lua_call(stack, 1, 1);
    std::optional<std::string> text;
    if (lua_type(stack, -1) == LUA_TSTRING) {
        size_t length = 0;
        const char* bytes = lua_tolstring(stack, -1, &length);
        text.emplace(bytes, length);
    } else if (lua_type(stack, -1) != LUA_TNIL) {
        luaL_error(stack, "the modules' reader gave a %s for '%s', not a string or nil",
                   luaL_typename(stack, -1), path.c_str());
    }
    lua_pop(stack, 1);
    return state.module_files.emplace(path, std::move(text)).first->second;
}

// Finds the module the walk is at: how many of its files there are, and the first's path in
// module_file, or "" for none.
int resolve(fbl::state_t& state, lua_State* stack) {
    state.module_file.clear();
    if (state.module_path.empty()) {
        return 0;
    }
    const std::string base = joined(state.module_path);
    int count = 0;
    for (const std::string_view suffix : module_suffixes) {
        std::string file = base + std::string(suffix);
        if (read_file(state, stack, file).has_value()) {
            if (count == 0) {
                state.module_file = std::move(file);
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
    return true;
}

luarequire_NavigateResult reset(lua_State* /*stack*/, void* context, const char* requirer) {
    fbl::state_t& state = state_of(context);
    state.module_files.clear();
    const std::string_view name = requirer;
    if (!name.starts_with('@')) {
        // A chunk of another name requires from the root, as a module there.
        state.module_path = { std::string(name) };
        return NAVIGATE_SUCCESS;
    }
    std::optional<std::vector<std::string>> parts = module_parts(name.substr(1));
    if (!parts) {
        return NAVIGATE_NOT_FOUND;
    }
    state.module_path = std::move(*parts);
    return NAVIGATE_SUCCESS;
}

luarequire_NavigateResult jump_to_alias(lua_State* stack, void* context, const char* path) {
    fbl::state_t& state = state_of(context);
    // An alias's path that isn't relative to its .luaurc is from the root.
    const std::string_view given = path;
    if (!given.starts_with('/')) {
        return NAVIGATE_NOT_FOUND;
    }
    std::optional<std::vector<std::string>> parts = module_parts(given);
    if (!parts) {
        return NAVIGATE_NOT_FOUND;
    }
    state.module_path = std::move(*parts);
    return resolve(state, stack) > 1 ? NAVIGATE_AMBIGUOUS : NAVIGATE_SUCCESS;
}

luarequire_NavigateResult to_parent(lua_State* /*stack*/, void* context) {
    fbl::state_t& state = state_of(context);
    // Nothing is above the root: a module reaches only what the program gives.
    if (state.module_path.empty()) {
        return NAVIGATE_NOT_FOUND;
    }
    state.module_path.pop_back();
    return NAVIGATE_SUCCESS;
}

luarequire_NavigateResult to_child(lua_State* stack, void* context, const char* name) {
    fbl::state_t& state = state_of(context);
    const std::string_view child = name;
    if (child.empty() || child == "." || child == ".." || child == ".config" ||
        child.find_first_of("/\\") != std::string_view::npos) {
        return NAVIGATE_NOT_FOUND;
    }
    state.module_path.emplace_back(child);
    return resolve(state, stack) > 1 ? NAVIGATE_AMBIGUOUS : NAVIGATE_SUCCESS;
}

bool is_module_present(lua_State* stack, void* context) {
    fbl::state_t& state = state_of(context);
    return resolve(state, stack) > 0;
}

luarequire_WriteResult get_chunkname(lua_State* /*stack*/, void* context, char* buffer, size_t size,
                                     size_t* size_out) {
    return write("@" + state_of(context).module_file, buffer, size, size_out);
}

luarequire_WriteResult get_loadname(lua_State* /*stack*/, void* context, char* buffer, size_t size,
                                    size_t* size_out) {
    return write(state_of(context).module_file, buffer, size, size_out);
}

luarequire_WriteResult get_cache_key(lua_State* /*stack*/, void* context, char* buffer, size_t size,
                                     size_t* size_out) {
    return write(state_of(context).module_file, buffer, size, size_out);
}

// Where the .luaurc of the place the walk is at would be.
std::string config_file(const fbl::state_t& state) {
    const std::string folder = joined(state.module_path);
    return folder.empty() ? ".luaurc" : folder + "/.luaurc";
}

luarequire_ConfigStatus get_config_status(lua_State* stack, void* context) {
    fbl::state_t& state = state_of(context);
    const std::string file = config_file(state);
    return read_file(state, stack, file).has_value() ? CONFIG_PRESENT_JSON : CONFIG_ABSENT;
}

luarequire_WriteResult get_config(lua_State* stack, void* context, char* buffer, size_t size,
                                  size_t* size_out) {
    fbl::state_t& state = state_of(context);
    const std::string file = config_file(state);
    const std::optional<std::string>& text = read_file(state, stack, file);
    if (!text) {
        return WRITE_FAILURE;
    }
    return write(*text, buffer, size, size_out);
}

// Runs a module, on a thread of its own whose globals are an environment of its own, and leaves
// its one value on the requiring thread's stack; its errors are the require's.
int load(lua_State* stack, void* context, const char* /*path*/, const char* chunkname,
         const char* loadname) {
    fbl::state_t& state = state_of(context);
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
    luaL_checkstack(stack, 3, "loading a module");
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
    if (FFlag::LuauCyclicRequireShortCircuit && lua_usesexport(module, -1) != 0) {
        luarequire_createplaceholder(stack);
    }
    const int status = lua_resume(module, stack, 0);
    if (status == LUA_YIELD) {
        lua_resetthread(module);
        luaL_error(stack, "%.*s: module '%s' yielded as it loaded",
                   static_cast<int>(fbl::yield_refused.size()), fbl::yield_refused.data(),
                   loadname);
    }
    if (status != LUA_OK) {
        // Its error, as it raised it.
        lua_xmove(module, stack, 1);
        lua_error(stack);
    }
    if (lua_gettop(module) != 1) {
        luaL_error(stack, "module '%s' gave %d values, not one", loadname, lua_gettop(module));
    }
    lua_xmove(module, stack, 1);
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
    auto work = [](lua_State* stack) {
        luarequire_clearcache(stack);
        return 0;
    };
    return fbl::run_protected(*found, found->current, 0, 0, work);
} catch (...) {
    return fbl::escaped();
}
