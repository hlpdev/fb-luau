#include "core/core.hpp"
#include "fbl.h"

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <lua.h>
#include <lualib.h>
#include <Luau/Common.h>
#include <Luau/ExperimentalFlags.h>
#include <sstream>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

using version_t = const char* (*)();

// A function the built library exports, by its name, from the library loaded as a program loads it.
version_t exported(const char* name) {
#ifdef _WIN32
    HMODULE library = LoadLibraryA(FBL_LIBRARY);
    REQUIRE(library != nullptr);
    // GetProcAddress gives any function as one of its own type, which goes through C's generic
    // function type, as GCC asks.
    using any_function_t = void (*)();
    return reinterpret_cast<version_t>(
        reinterpret_cast<any_function_t>(GetProcAddress(library, name)));
#else
    void* library = dlopen(FBL_LIBRARY, RTLD_NOW | RTLD_LOCAL);
    REQUIRE(library != nullptr);
    // dlsym gives any function as a pointer to data.
    return reinterpret_cast<version_t>(dlsym(library, name));
#endif
}

} // namespace

TEST_CASE("the library says the package's version and Luau's") {
    CHECK(std::string_view(fbl_core_version()) == FBL_VERSION);
    CHECK(std::string_view(fbl_core_luau_version()) == FBL_LUAU_VERSION);
}

TEST_CASE("the built library loads, and exports its versions") {
    version_t version = exported("fbl_core_version");
    version_t luau = exported("fbl_core_luau_version");
    REQUIRE(version != nullptr);
    REQUIRE(luau != nullptr);
    CHECK(std::string_view(version()) == FBL_VERSION);
    CHECK(std::string_view(luau()) == FBL_LUAU_VERSION);
}

TEST_CASE("a failure is this thread's last error") {
    CHECK_FALSE(fbl::fail("the first"));
    CHECK(std::string_view(fbl_core_error()) == "the first");
    CHECK_FALSE(fbl::fail(std::string(100'000, 'x')));
    CHECK(std::strlen(fbl_core_error()) == 100'000);
}

TEST_CASE("Luau's flags are the luau CLI's, but its experimental ones") {
    fbl::set_flags();
    // Twice is once.
    fbl::set_flags();
    int seen = 0;
    for (const Luau::FValue<bool>* flag = Luau::FValue<bool>::list; flag != nullptr;
         flag = flag->next) {
        if (std::strncmp(flag->name, "Luau", 4) != 0) {
            continue;
        }
        INFO(flag->name);
        CHECK(flag->value == !Luau::isAnalysisFlagExperimental(flag->name));
        seen++;
    }
    CHECK(seen > 0);

    // The integer library is behind one, which a host without them never gets.
    lua_State* state = luaL_newstate();
    REQUIRE(state != nullptr);
    luaL_openlibs(state);
    lua_getglobal(state, "integer");
    CHECK(lua_istable(state, -1));
    lua_close(state);
}

TEST_CASE("a string stream works in the toolchain's C++ library") {
    // Luau's Analysis writes with them; a C++ library built for another C runtime than the one the
    // library's compiled for gets this wrong, as fbengine found.
    std::ostringstream stream;
    stream << "line " << 12 << ':' << 3.5;
    CHECK(stream.str() == "line 12:3.5");
}
