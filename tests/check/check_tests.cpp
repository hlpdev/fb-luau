#include "fbl.h"
#include "support.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
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

TEST_CASE("a type function that hoards ends at its heap limit, its memory back") {
    const checker_t checker(FBL_CHECK_STRICT, 30);
    const int64_t before = fbl_core_resident();
    // Each string its own, as string.rep's would be one string, interned, each time.
    const std::map<std::string, std::string> files = {
        {
            "hog.luau",
            "type function hog(t)\n"
            "    local k = {}\n"
            "    for i = 1, 1e9 do k[i] = string.rep('x', 1048576) .. i end\n"
            "    return t\n"
            "end\n"
            "type T = hog<number>\n",
        },
    };
    const std::vector<diagnostic_t> found = checked(checker, "hog", files);
    REQUIRE(found.size() == 1);
    CHECK(found[0].name == "TypeError");
    CHECK(found[0].message.contains("not enough memory"));
    const int64_t after = fbl_core_resident();
    if (before > 0 && after > 0) {
        CHECK(after - before < int64_t{ 256 } * 1024 * 1024);
    }
}

TEST_CASE("a type function that loops ends at the time limit") {
    const checker_t checker(FBL_CHECK_STRICT, 0.5);
    const std::map<std::string, std::string> files = {
        {
            "spin.luau",
            "type function spin(t)\n"
            "    while true do end\n"
            "    return t\n"
            "end\n"
            "type T = spin<number>\n",
        },
    };
    const std::vector<diagnostic_t> found = checked(checker, "spin", files);
    CHECK(count_named(found, "TimeLimit") == 1);
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
        // Each diagnostic's line, in any order: luau-analyze names a cycle's modules by its root's
        // name and by their full paths, so it gives them in an order of its own there.
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
