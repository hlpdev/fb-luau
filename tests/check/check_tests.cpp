#include "fbl.h"
#include "support.hpp"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

// The type functions' heap limit the tests' checkers ask for, the package's default.
constexpr int64_t heap_limit = int64_t{ 64 } * 1024 * 1024;

struct diagnostic_t {
    std::string module;
    std::string name;
    std::string message;
    std::array<int32_t, 6> numbers{};
};

// A checker the test owns.
class checker_t {
public:
    explicit checker_t(int32_t mode = FBL_CHECK_NONSTRICT, double seconds = 60)
        : m_handle(fbl_check_new(mode, seconds, heap_limit)) {
        REQUIRE(m_handle != 0);
    }
    ~checker_t() {
        fbl_check_free(m_handle);
    }
    checker_t(const checker_t&) = delete;
    checker_t& operator=(const checker_t&) = delete;
    checker_t(checker_t&&) = delete;
    checker_t& operator=(checker_t&&) = delete;

    [[nodiscard]] uint64_t handle() const {
        return m_handle;
    }

private:
    uint64_t m_handle;
};

// Checks `module` over `files`, giving each file it wants, or none, until it's checked: its
// diagnostics, and in `asked` each path it asked for.
std::vector<diagnostic_t> checked(const checker_t& checker, const std::string& module,
                                  const std::map<std::string, std::string>& files,
                                  std::set<std::string>* asked = nullptr) {
    for (int round = 0; round < 100; round++) {
        const int32_t wanted = fbl_check_run(checker.handle(), module.c_str());
        INFO(fbl_core_error());
        REQUIRE(wanted >= 0);
        if (wanted == 0) {
            std::vector<diagnostic_t> out;
            const int32_t count = fbl_check_count(checker.handle());
            for (int32_t index = 0; index < count; index++) {
                diagnostic_t diagnostic;
                for (int32_t which = 0; which < 6; which++) {
                    diagnostic.numbers.at(static_cast<size_t>(which)) =
                        fbl_check_number(checker.handle(), index, which);
                }
                diagnostic.module = fbl_check_text(checker.handle(), index, 0);
                diagnostic.name = fbl_check_text(checker.handle(), index, 1);
                diagnostic.message = fbl_check_text(checker.handle(), index, 2);
                out.push_back(diagnostic);
            }
            for (const diagnostic_t& each : out) {
                UNSCOPED_INFO(each.module << ":" << each.numbers[0] << ":" << each.numbers[1] << " "
                                          << each.name << " " << each.message);
            }
            return out;
        }
        std::vector<std::string> paths;
        for (int32_t index = 0; index < wanted; index++) {
            const char* path = fbl_check_wanted(checker.handle(), index);
            REQUIRE(path != nullptr);
            paths.emplace_back(path);
        }
        for (const std::string& path : paths) {
            if (asked != nullptr) {
                asked->insert(path);
            }
            const auto found = files.find(path);
            const std::string_view text =
                found == files.end() ? std::string_view() : std::string_view(found->second);
            REQUIRE(fbl_check_give(checker.handle(), path.c_str(), bytes_of(text),
                                   static_cast<int64_t>(text.size()),
                                   found == files.end() ? 0 : 1) == FBL_OK);
        }
    }
    FAIL("the check never finished");
    return {};
}

// Checks `module` over `files` as `checked` does, without Catch2's macros, which aren't for other
// threads: how many diagnostics it gave, or -1.
int32_t count_quietly(uint64_t checker, const std::string& module,
                      const std::map<std::string, std::string>& files) {
    int32_t wanted = fbl_check_run(checker, module.c_str());
    for (int round = 0; round < 100 && wanted > 0; round++) {
        for (int32_t index = 0; index < wanted; index++) {
            const char* path = fbl_check_wanted(checker, index);
            if (path == nullptr) {
                return -1;
            }
            const auto found = files.find(path);
            const std::string_view text =
                found == files.end() ? std::string_view() : std::string_view(found->second);
            if (fbl_check_give(checker, path, bytes_of(text), static_cast<int64_t>(text.size()),
                               found == files.end() ? 0 : 1) != FBL_OK) {
                return -1;
            }
        }
        wanted = fbl_check_run(checker, module.c_str());
    }
    return wanted == 0 ? fbl_check_count(checker) : -1;
}

