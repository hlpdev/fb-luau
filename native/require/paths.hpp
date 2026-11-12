#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fbl {

// The files a module path names, in the order they're looked for, as Luau's CLI has them.
inline constexpr std::array<std::string_view, 4> module_suffixes = {
    ".luau",
    ".lua",
    "/init.luau",
    "/init.lua",
};

// The most a require's path, or the walk to a module, has: bytes, and parts between its "/"s.
inline constexpr size_t most_path_bytes = 1024;
inline constexpr size_t most_path_parts = 64;

// A module path's parts joined as a file path's are.
std::string joined(const std::vector<std::string>& parts);

// Whether a part of a module's path is one the reader can be given: not empty, nor holding a
// ":", "\", or NUL, which a reader joining paths would take as a drive, a separator, or an end,
// nor a Windows device's name.
bool is_safe_part(std::string_view part);

// A file's path as a module path's parts from the root: "." left out, ".." going up, and a
// module's file named as its module, or nothing for a path that goes above the root, or has a
// part the reader can't be given.
std::optional<std::vector<std::string>> module_parts(std::string_view path);

// Whether parts are past what a require's path may be.
bool is_too_long(const std::vector<std::string>& parts);

// Where the .luaurc of the folder `parts` is would be.
std::string config_file(const std::vector<std::string>& parts);

} // namespace fbl
