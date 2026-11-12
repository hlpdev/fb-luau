// The C API: the input as a sequence of calls, each of a function the input picks, with arguments
// it gives, handles, frames, slots, and indexes among them, often wrong. Each must give a status,
// never crash, as the shim's checks are what stand between misuse and Luau's unchecked API.

#include "fuzz.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <span>
#include <string_view>

namespace {

// The input, read a byte or a number at a time; 0s once it's read.
class reader_t {
public:
    reader_t(const uint8_t* data, size_t size) : m_bytes(data, size) {}

    [[nodiscard]] bool done() const {
        return m_at >= m_bytes.size();
    }

    uint8_t byte() {
        return m_at < m_bytes.size() ? m_bytes[m_at++] : 0;
    }

    // A small number, a slot's or an index's, mostly in range, sometimes not.
    int32_t small() {
        const uint8_t value = byte();
        switch (value % 8) {
        case 0:
            return -static_cast<int32_t>(value / 8);
        case 1:
            return 1000000 + value;
        default:
            return value % 12;
        }
    }

    int64_t wide() {
        uint64_t value = 0;
        for (int each = 0; each < 8; each++) {
            value = (value << 8U) | byte();
        }
        return static_cast<int64_t>(value);
    }

    double number() {
        const std::array<double, 8> numbers = { 0.0, 1.0, -1.0, 0.5, 1e300, -1e300, 2e9, 3.0 };
        return numbers.at(byte() % numbers.size());
    }

private:
    std::span<const uint8_t> m_bytes;
    size_t m_at = 0;
};

// Snippets a frame can load, which make functions, tables, and threads to work on.
constexpr std::array<std::string_view, 8> snippets = {
    "return 1",
    "return {1, 2, x = 3}",
    "return function(...) return ... end",
    "return coroutine.create(function(a) coroutine.yield(a) return 2 end)",
    "error('raised')",
    "return setmetatable({}, { __index = function() error('index') end })",
    "return buffer.create(8)",
    "while true do end",
};

// The call the step makes, which a finding names.
int current_call = 0;

void check_status(int32_t status) {
    if (!is_status(status)) {
        std::fputs(std::format("call {} gave {}\n", current_call, status).c_str(), stderr);
        finding("a call gave what isn't a status");
    }
}

void step(reader_t& in, uint64_t state, uint64_t other) {
    // Which State: its own mostly, sometimes another's, a closed one's, or none.
    const uint8_t pick = in.byte();
    uint64_t handle = state;
    if (pick % 16 == 0) {
        handle = other;
    } else if (pick % 16 == 1) {
        handle = 0;
    } else if (pick % 16 == 2) {
        handle = state + (uint64_t{ 1 } << 32U);
    } else if (pick % 16 == 3) {
        handle = static_cast<uint64_t>(in.wide());
    }
    const int32_t frame = in.small();
    const uint8_t op = in.byte();
    std::array<uint8_t, 16> bytes{};
    current_call = op % 40;
    switch (current_call) {
    case 0:
        fbl_frame_open(handle);
        break;
    case 1:
        fbl_frame_close(handle, frame);
        break;
    case 2:
        fbl_frame_count(handle, frame);
        break;
    case 3:
        fbl_frame_truncate(handle, frame, in.small());
        break;
    case 4:
        fbl_frame_insert(handle, frame, in.small());
        break;
    case 5:
        fbl_frame_remove(handle, frame, in.small());
        break;
    case 6:
        check_status(fbl_frame_push_nil(handle, frame));
        break;
    case 7:
        check_status(fbl_frame_push_number(handle, frame, in.number()));
        break;
    case 8:
        check_status(fbl_frame_push_integer(handle, frame, in.wide()));
        break;
    case 9: {
        const std::string_view text = snippets.at(in.byte() % snippets.size());
        check_status(fbl_frame_push_string(handle, frame,
                                           reinterpret_cast<const uint8_t*>(text.data()),
                                           static_cast<int64_t>(text.size())));
        break;
    }
    case 10:
        check_status(fbl_frame_push_copy(handle, frame, in.small()));
        break;
    case 11: {
        const std::string_view source = snippets.at(in.byte() % snippets.size());
        check_status(fbl_load_source(handle, frame, reinterpret_cast<const uint8_t*>(source.data()),
                                     static_cast<int64_t>(source.size()), "=api"));
        break;
    }
    case 12:
        check_status(fbl_call_function(handle, frame, in.small(), in.small()));
        break;
    case 13:
        check_status(fbl_table_new(handle, frame, in.small(), in.small()));
        break;
    case 14:
        check_status(fbl_table_get(handle, frame, in.small(), in.small()));
        break;
    case 15:
        check_status(fbl_table_set(handle, frame, in.small(), in.small(), in.small()));
        break;
    case 16:
        check_status(fbl_table_raw_get(handle, frame, in.small(), in.small()));
        break;
    case 17:
        check_status(fbl_table_raw_set(handle, frame, in.small(), in.small(), in.small()));
        break;
    case 18:
        check_status(fbl_table_raw_get_index(handle, frame, in.small(), in.small()));
        break;
    case 19:
        fbl_table_length(handle, frame, in.small());
        break;
    case 20:
        fbl_table_next(handle, frame, in.small(), in.small());
        break;
    case 21:
        check_status(fbl_table_get_metatable(handle, frame, in.small()));
        break;
    case 22:
        check_status(fbl_table_set_metatable(handle, frame, in.small(), in.small()));
        break;
    case 23:
        check_status(fbl_table_freeze(handle, frame, in.small()));
        break;
    case 24:
        check_status(fbl_thread_new(handle, frame, in.small()));
        break;
    case 25: {
        int32_t finished = 0;
        check_status(fbl_thread_resume(handle, frame, in.small(), in.byte() % 2, &finished));
        break;
    }
    case 26:
        fbl_thread_status(handle, frame, in.small());
        fbl_thread_id(handle, frame, in.small());
        break;
    case 27:
        check_status(fbl_thread_close(handle, frame, in.small()));
        break;
    case 28: {
        int32_t ref = 0;
        check_status(fbl_ref_new(handle, frame, in.small(), &ref));
        break;
    }
    case 29:
        check_status(fbl_ref_push(handle, frame, in.small()));
        break;
    case 30:
        fbl_ref_free(handle, in.small());
        break;
    case 31:
        check_status(fbl_buffer_new(handle, frame, in.wide() % 4096));
        break;
    case 32:
        fbl_buffer_read(handle, frame, in.small(), in.wide(), bytes.data(), in.byte() % 17);
        break;
    case 33:
        fbl_buffer_write(handle, frame, in.small(), in.wide(), bytes.data(), in.byte() % 17);
        break;
    case 34:
        check_status(fbl_sandbox_environment(handle, frame));
        break;
    case 35:
        check_status(fbl_sandbox_freeze(handle));
        break;
    case 36:
        check_status(
            fbl_frame_get_global(handle, frame, reinterpret_cast<const uint8_t*>("print"), 5));
        break;
    case 37:
        check_status(fbl_frame_set_global(handle, frame, reinterpret_cast<const uint8_t*>("x"), 1,
                                          in.small()));
        break;
    case 38: {
        int64_t object = 0;
        int32_t type = 0;
        fbl_udata_object(handle, frame, in.small(), &object, &type);
        check_status(fbl_udata_new(handle, frame, in.wide(), in.small()));
        break;
    }
    default: {
        check_status(fbl_state_collect(handle));
        // A step gives 1 for a cycle finished, 0 for not, and -1 for misuse.
        const int32_t stepped = fbl_state_step(handle, in.small());
        if (stepped < -1 || stepped > 1) {
            finding("a step gave what it doesn't say it gives");
        }
        break;
    }
    }
}

} // namespace

// NOLINTNEXTLINE(readability-identifier-naming): libFuzzer names it.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const no_escape_t watched;
    {
        const fuzz_state_t lua;
        const fuzz_state_t other;
        if (lua.handle() == 0 || other.handle() == 0) {
            return 0;
        }
        reader_t in(data, size);
        for (int steps = 0; steps < 256 && !in.done(); steps++) {
            step(in, lua.handle(), other.handle());
        }
    }
    expect_nothing_left();
    return 0;
}
