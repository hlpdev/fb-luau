// A mod, as a game runs one: arbitrary bytes as a script in an environment of its own, in a frozen
// State with the program's functions, a registered type and an object of it, and require over a
// module the input gives, the script run on a thread resumed until it ends, as a scheduler would.
// Each call must give a status, never crash, hang past its budget, or leak a State or its memory;
// the program's functions call back into the State, raise, and yield, as a game's do.

#include "fuzz.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace {

// The program's functions, by their numbers.
enum function_t : uint8_t {
    ECHO = 1,
    FAIL,
    YIELD,
    CALL,
    WHERE,
    READER,
    INDEX,
    NEWINDEX,
    NAMECALL,
    TOSTRING,
};

// The module the input gives, "m.luau", read by the reader while the input runs.
std::string_view module_text;

void expect(bool holds, const char* what) {
    if (!holds) {
        finding(what);
    }
}

// Leaves `text` alone in the frame, as an error to raise.
int32_t raised(uint64_t state, int32_t frame, std::string_view text) {
    fbl_frame_truncate(state, frame, 0);
    fbl_frame_push_string(state, frame, reinterpret_cast<const uint8_t*>(text.data()),
                          static_cast<int64_t>(text.size()));
    return FBL_RUNTIME;
}

// The one callback, as the package's is: the program's functions by their numbers.
int32_t callback(uint64_t state, int32_t frame, int64_t function) {
    const int32_t count = fbl_frame_count(state, frame);
    expect(count >= 0, "a callback's frame has no count");
    switch (function) {
    case ECHO:
        return FBL_OK;
    case FAIL:
        return raised(state, frame, "the program's function failed");
    case YIELD:
        if (fbl_thread_can_yield(state, frame) == 1) {
            return FBL_YIELD;
        }
        return raised(state, frame, "can't yield here");
    case CALL: {
        // Calls its first argument with the rest, back into the State, and gives what it gave,
        // or raises what it raised.
        if (count == 0) {
            return FBL_OK;
        }
        if (fbl_frame_type(state, frame, 0) != FBL_TYPE_FUNCTION) {
            return raised(state, frame, "call's first argument isn't a function");
        }
        const int32_t status = fbl_call_function(state, frame, 0, -1);
        if (!is_status(status) || status == FBL_MISUSE) {
            finding(fbl_core_error());
        }
        if (status != FBL_OK) {
            return raised(state, frame, "the call failed");
        }
        return FBL_OK;
    }
    case WHERE:
        fbl_frame_truncate(state, frame, 0);
        expect(fbl_callback_where(state, frame) == FBL_OK, "where wasn't given");
        return FBL_OK;
    case READER: {
        int64_t length = 0;
        const uint8_t* path = fbl_frame_string(state, frame, 0, &length);
        fbl_frame_truncate(state, frame, 0);
        if (path != nullptr && std::string_view(reinterpret_cast<const char*>(path),
                                                static_cast<size_t>(length)) == "m.luau") {
            return fbl_frame_push_string(state, frame,
                                         reinterpret_cast<const uint8_t*>(module_text.data()),
                                         static_cast<int64_t>(module_text.size()));
        }
        return fbl_frame_push_nil(state, frame);
    }
    case INDEX: {
        // The door's one field, Open, and the object its slot holds.
        int64_t object = 0;
        int32_t type = 0;
        expect(fbl_udata_object(state, frame, 0, &object, &type) && type == 1,
               "__index wasn't given its object");
        fbl_frame_truncate(state, frame, 0);
        return fbl_frame_push_boolean(state, frame, object % 2 == 0);
    }
    case NEWINDEX:
        fbl_frame_truncate(state, frame, 0);
        return FBL_OK;
    case NAMECALL:
        // The method's name, then the object, then the arguments: gives them back.
        return FBL_OK;
    case TOSTRING: {
        fbl_frame_truncate(state, frame, 0);
        const std::string_view text = "door";
        return fbl_frame_push_string(state, frame, reinterpret_cast<const uint8_t*>(text.data()),
                                     static_cast<int64_t>(text.size()));
    }
    default:
        finding("a callback for a function the program never made");
    }
}

// Sets global `name` to the frame's last value, which it takes off.
void set_global(uint64_t state, int32_t frame, std::string_view name) {
    const int32_t top = fbl_frame_count(state, frame) - 1;
    expect(fbl_frame_set_global(state, frame, reinterpret_cast<const uint8_t*>(name.data()),
                                static_cast<int64_t>(name.size()), top) == FBL_OK,
           "a global wasn't set");
    fbl_frame_truncate(state, frame, top);
}