// Two modules that make the solver work, a type error in each: the first requires a module whose
// function it calls wrongly, the second a table it reads past.
std::map<std::string, std::string> first_files() {
    return {
        {
            "first.luau",
            "--!strict\nlocal util = require('./util')\nlocal n: number = "
            "util.open('x')\nprint(n)\n",
        },
        {
            "util.luau",
            "--!strict\nreturn { open = function(n: number): number return n + 1 end }\n",
        },
    };
}
std::map<std::string, std::string> second_files() {
    return {
        {
            "second.luau",
            "--!strict\nlocal shelf = require('./shelf')\nlocal s: string = "
            "shelf.count\nprint(s)\n",
        },
        { "shelf.luau", "--!strict\nreturn { count = 3, items = { 'a', 'b' } }\n" },
    };
}

// How many of `diagnostics` are named `name`.
size_t count_named(const std::vector<diagnostic_t>& diagnostics, std::string_view name) {
    size_t count = 0;
    for (const diagnostic_t& diagnostic : diagnostics) {
        count += diagnostic.name == name ? 1U : 0U;
    }
    return count;
}

} // namespace

TEST_CASE("a type error is given with its module, place, and code") {
    const checker_t checker;
    const std::vector<diagnostic_t> found = checked(
        checker, "main", { { "main.luau", "--!strict\nlocal x: number = 'a'\nprint(x)\n" } });
    REQUIRE(found.size() == 1);
    CHECK(found[0].module == "main.luau");
    CHECK(found[0].name == "TypeError");
    CHECK(found[0].message.contains("string"));
    CHECK(found[0].numbers[0] == 2);
    CHECK(found[0].numbers[1] == 19);
    CHECK(found[0].numbers[4] == FBL_CHECK_ERROR);
    CHECK(found[0].numbers[5] >= 1000);
}

TEST_CASE("a clean module gives nothing") {
    const checker_t checker(FBL_CHECK_STRICT);
    CHECK(checked(checker, "main", { { "main.luau", "local x: number = 1\nprint(x)\n" } }).empty());
}

TEST_CASE("a syntax error is a SyntaxError") {
    const checker_t checker;
    const std::vector<diagnostic_t> found =
        checked(checker, "main", { { "main.luau", "local = 1\n" } });
    REQUIRE(!found.empty());
    CHECK(found[0].name == "SyntaxError");
    CHECK(found[0].numbers[0] == 1);
}

TEST_CASE("a lint's warning is named as Luau names it") {
    const checker_t checker;
    const std::vector<diagnostic_t> found =
        checked(checker, "main", { { "main.luau", "local unused = 1\n" } });
    REQUIRE(count_named(found, "LocalUnused") == 1);
    for (const diagnostic_t& diagnostic : found) {
        if (diagnostic.name == "LocalUnused") {
            CHECK(diagnostic.numbers[4] == FBL_CHECK_WARNING);
        }
    }
}

