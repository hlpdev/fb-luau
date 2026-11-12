#include "fbl.h"
#include "support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace {

// The program's functions, as the test's callback has them, by number.
constexpr int64_t reader = 1;
constexpr int64_t forget = 2;
constexpr int64_t attempt = 3;

// How many times `attempt` has been called.
int attempts = 0;

// The modules' files the reader gives, by their paths, and the paths it was asked for.
std::map<std::string, std::string, std::less<>>& files_of() {
    static std::map<std::string, std::string, std::less<>> files;
    return files;
}

std::map<std::string, int, std::less<>>& reads_of() {
    static std::map<std::string, int, std::less<>> reads;
    return reads;
}

int32_t test_callback(uint64_t state, int32_t frame, int64_t function) {
    switch (function) {
    case reader: {
        const uint8_t* bytes = nullptr;
        int64_t length = 0;
        bytes = fbl_frame_string(state, frame, 0, &length);
        const std::string path = bytes == nullptr ? std::string() : text_of(bytes, length);
        fbl_frame_truncate(state, frame, 0);
        reads_of()[path]++;
        if (path == "broken.luau") {
            fbl_frame_push_string(state, frame, bytes_of("the disk is gone"), 16);
            return FBL_RUNTIME;
        }
        // A reader that runs a script's code that requires, in the middle of a walk.
        if (path == "nested.luau") {
            const int32_t inner = fbl_frame_open(state);
            if (fbl_frame_get_global(state, inner, bytes_of("inner"), 5) == FBL_OK) {
                fbl_call_function(state, inner, 0, 0);
            }
            fbl_frame_close(state, inner);
        }
        const auto found = files_of().find(path);
        if (found != files_of().end()) {
            fbl_frame_push_string(state, frame, bytes_of(found->second),
                                  static_cast<int64_t>(found->second.size()));
        }
        return FBL_OK;
    }
    case forget:
        fbl_frame_truncate(state, frame, 0);
        return fbl_require_forget(state);
    case attempt:
        fbl_frame_truncate(state, frame, 0);
        fbl_frame_push_number(state, frame, ++attempts);
        return FBL_OK;
    default:
        return FBL_OTHER;
    }
}

// A State whose require reads `files`, unless it's made without a reader.
struct require_state_t : test_state_t {
    explicit require_state_t(bool with_reader = true,
                             const fbl_state_options_t& options = default_options())
        : test_state_t(options) {
        REQUIRE(fbl_callback_set(test_callback));
        reads_of().clear();
        int32_t frame = fbl_frame_open(handle);
        REQUIRE(fbl_callback_function(handle, frame, forget, "forget") == FBL_OK);
        REQUIRE(fbl_frame_set_global(handle, frame, bytes_of("forget"), 6, 0) == FBL_OK);
        REQUIRE(fbl_callback_function(handle, frame, attempt, "attempt") == FBL_OK);
        REQUIRE(fbl_frame_set_global(handle, frame, bytes_of("attempt"), 7, 1) == FBL_OK);
        REQUIRE(fbl_frame_close(handle, frame));
        attempts = 0;
        frame = fbl_frame_open(handle);
        if (with_reader) {
            REQUIRE(fbl_callback_function(handle, frame, reader, "modules") == FBL_OK);
        }
        REQUIRE(fbl_require_open(handle, frame, with_reader ? 0 : -1) == FBL_OK);
        REQUIRE(fbl_frame_close(handle, frame));
    }
};

} // namespace

TEST_CASE("require finds modules from the requiring module, as Luau's rules have it") {
    files_of() = {
        { "doors.luau", "return { name = 'doors' }" },
        {
            "lib/init.luau",
            "local util = require('@self/util') return { name = 'lib ' .. util.name }",
        },
        { "lib/util.lua", "return { name = 'util' }" },
        { "lib/deep/a.luau", "return { name = 'a ' .. require('../../doors').name }" },
    };
    const require_state_t lua;
    CHECK(lua.run("assert(require('./doors').name == 'doors')\n"
                  "assert(require('./lib').name == 'lib util')\n"
                  "assert(require('./lib/deep/a').name == 'a doors')",
                  "@main.luau") == FBL_OK);
    // A chunk of another name requires from the root.
    CHECK(lua.run("assert(require('./doors').name == 'doors')", "=chunk") == FBL_OK);
    // From a module's own path.
    CHECK(lua.run("assert(require('../doors').name == 'doors')", "@lib/main.luau") == FBL_OK);
}

