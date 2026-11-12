// The checker: arbitrary bytes as a module's source, as a second module it requires, and as
// definitions, checked with a time limit, by a checker the input makes enforced or not, then,
// for some inputs, forgotten and checked again; each must finish with diagnostics, never crash or
// hang.

#include "fuzz.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace {

// Checks "main" until it wants nothing, giving it `main` and `other` as "main.luau" and "b.luau".
void check(uint64_t checker, std::string_view main, std::string_view other) {
    for (int round = 0; round < 64; round++) {
        const int32_t wanted = fbl_check_run(checker, "main");
        if (wanted <= 0) {
            break;
        }
        for (int32_t index = 0; index < wanted; index++) {
            const char* path = fbl_check_wanted(checker, index);
            if (path == nullptr) {
                finding("a wanted file has no path");
            }
            const std::string_view name = path;
            const std::string_view* text = nullptr;
            if (name == "main.luau") {
                text = &main;
            } else if (name == "b.luau") {
                text = &other;
            }
            const auto* bytes =
                text == nullptr ? nullptr : reinterpret_cast<const uint8_t*>(text->data());
            const int64_t length = text == nullptr ? 0 : static_cast<int64_t>(text->size());
            if (fbl_check_give(checker, path, length == 0 ? nullptr : bytes, length,
                               text == nullptr ? 0 : 1) != FBL_OK) {
                finding("a wanted file wasn't taken");
            }
        }
    }
    const int32_t count = fbl_check_count(checker);
    for (int32_t index = 0; index < count; index++) {
        for (int32_t which = 0; which < 3; which++) {
            if (fbl_check_text(checker, index, which) == nullptr) {
                finding("a diagnostic has no text");
            }
        }
        if (fbl_check_number(checker, index, 0) < 1) {
            finding("a diagnostic's line isn't from 1");
        }
    }
}

} // namespace

// NOLINTNEXTLINE(readability-identifier-naming): libFuzzer names it.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const no_escape_t watched;
    // The input is the main module, then, after its first NUL, the module it requires as "./b",
    // then, after the next, definitions.
    const std::string_view input(reinterpret_cast<const char*>(data), size);
    const size_t split = input.find('\0');
    const std::string_view main = input.substr(0, split);
    std::string_view other = split == std::string_view::npos ? "" : input.substr(split + 1);
    const size_t second = other.find('\0');
    const std::string_view definitions =
        second == std::string_view::npos ? "" : other.substr(second + 1);
    other = other.substr(0, second);
    const uint64_t checker =
        fbl_check_new(static_cast<int32_t>(size % 3), 0.25, int64_t{ 16 } * 1024 * 1024);
    if (checker == 0) {
        finding("a checker wasn't made");
    }
    if (fbl_check_enforce(checker, static_cast<int32_t>(size % 2)) != FBL_OK) {
        finding("a checker wasn't enforced");
    }
    if (!definitions.empty()) {
        const int32_t status =
            fbl_check_definitions(checker, reinterpret_cast<const uint8_t*>(definitions.data()),
                                  static_cast<int64_t>(definitions.size()), "fuzz");
        if (status != FBL_OK && status != FBL_SYNTAX && status != FBL_MEMORY) {
            finding("definitions gave what they don't say they give");
        }
    }
    check(checker, main, other);
    if (size % 5 == 0) {
        if (fbl_check_forget(checker) != FBL_OK) {
            finding("a checker didn't forget");
        }
        check(checker, main, other);
    }
    fbl_check_free(checker);
    return 0;
}