TEST_CASE("modules are found as require finds them, and their types cross") {
    const checker_t checker(FBL_CHECK_STRICT);
    const std::map<std::string, std::string> files = {
        {
            "mods/doors/init.luau",
            "local util = require('@self/util')\nlocal lib = require('@lib/math')\n"
            "util.open('x')\nreturn lib.twice(2)\n",
        },
        { "mods/doors/util.luau", "return { open = function(n: number): number return n end }\n" },
        { "lib/math.luau", "return { twice = function(n: number): number return n * 2 end }\n" },
        { ".luaurc", R"({ "aliases": { "lib": "./lib" } })" },
    };
    std::set<std::string> asked;
    const std::vector<diagnostic_t> found = checked(checker, "mods/doors", files, &asked);
    REQUIRE(count_named(found, "TypeError") == 1);
    for (const diagnostic_t& diagnostic : found) {
        if (diagnostic.name == "TypeError") {
            CHECK(diagnostic.module == "mods/doors/init.luau");
            CHECK(diagnostic.numbers[0] == 3);
        }
    }
    CHECK(asked.contains("mods/doors/util.luau"));
    CHECK(asked.contains("lib/math.luau"));
    for (const std::string& path : asked) {
        CHECK(!path.contains(".."));
    }
}

TEST_CASE("nothing above the root is asked for") {
    const checker_t checker(FBL_CHECK_STRICT);
    std::set<std::string> asked;
    const std::vector<diagnostic_t> found =
        checked(checker, "main",
                { { "main.luau", "local x = require('../../secret')\nprint(x)\n" } }, &asked);
    CHECK(count_named(found, "TypeError") == 1);
    for (const std::string& path : asked) {
        CHECK(!path.contains("secret"));
    }
}

TEST_CASE("a module's mode is its own line, or the checker's") {
    const std::map<std::string, std::string> files = {
        { "main.luau", "local x: number = 'a'\n" },
    };
    const checker_t strict(FBL_CHECK_STRICT);
    CHECK(count_named(checked(strict, "main", files), "TypeError") == 1);
    const checker_t none(FBL_CHECK_NOCHECK);
    CHECK(count_named(checked(none, "main", files), "TypeError") == 0);
    const checker_t marked(FBL_CHECK_NOCHECK);
    CHECK(count_named(
              checked(marked, "main", { { "main.luau", "--!strict\nlocal x: number = 'a'\n" } }),
              "TypeError") == 1);
}

TEST_CASE("definitions are what every module sees") {
    const checker_t checker(FBL_CHECK_STRICT);
    const std::string_view definitions = "declare function spawn_item(name: string): number\n";
    REQUIRE(fbl_check_definitions(checker.handle(), bytes_of(definitions),
                                  static_cast<int64_t>(definitions.size()), "game") == FBL_OK);
    CHECK(checked(checker, "good",
                  { { "good.luau", "local n: number = spawn_item('key')\nprint(n)\n" } })
              .empty());
    const std::vector<diagnostic_t> bad =
        checked(checker, "bad", { { "bad.luau", "spawn_item(5)\n" } });
    CHECK(count_named(bad, "TypeError") == 1);
}

TEST_CASE("definitions that don't parse are a syntax error") {
    const checker_t checker;
    const std::string_view definitions = "declare function (\n";
    CHECK(fbl_check_definitions(checker.handle(), bytes_of(definitions),
                                static_cast<int64_t>(definitions.size()), "game") == FBL_SYNTAX);
    CHECK(std::string_view(fbl_core_error()).starts_with("game:1:"));
}

TEST_CASE("a file given again is checked again") {
    const checker_t checker;
    const std::map<std::string, std::string> files = {
        { "main.luau", "--!strict\nlocal x: number = 'a'\n" },
    };
    CHECK(count_named(checked(checker, "main", files), "TypeError") == 1);
    const std::string_view fixed = "--!strict\nlocal x: number = 1\n";
    REQUIRE(fbl_check_give(checker.handle(), "main.luau", bytes_of(fixed),
                           static_cast<int64_t>(fixed.size()), 1) == FBL_OK);
    CHECK(count_named(checked(checker, "main", files), "TypeError") == 0);
    REQUIRE(fbl_check_forget(checker.handle()) == FBL_OK);
    CHECK(count_named(checked(checker, "main", files), "TypeError") == 1);
}