TEST_CASE("a .luaurc's aliases are the modules' its folder has") {
    files_of() = {
        { ".luaurc", R"({ "aliases": { "shared": "./common" } })" },
        { "common/util.luau", "return 'common util'" },
        { "mods/.luaurc", R"({ "aliases": { "shared": "../other" } })" },
        { "other/util.luau", "return 'other util'" },
    };
    const require_state_t lua;
    CHECK(lua.run("assert(require('@shared/util') == 'common util')", "@main.luau") == FBL_OK);
    CHECK(lua.run("assert(require('@shared/util') == 'other util')", "@mods/main.luau") == FBL_OK);
    CHECK(lua.run("require('@missing/util')", "@main.luau") == FBL_RUNTIME);
}

TEST_CASE("a module runs once, its value kept, until the modules are forgotten") {
    files_of() = { { "once.luau", "return {}" } };
    const require_state_t lua;
    CHECK(lua.run("local a = require('./once') assert(a == require('./once'))\n"
                  "forget() assert(require('./once') ~= a)",
                  "@main.luau") == FBL_OK);
}

TEST_CASE("a module reaches only what the program gives") {
    files_of() = { { "doors.luau", "return 1" } };
    const require_state_t lua;
    CHECK(lua.run("require('../doors')", "@main.luau") == FBL_RUNTIME);
    CHECK(lua.run("require('./nothing')", "@main.luau") == FBL_RUNTIME);
    CHECK(lua.error_message().contains("no module present at resolved path"));
    CHECK(lua.run("require('../../etc/passwd')", "@lib/main.luau") == FBL_RUNTIME);
    CHECK(lua.run("require('./doors')", "@../outside.luau") == FBL_RUNTIME);
    // Every path it asked for is under the root.
    for (const auto& [path, count] : reads_of()) {
        CHECK_FALSE(path.starts_with(".."));
        CHECK_FALSE(path.starts_with('/'));
    }
}

TEST_CASE("a module's globals are its own, and the State is frozen by its first") {
    files_of() = { { "leaky.luau", "leaked = 1 return leaked" } };
    const require_state_t lua;
    CHECK(fbl_sandbox_is_frozen(lua.handle) == 0);
    CHECK(lua.run("assert(require('./leaky') == 1) assert(leaked == nil)", "@main.luau") == FBL_OK);
    CHECK(fbl_sandbox_is_frozen(lua.handle) == 1);
}

TEST_CASE("a module's errors are each require's") {
    files_of() = {
        { "boom.luau", "error('boom')" },
        { "two.luau", "return 1, 2" },
        { "waits.luau", "coroutine.yield() return 1" },
        { "both.luau", "return 1" },
        { "both/init.luau", "return 2" },
        { "bad.luau", "return +" },
    };
    const require_state_t lua;
    CHECK(lua.run("for i = 1, 2 do local ok, e = pcall(require, './boom')\n"
                  "assert(not ok and e:find('boom.luau:1: boom')) end",
                  "@main.luau") == FBL_OK);
    CHECK(lua.run("require('./two')", "@main.luau") == FBL_RUNTIME);
    CHECK(lua.error_message().contains("gave 2 values, not one"));
    CHECK(lua.run("require('./both')", "@main.luau") == FBL_RUNTIME);
    CHECK(lua.run("require('./bad')", "@main.luau") == FBL_RUNTIME);
    CHECK(lua.run("require('./broken')", "@main.luau") == FBL_RUNTIME);
    CHECK(lua.error_message().contains("the disk is gone"));
    CHECK(lua.run("require('./waits')", "@main.luau") == FBL_YIELD);
    CHECK(lua.error_message().contains("module 'waits.luau' yielded as it loaded"));
    // Nothing can wait for one as it loads, so a wait is refused there.
    CHECK(lua.run("coroutine.wrap(function() require('./waits') end)()", "@main.luau") ==
          FBL_YIELD);
}

TEST_CASE("without a reader, require says the program gives no modules") {
    const require_state_t lua(false);
    CHECK(lua.run("require('./doors')", "@main.luau") == FBL_RUNTIME);
    CHECK(lua.error_message().contains("the program gives no modules"));
}

TEST_CASE("require is opened once, from a function") {
    const require_state_t lua;
    const int32_t frame = fbl_frame_open(lua.handle);
    CHECK(fbl_require_open(lua.handle, frame, -1) == FBL_MISUSE);
    REQUIRE(fbl_frame_push_number(lua.handle, frame, 1) == FBL_OK);
    CHECK(fbl_require_open(lua.handle, frame, 0) == FBL_MISUSE);
    CHECK(fbl_require_open(lua.handle, frame, 4) == FBL_MISUSE);
    CHECK(fbl_frame_close(lua.handle, frame));
    CHECK(fbl_require_forget(0) == FBL_MISUSE);
}

