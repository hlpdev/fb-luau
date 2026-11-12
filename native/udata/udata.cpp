#include "core/core.hpp"
#include "fbl.h"
#include "state/state.hpp"

#include <cstddef>
#include <cstring>
#include <exception>
#include <lua.h>
#include <lualib.h>
#include <string_view>

namespace {

// What a program's object is in Luau: a userdata of its number and its type's.
struct object_box_t {
    int64_t object;
    int32_t type;
};

// An object's userdata's destructor, run as Luau collects it: it keeps the object's number for
// the program to take, in room kept for it, as a destructor mustn't need memory, or touch the VM.
// NOLINTNEXTLINE(misc-const-correctness): Luau's lua_Destructor takes a void*.
void released(lua_State* thread, void* data) {
    auto* state = static_cast<fbl::state_t*>(lua_callbacks(thread)->userdata);
    const auto* box = static_cast<const object_box_t*>(data);
    if (state == nullptr) {
        return;
    }
    if (state->released_objects.size() < state->released_objects.capacity()) {
        state->released_objects.push_back(box->object);
    }
    state->live_objects--;
}

// The registry's reference to a registered type's metatable, or LUA_NOREF.
int type_ref(const fbl::state_t& state, int32_t type) {
    if (type < 1 || std::cmp_greater(type, state.type_refs.size())) {
        return LUA_NOREF;
    }
    return state.type_refs[static_cast<size_t>(type - 1)];
}

// What each call names: its State and frame, or null, saying why.
struct found_t {
    fbl::state_t* state = nullptr;
    const fbl::frame_t* frame = nullptr;

    explicit operator bool() const {
        return frame != nullptr;
    }
};

found_t find(uint64_t state, int32_t frame) noexcept {
    fbl::state_t* found = fbl::find_state(state);
    return { .state = found, .frame = found == nullptr ? nullptr : fbl::find_frame(*found, frame) };
}

} // namespace

namespace fbl {

void prepare_objects(lua_State* thread, state_t& state) {
    lua_setuserdatadtor(thread, object_tag, released);
    // The userdata each object has while Luau has it, by its number, which Luau's collector
    // clears as it collects one.
    lua_newtable(thread);
    lua_newtable(thread);
    lua_pushstring(thread, "v");
    lua_setfield(thread, -2, "__mode");
    lua_setmetatable(thread, -2);
    state.objects_ref = lua_ref(thread, -1);
    lua_pop(thread, 1);
}

} // namespace fbl