TEST_CASE("a module past its time limit says so") {
    const checker_t checker(FBL_CHECK_STRICT, 1e-9);
    std::string source = "--!strict\n";
    for (int line = 0; line < 2000; line++) {
        source += "local v" + std::to_string(line) + " = { a = " + std::to_string(line) +
                  ", b = function(x: number) return x + 1 end }\n";
    }
    const std::vector<diagnostic_t> found = checked(checker, "big", { { "big.luau", source } });
    CHECK(count_named(found, "TimeLimit") == 1);
}

TEST_CASE("checkers made and run on many threads at once, first in a process, each work") {
    // Each test case runs in a process of its own, so this is the heap lock's first use: under
    // MinGW, a std::shared_mutex's first use on threads at once failed for some of them.
    std::vector<std::thread> threads;
    std::atomic<bool> go = false;
    std::atomic<int> worked = 0;
    threads.reserve(16);
    for (int each = 0; each < 16; each++) {
        threads.emplace_back([&go, &worked] {
            while (!go.load()) {
                std::this_thread::yield();
            }
            // Without Catch2's macros, which aren't for other threads.
            const uint64_t checker = fbl_check_new(FBL_CHECK_STRICT, 60, heap_limit);
            const std::string_view text = "local n: number = 'x'\nprint(n)\n";
            int32_t wanted = checker == 0 ? -1 : fbl_check_run(checker, "main");
            for (int round = 0; round < 10 && wanted > 0; round++) {
                for (int32_t index = 0; index < wanted; index++) {
                    const std::string path = fbl_check_wanted(checker, index);
                    const bool here = path == "main.luau";
                    fbl_check_give(checker, path.c_str(), here ? bytes_of(text) : nullptr,
                                   here ? static_cast<int64_t>(text.size()) : 0, here ? 1 : 0);
                }
                wanted = fbl_check_run(checker, "main");
            }
            if (wanted == 0 && fbl_check_count(checker) == 1) {
                worked++;
            }
            fbl_check_free(checker);
        });
    }
    go = true;
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK(worked == 16);
}

TEST_CASE("two checkers check on two threads at once, for a while, each right") {
    // Luau's Analysis writes the process's own as it checks, so checks on two threads take
    // turns; run under ThreadSanitizer, this is where one that didn't would show. Each forgets
    // and checks again, so each round parses and solves.
    REQUIRE(count_quietly(checker_t().handle(), "first", first_files()) == 1);
    REQUIRE(count_quietly(checker_t().handle(), "second", second_files()) == 1);
    std::atomic<bool> go = false;
    std::array<std::atomic<int>, 2> rounds{};
    std::array<std::atomic<int>, 2> wrong{};
    std::vector<std::thread> threads;
    threads.reserve(2);
    for (size_t each = 0; each < 2; each++) {
        threads.emplace_back([&go, &rounds, &wrong, each] {
            const uint64_t checker = fbl_check_new(FBL_CHECK_STRICT, 60, heap_limit);
            while (!go.load()) {
                std::this_thread::yield();
            }
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
            while (std::chrono::steady_clock::now() < until) {
                fbl_check_forget(checker);
                const int32_t count = each == 0 ? count_quietly(checker, "first", first_files())
                                                : count_quietly(checker, "second", second_files());
                rounds.at(each)++;
                wrong.at(each) += count == 1 ? 0 : 1;
            }
            fbl_check_free(checker);
        });
    }
    go = true;
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK(rounds[0] > 0);
    CHECK(rounds[1] > 0);
    CHECK(wrong[0] == 0);
    CHECK(wrong[1] == 0);
}

