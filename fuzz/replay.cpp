// A fuzzer's driver where libFuzzer isn't: runs LLVMFuzzerTestOneInput over each file of the
// folders it's given, then over random inputs, each a corpus file's bytes changed at random, or
// bytes of its own, from a fixed seed, so a run is the same each time. A finding is an abort, as
// the fuzzer's checks make it; a run that ends says how many inputs it ran.
//
//     fbl_fuzz_<name> <corpus folder>... [--random <count>]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// NOLINTNEXTLINE(readability-identifier-naming): libFuzzer names it.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

namespace {

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

void run(const std::vector<uint8_t>& input) {
    LLVMFuzzerTestOneInput(input.data(), input.size());
}

} // namespace

int main(int count, char** given) try {
    const std::span<char*> arguments(given, static_cast<size_t>(count));
    size_t randoms = 2000;
    std::vector<std::vector<uint8_t>> corpus;
    for (size_t index = 1; index < arguments.size(); index++) {
        const std::string_view argument = arguments[index];
        if (argument == "--random" && index + 1 < arguments.size()) {
            randoms = std::strtoull(arguments[++index], nullptr, 10);
            continue;
        }
        const std::filesystem::path folder(argument);
        if (!std::filesystem::is_directory(folder)) {
            continue;
        }
        for (const auto& entry : std::filesystem::recursive_directory_iterator(folder)) {
            if (entry.is_regular_file()) {
                corpus.push_back(read_file(entry.path()));
            }
        }
    }
    size_t inputs = 0;
    for (const std::vector<uint8_t>& input : corpus) {
        run(input);
        inputs++;
    }
    // A fixed seed, so a run is the same each time, and what it finds is found again.
    // NOLINTNEXTLINE(bugprone-random-generator-seed)
    std::mt19937_64 random(0x5EED);
    for (size_t each = 0; each < randoms; each++) {
        std::vector<uint8_t> input;
        if (!corpus.empty() && random() % 4 != 0) {
            input = corpus[random() % corpus.size()];
            const size_t changes = 1 + (random() % 8);
            for (size_t change = 0; change < changes && !input.empty(); change++) {
                const size_t at = random() % input.size();
                switch (random() % 4) {
                case 0:
                    input[at] = static_cast<uint8_t>(random());
                    break;
                case 1:
                    input.erase(input.begin() + static_cast<std::ptrdiff_t>(at));
                    break;
                case 2:
                    input.insert(input.begin() + static_cast<std::ptrdiff_t>(at),
                                 static_cast<uint8_t>(random()));
                    break;
                default:
                    input.resize(at);
                    break;
                }
            }
        } else {
            input.resize(random() % 256);
            for (uint8_t& byte : input) {
                byte = static_cast<uint8_t>(random());
            }
        }
        run(input);
        inputs++;
    }
    std::fputs(std::format("{} inputs, no finding\n", inputs).c_str(), stdout);
    return 0;
} catch (const std::exception& error) {
    std::fputs("the replay failed: ", stderr);
    std::fputs(error.what(), stderr);
    std::fputs("\n", stderr);
    return 1;
} catch (...) {
    std::fputs("the replay failed\n", stderr);
    return 1;
}
