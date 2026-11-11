#include "core/core.hpp"

#include "fbl.h"

#include <cstring>
#include <exception>
#include <Luau/Common.h>
#include <Luau/ExperimentalFlags.h>
#include <string>
#include <string_view>

namespace {

thread_local std::string last_error;

} // namespace

namespace fbl {

// NOLINTNEXTLINE(bugprone-exception-escape): what it assigns second fits in any string as it is.
bool fail(std::string_view message) noexcept {
    try {
        last_error = message;
    } catch (const std::exception&) {
        // No memory for it, or more than a string holds.
        last_error = "out of memory";
    }
    return false;
}

void set_flags() noexcept {
    // A function's static is made once, by the first thread here, while any other waits for it.
    static const bool set = [] {
        for (Luau::FValue<bool>* flag = Luau::FValue<bool>::list; flag != nullptr;
             flag = flag->next) {
            if (std::strncmp(flag->name, "Luau", 4) == 0 &&
                !Luau::isAnalysisFlagExperimental(flag->name)) {
                flag->value = true;
            }
        }
        return true;
    }();
    static_cast<void>(set);
}

} // namespace fbl

const char* fbl_core_version(void) {
    return FBL_VERSION;
}

const char* fbl_core_luau_version(void) {
    return FBL_LUAU_VERSION;
}

const char* fbl_core_error(void) {
    return last_error.c_str();
}