// The game's side: its functions, its type and a door, require, and the State frozen.
void give_api(uint64_t state, int32_t frame) {
    const std::array<std::pair<function_t, const char*>, 5> globals = {
        {
            { ECHO, "echo" },
            { FAIL, "fail" },
            { YIELD, "yield" },
            { CALL, "call" },
            { WHERE, "where" },
        },
    };
    for (const auto& [number, name] : globals) {
        expect(fbl_callback_function(state, frame, number, name) == FBL_OK,
               "a function wasn't made");
        set_global(state, frame, name);
    }
    const int32_t base = fbl_frame_count(state, frame);
    expect(fbl_callback_function(state, frame, INDEX, "__index") == FBL_OK &&
               fbl_callback_function(state, frame, NEWINDEX, "__newindex") == FBL_OK &&
               fbl_callback_namecall(state, frame, NAMECALL, "__namecall") == FBL_OK &&
               fbl_udata_type(state, frame, 1, "Door", base, base + 1, base + 2) == FBL_OK,
           "the type wasn't registered");
    expect(fbl_callback_function(state, frame, TOSTRING, "__tostring") == FBL_OK &&
               fbl_udata_type_set(state, frame, 1, "__tostring", base + 3) == FBL_OK,
           "the type's __tostring wasn't set");
    fbl_frame_truncate(state, frame, base);
    expect(fbl_udata_new(state, frame, 2, 1) == FBL_OK, "a door wasn't made");
    set_global(state, frame, "front");
    expect(fbl_udata_new(state, frame, 3, 1) == FBL_OK, "a door wasn't made");
    set_global(state, frame, "back");
    expect(fbl_callback_function(state, frame, READER, "reader") == FBL_OK &&
               fbl_require_open(state, frame, base) == FBL_OK,
           "require wasn't opened");
    fbl_frame_truncate(state, frame, base);
    expect(fbl_sandbox_freeze(state) == FBL_OK && fbl_sandbox_is_frozen(state) == 1,
           "the State wasn't frozen");
}

void run(const uint8_t* data, size_t size) {
    fbl_state_options_t options = fuzz_options();
    // A mod's: no loadstring or environments.
    options.load_string = false;
    options.environments = false;
    const fuzz_state_t lua(options);
    if (lua.handle() == 0) {
        return;
    }
    const uint64_t state = lua.handle();
    const int32_t frame = fbl_frame_open(state);
    expect(frame >= 0, "a new State's first frame didn't open");
    give_api(state, frame);

    // The script, then, after its first NUL, the module m.
    const std::string_view input(reinterpret_cast<const char*>(data), size);
    const size_t split = input.find('\0');
    const std::string_view script = input.substr(0, split);
    module_text = split == std::string_view::npos ? "" : input.substr(split + 1);

    const int32_t environment = fbl_frame_count(state, frame);
    expect(fbl_sandbox_environment(state, frame) == FBL_OK, "no environment was made");
    int32_t status =
        fbl_load_source_in(state, frame, reinterpret_cast<const uint8_t*>(script.data()),
                           static_cast<int64_t>(script.size()), "@mod", environment);
    expect(is_status(status) && status != FBL_MISUSE, "loading gave what isn't a script's status");
    if (status == FBL_OK) {
        expect(fbl_thread_new(state, frame, environment + 1) == FBL_OK, "no thread was made");
        const int32_t thread = fbl_frame_count(state, frame) - 1;
        for (int resume = 0; resume < 16; resume++) {
            expect(fbl_frame_push_copy(state, frame, thread) == FBL_OK, "no copy");
            expect(fbl_frame_push_integer(state, frame, resume) == FBL_OK, "no argument");
            int32_t finished = 0;
            status =
                fbl_thread_resume(state, frame, thread + 1, resume % 5 == 4 ? 1 : 0, &finished);
            expect(is_status(status) && status != FBL_MISUSE,
                   "a resume gave what isn't a script's status");
            fbl_frame_truncate(state, frame, thread + 1);
            if (status != FBL_OK || finished == 1) {
                break;
            }
            // What a driver does between ticks.
            expect(fbl_state_step(state, 1) >= 0, "a step failed");
        }
        expect(fbl_thread_status(state, frame, thread) >= 0, "the thread has no status");
    }
    if (status != FBL_OK) {
        int64_t length = 0;
        expect(fbl_state_error_kind(state) != FBL_OK, "an error wasn't kept");
        expect(fbl_state_error_message(state, &length) != nullptr && length >= 0,
               "an error has no message");
        expect(fbl_state_error_traceback(state, &length) != nullptr, "an error has no traceback");
        expect(fbl_state_error_value(state, frame) == FBL_OK, "an error has no value");
    }
    expect(fbl_frame_close(state, frame), "the frame didn't close");
    std::array<int64_t, 16> released{};
    expect(fbl_state_collect(state) == FBL_OK, "a collection failed");
    const auto capacity = static_cast<int32_t>(released.size());
    expect(fbl_callback_released(state, released.data(), capacity) >= 0 &&
               fbl_udata_released(state, released.data(), capacity) >= 0,
           "what was released wasn't given");
}

} // namespace

// NOLINTNEXTLINE(readability-identifier-naming): libFuzzer names it.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static const bool set = fbl_callback_set(callback);
    expect(set, "the callback wasn't set");
    {
        const no_escape_t watched;
        run(data, size);
    }
    expect_nothing_left();
    return 0;
}