TEST_CASE("a check on one thread and a State's scripts on another each work") {
    // A State's VM and compiler and the checker's Analysis are two parts of Luau; under
    // ThreadSanitizer, this is where anything they share unlocked would show.
    std::atomic<bool> go = false;
    std::atomic<bool> checking = true;
    std::atomic<int> runs = 0;
    std::atomic<int> failed = 0;
    std::thread scripts([&go, &checking, &runs, &failed] {
        const fbl_state_options_t options = default_options();
        int32_t status = FBL_OTHER;
        const uint64_t state = fbl_state_new(&options, &status);
        if (state == 0) {
            failed++;
            return;
        }
        while (!go.load()) {
            std::this_thread::yield();
        }
        const std::string_view source =
            "local t = {} for i = 1, 200 do t[i] = tostring(i) .. 'x' end "
            "assert(#table.concat(t) > 0) local n: number = #t return n";
        do {
            const int32_t frame = fbl_frame_open(state);
            int32_t ran = fbl_load_source(state, frame, bytes_of(source),
                                          static_cast<int64_t>(source.size()), "=script");
            if (ran == FBL_OK) {
                ran = fbl_call_function(state, frame, 0, 0);
            }
            fbl_frame_close(state, frame);
            runs++;
            failed += ran == FBL_OK ? 0 : 1;
        } while (checking.load());
        fbl_state_close(state);
    });
    const checker_t checker(FBL_CHECK_STRICT);
    go = true;
    int checks = 0;
    int wrong = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < until) {
        fbl_check_forget(checker.handle());
        wrong += count_quietly(checker.handle(), "first", first_files()) == 1 ? 0 : 1;
        checks++;
    }
    checking = false;
    scripts.join();
    CHECK(checks > 0);
    CHECK(wrong == 0);
    CHECK(runs > 0);
    CHECK(failed == 0);
}

TEST_CASE("a check's modules share its time limit") {
    // A module whose time is the solver's, its operators' type functions, timed whole on this
    // machine, then eight of them under half that: given the limit each, as Luau gives it, the
    // check would take four times its time; with one deadline, about half.
    std::string slow = "--!strict\nlocal function g(a, b)\n    local x0 = a\n";
    for (int line = 1; line < 200; line++) {
        slow += "    local x" + std::to_string(line) + " = x" + std::to_string(line - 1) +
                " + b * x" + std::to_string(line - 1) + "\n";
    }
    slow += "    return x199\nend\nreturn g\n";
    const checker_t timing(FBL_CHECK_STRICT, 600);
    const auto started = std::chrono::steady_clock::now();
    CHECK(count_named(checked(timing, "slow", { { "slow.luau", slow } }), "TimeLimit") == 0);
    const std::chrono::duration<double> whole = std::chrono::steady_clock::now() - started;

    const double limit = whole.count() / 2;
    const checker_t checker(FBL_CHECK_STRICT, limit);
    std::map<std::string, std::string> files;
    std::string main = "--!strict\n";
    for (int module = 0; module < 8; module++) {
        const std::string name = "m" + std::to_string(module);
        files[name + ".luau"] = slow;
        main += "local " + name;
        main += " = require('./" + name;
        main += "')\nprint(" + name;
        main += ")\n";
    }
    files["main.luau"] = main;
    const auto checking = std::chrono::steady_clock::now();
    const std::vector<diagnostic_t> found = checked(checker, "main", files);
    const std::chrono::duration<double> took = std::chrono::steady_clock::now() - checking;
    REQUIRE(found.size() == 1);
    CHECK(found[0].name == "TimeLimit");
    CHECK(found[0].module == "main.luau");
    CHECK(took.count() < limit * 4);
}