int32_t fbl_udata_type(uint64_t state, int32_t frame, int32_t type, const char* name,
                       int32_t index_slot, int32_t newindex_slot, int32_t namecall_slot) try {
    const found_t found = find(state, frame);
    if (!found) {
        return FBL_MISUSE;
    }
    if (type < 1 || type > 1000000) {
        return fbl::misuse(fbl::formatted("{} isn't a type's number, from 1", type));
    }
    if (type_ref(*found.state, type) != LUA_NOREF) {
        return fbl::misuse(fbl::formatted("type {} is registered already", type));
    }
    if (name == nullptr) {
        return fbl::misuse("a type needs a name");
    }
    lua_State* thread = found.frame->thread;
    int slots[3] = {};
    int at = 0;
    for (const int32_t slot : { index_slot, newindex_slot, namecall_slot }) {
        const int index = fbl::slot_index(*found.frame, slot);
        if (index == 0) {
            return FBL_MISUSE;
        }
        if (lua_type(thread, index) != LUA_TFUNCTION) {
            return fbl::misuse(fbl::formatted("slot {} isn't a function", slot));
        }
        slots[at++] = index;
    }
    try {
        if (found.state->type_refs.size() < static_cast<size_t>(type)) {
            found.state->type_refs.resize(static_cast<size_t>(type), LUA_NOREF);
        }
    } catch (const std::exception&) {
        return fbl::record_error(*found.state, FBL_MEMORY, "not enough memory");
    }
    if (lua_checkstack(thread, 3) == 0) {
        return fbl::record_error(*found.state, FBL_MEMORY, "not enough memory");
    }
    for (const int index : slots) {
        lua_pushvalue(thread, index);
    }
    int made = LUA_NOREF;
    auto work = [name, &made](lua_State* stack) {
        lua_createtable(stack, 0, 6);
        lua_pushvalue(stack, 1);
        lua_setfield(stack, -2, "__index");
        lua_pushvalue(stack, 2);
        lua_setfield(stack, -2, "__newindex");
        lua_pushvalue(stack, 3);
        lua_setfield(stack, -2, "__namecall");
        lua_pushstring(stack, name);
        lua_setfield(stack, -2, "__type");
        // What getmetatable gives a script, so it never has the table itself.
        lua_pushstring(stack, "The metatable is locked");
        lua_setfield(stack, -2, "__metatable");
        lua_setreadonly(stack, -1, 1);
        made = lua_ref(stack, -1);
        lua_pop(stack, 1);
        return 0;
    };
    const int status = fbl::protect(*found.state, thread, 3, 0, work);
    if (status == fbl::no_room) {
        lua_pop(thread, 3);
    }
    const int32_t settled = fbl::settle(*found.state, thread, status);
    if (settled == FBL_OK) {
        found.state->type_refs[static_cast<size_t>(type - 1)] = made;
    }
    return settled;
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_udata_type_set(uint64_t state, int32_t frame, int32_t type, const char* field,
                           int32_t value_slot) try {
    const found_t found = find(state, frame);
    const int index = found ? fbl::slot_index(*found.frame, value_slot) : 0;
    if (index == 0) {
        return FBL_MISUSE;
    }
    const int ref = type_ref(*found.state, type);
    if (ref == LUA_NOREF) {
        return fbl::misuse(fbl::formatted("type {} isn't registered", type));
    }
    if (field == nullptr) {
        return fbl::misuse("a metatable's field needs a name");
    }
    const std::string_view name(field);
    for (const std::string_view kept :
         { "__index", "__newindex", "__namecall", "__metatable", "__type" }) {
        if (name == kept) {
            return fbl::misuse(fbl::formatted("{} is the type's own", name));
        }
    }
    lua_State* thread = found.frame->thread;
    if (lua_checkstack(thread, 1) == 0) {
        return fbl::record_error(*found.state, FBL_MEMORY, "not enough memory");
    }
    lua_pushvalue(thread, index);
    auto work = [ref, field](lua_State* stack) {
        lua_getref(stack, ref);
        lua_setreadonly(stack, -1, 0);
        lua_pushvalue(stack, 1);
        lua_setfield(stack, -2, field);
        lua_setreadonly(stack, -1, 1);
        return 0;
    };
    const int status = fbl::protect(*found.state, thread, 1, 0, work);
    if (status == fbl::no_room) {
        lua_pop(thread, 1);
    }
    return fbl::settle(*found.state, thread, status);
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_udata_push(uint64_t state, int32_t frame, int64_t object) try {
    const found_t found = find(state, frame);
    if (!found) {
        return FBL_MISUSE;
    }
    lua_State* thread = found.frame->thread;
    // Reading a table raw, by a number, needs no memory.
    if (lua_checkstack(thread, 2) == 0) {
        return fbl::record_error(*found.state, FBL_MEMORY, "not enough memory");
    }
    lua_getref(thread, found.state->objects_ref);
    lua_pushnumber(thread, static_cast<double>(object));
    lua_rawget(thread, -2);
    lua_remove(thread, -2);
    if (lua_type(thread, -1) != LUA_TUSERDATA) {
        lua_pop(thread, 1);
        return FBL_OTHER;
    }
    return FBL_OK;
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_udata_new(uint64_t state, int32_t frame, int64_t object, int32_t type) try {
    const found_t found = find(state, frame);
    if (!found) {
        return FBL_MISUSE;
    }
    const int ref = type_ref(*found.state, type);
    if (ref == LUA_NOREF) {
        return fbl::misuse(fbl::formatted("type {} isn't registered", type));
    }
    try {
        // Room for its number when it's collected, before it can be.
        found.state->released_objects.reserve(static_cast<size_t>(found.state->live_objects + 1));
    } catch (const std::exception&) {
        return fbl::record_error(*found.state, FBL_MEMORY, "not enough memory");
    }
    const int objects = found.state->objects_ref;
    auto work = [object, type, ref, objects](lua_State* stack) {
        auto* box = static_cast<object_box_t*>(
            lua_newuserdatatagged(stack, sizeof(object_box_t), fbl::object_tag));
        box->object = object;
        box->type = type;
        lua_getref(stack, ref);
        lua_setmetatable(stack, -2);
        lua_getref(stack, objects);
        lua_pushnumber(stack, static_cast<double>(object));
        lua_pushvalue(stack, -3);
        lua_rawset(stack, -3);
        lua_pop(stack, 1);
        return 1;
    };
    const int32_t status = fbl::run_protected(*found.state, found.frame->thread, 0, 1, work);
    if (status == FBL_OK) {
        found.state->live_objects++;
    }
    return status;
} catch (...) {
    return fbl::escaped();
}

bool fbl_udata_object(uint64_t state, int32_t frame, int32_t slot, int64_t* object,
                      int32_t* type) try {
    const found_t found = find(state, frame);
    const int index = found ? fbl::slot_index(*found.frame, slot) : 0;
    if (index == 0 || object == nullptr || type == nullptr) {
        return false;
    }
    const auto* box = static_cast<const object_box_t*>(
        lua_touserdatatagged(found.frame->thread, index, fbl::object_tag));
    if (box == nullptr) {
        return fbl::fail(fbl::formatted("slot {} isn't one of the program's objects", slot));
    }
    *object = box->object;
    *type = box->type;
    return true;
} catch (...) {
    fbl::escaped();
    return false;
}

int32_t fbl_udata_released(uint64_t state, int64_t* objects, int32_t capacity) try {
    fbl::state_t* found = fbl::find_state(state);
    if (found == nullptr) {
        return -1;
    }
    if (capacity < 0 || (objects == nullptr && capacity > 0)) {
        fbl::fail(fbl::formatted("{} objects can't go there", capacity));
        return -1;
    }
    int32_t taken = 0;
    while (taken < capacity && !found->released_objects.empty()) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): C's array of them.
        objects[taken++] = found->released_objects.back();
        found->released_objects.pop_back();
    }
    return taken;
} catch (...) {
    fbl::escaped();
    return -1;
}
