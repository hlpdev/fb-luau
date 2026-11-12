#pragma once

#include <cstdint>
#include <exception>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace fbl {

// std::format's text, or "" when there's no memory for it, so code that mustn't throw can say why.
template <typename... Args>
std::string formatted(std::format_string<Args...> format, Args&&... args) noexcept {
    try {
        return std::format(format, std::forward<Args>(args)...);
    } catch (const std::exception&) {
        return {};
    }
}

// Keeps `message` as this thread's last error, which fbl_core_error gives, and gives false, so a
// function that fails can `return fail("why")`.
bool fail(std::string_view message) noexcept;

// What an exported function does with an exception that got as far as it, which nothing should:
// says so as this thread's last error, and gives FBL_OTHER.
int32_t escaped() noexcept;

// Sets Luau's flags as the `luau` CLI of its release does: each one whose name starts with "Luau",
// but the ones Luau calls experimental. Once in the process, whichever thread asks first; each
// State asks before it's made, as the flags are globals every State reads.
void set_flags() noexcept;

// The lowest address this thread's native stack may be used down to, with `reserve` bytes left
// below it, or 0 when the platform doesn't say where the stack ends. Found once a thread.
uintptr_t stack_floor(uintptr_t reserve) noexcept;

} // namespace fbl
