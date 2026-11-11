#pragma once

#include <string_view>

namespace fbl {

// Keeps `message` as this thread's last error, which fbl_core_error gives, and gives false, so a
// function that fails can `return fail("why")`.
bool fail(std::string_view message) noexcept;

// Sets Luau's flags as the `luau` CLI of its release does: each one whose name starts with "Luau",
// but the ones Luau calls experimental. Once in the process, whichever thread asks first; each
// State asks before it's made, as the flags are globals every State reads.
void set_flags() noexcept;

} // namespace fbl
