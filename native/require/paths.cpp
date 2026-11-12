#include "require/paths.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Whether `name`, without what follows its first ".", is one of Windows's devices, which a reader
// that opens files would open as one.
bool is_device(std::string_view name) {
    const std::string_view base = name.substr(0, name.find('.'));
    std::string upper;
    for (const char each : base) {
        upper += static_cast<char>(each >= 'a' && each <= 'z' ? each - 'a' + 'A' : each);
    }
    for (const std::string_view device : { "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$" }) {
        if (upper == device) {
            return true;
        }
    }
    return upper.size() == 4 && (upper.starts_with("COM") || upper.starts_with("LPT")) &&
           upper[3] >= '0' && upper[3] <= '9';
}

} // namespace

namespace fbl {

std::string joined(const std::vector<std::string>& parts) {
    std::string path;
    for (const std::string& part : parts) {
        if (!path.empty()) {
            path += '/';
        }
        path += part;
    }
    return path;
}

bool is_safe_part(std::string_view part) {
    return !part.empty() &&
           part.find_first_of(std::string_view(":\\\0", 3)) == std::string_view::npos &&
           !is_device(part);
}

std::optional<std::vector<std::string>> module_parts(std::string_view path) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find_first_of("/\\", start);
        if (end == std::string_view::npos) {
            end = path.size();
        }
        const std::string_view part = path.substr(start, end - start);
        if (part == "..") {
            if (parts.empty()) {
                return std::nullopt;
            }
            parts.pop_back();
        } else if (!part.empty() && part != ".") {
            if (!is_safe_part(part)) {
                return std::nullopt;
            }
            parts.emplace_back(part);
        }
        start = end + 1;
    }
    if (!parts.empty()) {
        std::string& last = parts.back();
        for (const std::string_view suffix :
             { std::string_view(".luau"), std::string_view(".lua") }) {
            if (last.size() > suffix.size() && last.ends_with(suffix)) {
                last.resize(last.size() - suffix.size());
                break;
            }
        }
        if (last == "init") {
            parts.pop_back();
        }
    }
    return parts;
}

bool is_too_long(const std::vector<std::string>& parts) {
    size_t bytes = 0;
    for (const std::string& part : parts) {
        bytes += part.size() + 1;
    }
    return parts.size() > most_path_parts || bytes > most_path_bytes;
}

std::string config_file(const std::vector<std::string>& parts) {
    const std::string folder = joined(parts);
    return folder.empty() ? ".luaurc" : folder + "/.luaurc";
}

} // namespace fbl