TEST_CASE("a check that reaches a type function is refused, and none is run") {
    // Each would run past what a limit holds: memory, time, the host's strings and arena, and
    // the native stack.
    const std::array<std::string_view, 5> bodies = {
        "local k = {} for i = 1, 1e9 do k[i] = string.rep('x', 1048576) .. i end",
        "while true do end",
        "local s = string.rep('x', 33554432) for i = 1, 1e9 do print(s) end",
        "for i = 1, 1e9 do types.unionof(types.singleton(tostring(i)), types.number) end",
        "local function deep(n) if n > 0 then string.gsub('x', 'x', function() deep(n - 1) "
        "end) "
        "end end deep(1000)",
    };
    for (const std::string_view body : bodies) {
        INFO(body);
        const checker_t checker(FBL_CHECK_STRICT, 30);
        const int64_t before = fbl_core_resident();
        const auto started = std::chrono::steady_clock::now();
        const std::map<std::string, std::string> files = {
            { "mod.luau", "local other = require('./other')\nprint(other)\n" },
            {
                "other.luau",
                "type function f(t)\n    " + std::string(body) + "\n    return t\nend\n" +
                    "type T = f<number>\nlocal x: number = 'not a number'\nreturn 1\n",
            },
        };
        const std::vector<diagnostic_t> found = checked(checker, "mod", files);
        REQUIRE(found.size() == 1);
        CHECK(found[0].name == "TypeFunction");
        CHECK(found[0].module == "other.luau");
        CHECK(found[0].numbers[0] == 1);
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
        const int64_t after = fbl_core_resident();
        if (before > 0 && after > 0) {
            CHECK(after - before < int64_t{ 64 } * 1024 * 1024);
        }
    }
}

TEST_CASE("an enforced checker's mode and lint stand whatever a module's files say") {
    const std::map<std::string, std::string> files = {
        {
            "mods/a.luau",
            "--!nocheck\n--!nolint\nlocal n: number = 'x'\nlocal unused = 1\nprint(n)\n",
        },
        {
            "mods/b.luau",
            "local s = require('@shared/s')\nlocal n: number = 'x'\nlocal unused = 1\n"
            "print(n, s, y)\n",
        },
        { "mods/shared/s.luau", "return 1\n" },
        {
            "mods/.luaurc",
            R"({ "languageMode": "nocheck", "lint": { "*": false }, "lintErrors": false, )"
            R"("globals": ["y"], "aliases": { "shared": "./shared" } })",
        },
    };
    for (const std::string& module : { std::string("mods/a"), std::string("mods/b") }) {
        INFO(module);
        const checker_t own(FBL_CHECK_STRICT);
        CHECK(checked(own, module, files).empty());

        const checker_t enforced(FBL_CHECK_STRICT);
        REQUIRE(fbl_check_enforce(enforced.handle(), 1) == FBL_OK);
        const std::vector<diagnostic_t> found = checked(enforced, module, files);
        CHECK(count_named(found, "TypeError") == (module == "mods/a" ? 1 : 2));
        CHECK(count_named(found, "LocalUnused") == 1);
        for (const diagnostic_t& each : found) {
            CHECK(!each.message.contains("require"));
        }

        // And off again, as luau-analyze has it.
        REQUIRE(fbl_check_enforce(enforced.handle(), 0) == FBL_OK);
        CHECK(checked(enforced, module, files).empty());
    }
    CHECK(fbl_check_enforce(0, 1) == FBL_MISUSE);
}

TEST_CASE("a .luaurc that doesn't parse is a ConfigError") {
    const checker_t checker;
    const std::vector<diagnostic_t> found =
        checked(checker, "main", { { "main.luau", "return 1\n" }, { ".luaurc", "{ not json" } });
    CHECK(count_named(found, "ConfigError") == 1);
}