TEST_CASE("a require's path past 64 parts or 1,024 bytes is refused before anything's read") {
    files_of() = { { "b.luau", "return 1" } };
    const require_state_t lua;
    CHECK(lua.run("require('./' .. string.rep('a/', 20000) .. 'b')", "@main.luau") == FBL_RUNTIME);
    CHECK(lua.error_message().contains("at most 64 parts and 1024 bytes"));
    CHECK(reads_of().empty());
    // At each limit, it's walked; one past, it isn't.
    CHECK(lua.run("require('./' .. string.rep('a/', 62) .. 'b')", "@main.luau") == FBL_RUNTIME);
    CHECK(lua.error_message().contains("no module present"));
    CHECK_FALSE(reads_of().empty());
    reads_of().clear();
    CHECK(lua.run("require('./' .. string.rep('a/', 63) .. 'b')", "@main.luau") == FBL_RUNTIME);
    CHECK(lua.error_message().contains("at most 64 parts"));
    CHECK(reads_of().empty());
    CHECK(lua.run("require('./' .. string.rep('x', 1022))", "@main.luau") == FBL_RUNTIME);
    CHECK(lua.error_message().contains("no module present"));
    reads_of().clear();
    CHECK(lua.run("require('./' .. string.rep('x', 1023))", "@main.luau") == FBL_RUNTIME);
    CHECK(lua.error_message().contains("at most 64 parts and 1024 bytes"));
    CHECK(reads_of().empty());
}

TEST_CASE("a require's path has no part a reader could take as a drive, a device, or an end") {
    files_of() = { { "b.luau", "return 1" } };
    const require_state_t lua;
    for (const char* path : { "./C:/Users/x/y", "./a\\b", "./con", "./COM1.txt", "./aux/b" }) {
        reads_of().clear();
        CHECK(lua.run(std::string("require('") + path + "')", "@main.luau") == FBL_RUNTIME);
        for (const auto& [read, count] : reads_of()) {
            CHECK_FALSE(read.contains(':'));
            CHECK_FALSE(read.contains('\\'));
        }
    }
    CHECK(lua.run("require('./b' .. string.char(0))", "@main.luau") == FBL_RUNTIME);
}

TEST_CASE("an exporting module that gives what isn't a table is an error, and loads anew") {
    files_of() = {
        { "a.luau", "loadstring('return')\n"
                    "table = { freeze = function() return 0 end }\n"
                    "local b = require('./b')\n"
                    "export local x = 1" },
        { "b.luau", "local a = require('./a')\nexport local y = 2" },
        { "ok.luau", "export local z = 3" },
    };
    fbl_state_options_t options = default_options();
    options.load_string = true;
    const require_state_t lua(true, options);
    CHECK(lua.run("require('./a')", "@main.luau") == FBL_RUNTIME);
    CHECK(lua.run("assert(require('./ok').z == 3)", "@main.luau") == FBL_OK);
}

TEST_CASE("an exporting module that errors once loads at its next require") {
    files_of() = { { "shared.luau", "if attempt() == 1 then error('not yet') end\n"
                                    "export local x = 1" } };
    const require_state_t lua;
    CHECK(lua.run("local ok = pcall(require, './shared') assert(not ok)", "@main.luau") == FBL_OK);
    CHECK(lua.run("assert(require('./shared').x == 1)", "@main.luau") == FBL_OK);
}

TEST_CASE("a module's table is frozen through its fields for every script") {
    files_of() = { { "util.luau", "return { check = function() return false end, inner = {} }" } };
    const require_state_t lua;
    CHECK(lua.run("require('./util').check = function() return true end", "@main.luau") ==
          FBL_RUNTIME);
    CHECK(lua.run("require('./util').inner.x = 1", "@main.luau") == FBL_RUNTIME);
    CHECK(lua.run("assert(require('./util').check() == false)", "@main.luau") == FBL_OK);
}

TEST_CASE("the modules aren't forgotten while one loads") {
    files_of() = { { "forgetful.luau", "local ok = pcall(forget) assert(not ok) export local x = 1" } };
    const require_state_t lua;
    CHECK(lua.run("assert(require('./forgetful').x == 1)", "@main.luau") == FBL_OK);
    CHECK(lua.run("forget() assert(require('./forgetful').x == 1)", "@main.luau") == FBL_OK);
}

TEST_CASE("a reader that runs code that requires doesn't lose the walk it's in") {
    files_of() = { { "nested.luau", "return 'outer'" }, { "other.luau", "return 'inner'" } };
    const require_state_t lua;
    REQUIRE(lua.run("function inner() innerResult = require('./other') end", "@main.luau") ==
            FBL_OK);
    CHECK(lua.run("assert(require('./nested') == 'outer')", "@main.luau") == FBL_OK);
}
