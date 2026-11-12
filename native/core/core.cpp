#include "core/core.hpp"

#include "fbl.h"

#include <atomic>
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
#include <pthread.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

namespace {

thread_local std::string last_error;

// How many exceptions got to the edge, which fbl_core_escaped gives.
std::atomic<int64_t> escapes{ 0 };

// Where this thread's native stack ends, its lowest address, or 0 when the platform doesn't say.
uintptr_t stack_end() noexcept {
#ifdef _WIN32
    // The stack is one reservation, and its base is where it ends.
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(&region, &region, sizeof(region)) == 0) {
        return 0;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the stack's place, as a number.
    return reinterpret_cast<uintptr_t>(region.AllocationBase);
#elifdef __APPLE__
    pthread_t self = pthread_self();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the stack's place, as a number.
    const auto top = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(self));
    const size_t size = pthread_get_stacksize_np(self);
    return top > size ? top - size : 0;
#else
    pthread_attr_t attributes;
    if (pthread_getattr_np(pthread_self(), &attributes) != 0) {
        return 0;
    }
    void* low = nullptr;
    size_t size = 0;
    const int got = pthread_attr_getstack(&attributes, &low, &size);
    pthread_attr_destroy(&attributes);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the stack's place, as a number.
    return got == 0 ? reinterpret_cast<uintptr_t>(low) : 0;
#endif
}

} // namespace

namespace fbl {

uintptr_t stack_floor(uintptr_t reserve) noexcept {
    // Linux finds the main thread's from /proc, so it's found once a thread.
    thread_local const uintptr_t end = stack_end();
    return end == 0 ? 0 : end + reserve;
}

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
    escapes.fetch_add(1, std::memory_order_relaxed);
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

int64_t fbl_core_escaped(void) {
    return escapes.load(std::memory_order_relaxed);
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
