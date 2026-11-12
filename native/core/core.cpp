#include "core/core.hpp"

#include "fbl.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <Luau/Common.h>
#include <Luau/ExperimentalFlags.h>
#include <string>
#include <string_view>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// psapi.h after windows.h, which it needs.
#include <psapi.h>
#elifdef __APPLE__
#include <mach/mach.h>
#else
#include <unistd.h>
#endif

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

int32_t escaped() noexcept {
    fail("an exception got to the edge of the library, which is a bug in it");
    return FBL_OTHER;
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

const char* fbl_core_version(void) try { return FBL_VERSION; } catch (...) {
    fbl::escaped();
    return "";
}

const char* fbl_core_luau_version(void) try { return FBL_LUAU_VERSION; } catch (...) {
    fbl::escaped();
    return "";
}

const char* fbl_core_error(void) try { return last_error.c_str(); } catch (...) {
    fbl::escaped();
    return "";
}

int64_t fbl_core_resident(void) try {
#ifdef _WIN32
    // Its private bytes, what it's committed of its own, which follow its heaps, where the
    // working set follows what Windows trims and pages back in.
    PROCESS_MEMORY_COUNTERS_EX counters{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the _EX counters, as Windows
    // has.
    if (K32GetProcessMemoryInfo(GetCurrentProcess(),
                                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                sizeof(counters)) == 0) {
        return -1;
    }
    return static_cast<int64_t>(counters.PrivateUsage);
#elifdef __APPLE__
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): mach's own info, as its ints.
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                  &count) != KERN_SUCCESS) {
        return -1;
    }
    return static_cast<int64_t>(info.resident_size);
#else
    // statm's second number is the resident set, in pages.
    std::FILE* file = std::fopen("/proc/self/statm", "r");
    if (file == nullptr) {
        return -1;
    }
    long long size = 0;
    long long resident = 0;
    const int read = std::fscanf(file, "%lld %lld", &size, &resident);
    std::fclose(file);
    if (read != 2) {
        return -1;
    }
    return static_cast<int64_t>(resident) * static_cast<int64_t>(sysconf(_SC_PAGESIZE));
#endif
} catch (...) {
    fbl::escaped();
    return -1;
}