TEST_CASE("misuse is refused, saying why") {
    CHECK(fbl_check_new(3, 1, heap_limit) == 0);
    CHECK(fbl_check_new(FBL_CHECK_STRICT, -1, heap_limit) == 0);
    CHECK(fbl_check_new(FBL_CHECK_STRICT, 0, heap_limit) == 0);
    CHECK(fbl_check_new(FBL_CHECK_STRICT, 1, 0) == 0);
    CHECK(fbl_check_new(FBL_CHECK_STRICT, 1, int64_t{ 1024 } * 1024 * 1024 * 1024) == 0);
    CHECK(fbl_check_run(12345678, "main") == -1);
    CHECK(fbl_check_count(12345678) == -1);
    fbl_check_free(12345678);
    const checker_t checker;
    CHECK(fbl_check_run(checker.handle(), nullptr) == -1);
    CHECK(fbl_check_run(checker.handle(), "../up") == -1);
    CHECK(fbl_check_run(checker.handle(), "") == -1);
    CHECK(fbl_check_wanted(checker.handle(), 0) == nullptr);
    CHECK(fbl_check_text(checker.handle(), 0, 0) == nullptr);
    CHECK(fbl_check_number(checker.handle(), 0, 0) == -1);
    CHECK(fbl_check_give(checker.handle(), nullptr, nullptr, 0, 0) == FBL_MISUSE);
    // No module, and two files of one.
    for (const std::string_view path :
         { "none.luau", "none.lua", "none/init.luau", "none/init.lua" }) {
        REQUIRE(fbl_check_give(checker.handle(), std::string(path).c_str(), nullptr, 0, 0) ==
                FBL_OK);
    }
    CHECK(fbl_check_run(checker.handle(), "none") == -1);
    CHECK(std::string_view(fbl_core_error()).contains("no module"));
    for (const std::string_view path : { "two.luau", "two/init.luau" }) {
        REQUIRE(fbl_check_give(checker.handle(), std::string(path).c_str(), bytes_of("return 1"), 8,
                               1) == FBL_OK);
    }
    for (const std::string_view path : { "two.lua", "two/init.lua" }) {
        REQUIRE(fbl_check_give(checker.handle(), std::string(path).c_str(), nullptr, 0, 0) ==
                FBL_OK);
    }
    CHECK(fbl_check_run(checker.handle(), "two") == -1);
    CHECK(std::string_view(fbl_core_error()).contains("2 files"));
}

TEST_CASE("a freed checker's handle isn't one") {
    const uint64_t handle = fbl_check_new(FBL_CHECK_STRICT, 1, heap_limit);
    REQUIRE(handle != 0);
    fbl_check_free(handle);
    CHECK(fbl_check_count(handle) == -1);
}

TEST_CASE("each case of the corpus gives what luau-analyze gives") {
    const std::filesystem::path corpus = FBL_CHECK_CORPUS;
    size_t cases = 0;
    for (const auto& entry : std::filesystem::directory_iterator(corpus)) {
        if (!entry.is_directory()) {
            continue;
        }
        cases++;
        const std::filesystem::path& folder = entry.path();
        std::map<std::string, std::string> files;
        std::string expected;
        for (const auto& file : std::filesystem::recursive_directory_iterator(folder)) {
            if (!file.is_regular_file()) {
                continue;
            }
            std::ifstream in(file.path(), std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
            const std::string path = file.path().lexically_relative(folder).generic_string();
            if (path == "expected.txt") {
                expected = std::move(text);
            } else {
                files.emplace(path, std::move(text));
            }
        }
        const checker_t checker;
        // Each diagnostic's line, in any order: luau-analyze names a cycle's modules by its
        // root's name and by their full paths, so it gives them in an order of its own there.
        std::multiset<std::string> got;
        for (const diagnostic_t& diagnostic : checked(checker, "main", files)) {
            got.insert(diagnostic.module + ":" + std::to_string(diagnostic.numbers[0]) + ":" +
                       std::to_string(diagnostic.numbers[1]) + ": " + diagnostic.name + ": " +
                       diagnostic.message);
        }
        std::multiset<std::string> wanted;
        size_t start = 0;
        while (start < expected.size()) {
            const size_t end = expected.find('\n', start);
            wanted.insert(expected.substr(start, end - start));
            start = end == std::string::npos ? expected.size() : end + 1;
        }
        INFO(folder.filename().string());
        CHECK(got == wanted);
    }
    CHECK(cases >= 8);
}
