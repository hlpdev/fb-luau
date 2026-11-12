#include "check/fbl_check.h"
#include "core/core.hpp"
#include "require/paths.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <limits>
#include <Luau/Ast.h>
#include <Luau/BuiltinDefinitions.h>
#include <Luau/Cancellation.h>
#include <Luau/Common.h>
#include <Luau/Config.h>
#include <Luau/ConfigResolver.h>
#include <Luau/Error.h>
#include <Luau/FileResolver.h>
#include <Luau/Frontend.h>
#include <Luau/LinterConfig.h>
#include <Luau/RequireNavigator.h>
#include <Luau/TypeArena.h>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Luau's heap limit for type functions' VMs, the process's.
// NOLINTNEXTLINE(readability-identifier-naming): Luau's name.
LUAU_FASTINT(DebugLuauTypeFunctionRuntimeHeapLimit)

namespace {

// The most files a checker is given, past which it's refused: a check that wants more is a
// module graph no mod has.
constexpr size_t most_files = 10000;

// The files a checker's been given, and those its check wanted that it hasn't been.
class files_t {
public:
    // The file at `path`: its text, or nothing when there's none, or when it hasn't been given,
    // which the check then wants.
    std::optional<std::string> read(const std::string& path) {
        const auto found = m_known.find(path);
        if (found != m_known.end()) {
            return found->second;
        }
        if (m_wanted_set.insert(path).second) {
            m_wanted.push_back(path);
        }
        return std::nullopt;
    }

    // Whether `path` has been given, as a file or as none.
    [[nodiscard]] bool knows(const std::string& path) const {
        return m_known.contains(path);
    }

    bool give(const std::string& path, std::optional<std::string> text) {
        if (!m_known.contains(path) && m_known.size() >= most_files) {
            return false;
        }
        m_known.insert_or_assign(path, std::move(text));
        return true;
    }

    void forget() {
        m_known.clear();
        clear_wanted();
    }

    void clear_wanted() {
        m_wanted.clear();
        m_wanted_set.clear();
    }

    [[nodiscard]] const std::vector<std::string>& wanted() const {
        return m_wanted;
    }

private:
    std::unordered_map<std::string, std::optional<std::string>> m_known;
    std::vector<std::string> m_wanted;
    std::unordered_set<std::string> m_wanted_set;
};

// Finds a require's module over the files, by the rules the VM's require has (require.cpp).
class navigation_t : public Luau::Require::NavigationContext {
public:
    navigation_t(files_t& files, std::string requirer)
        : m_files(files), m_requirer(std::move(requirer)) {}

    NavigateResult resetToRequirer() override {
        m_file.clear();
        std::optional<std::vector<std::string>> parts = fbl::module_parts(m_requirer);
        if (!parts) {
            return NavigateResult::NotFound;
        }
        m_parts = std::move(*parts);
        return NavigateResult::Success;
    }

    NavigateResult jumpToAlias(const std::string& path) override {
        if (!path.starts_with('/')) {
            return NavigateResult::NotFound;
        }
        std::optional<std::vector<std::string>> parts = fbl::module_parts(path);
        if (!parts || fbl::is_too_long(*parts)) {
            return NavigateResult::NotFound;
        }
        m_parts = std::move(*parts);
        return resolve() > 1 ? NavigateResult::Ambiguous : NavigateResult::Success;
    }

    NavigateResult toParent() override {
        if (m_parts.empty()) {
            return NavigateResult::NotFound;
        }
        m_parts.pop_back();
        return NavigateResult::Success;
    }

    NavigateResult toChild(const std::string& component) override {
        if (component == "." || component == ".." || component == ".config" ||
            component.contains('/') || !fbl::is_safe_part(component)) {
            return NavigateResult::NotFound;
        }
        m_parts.push_back(component);
        if (fbl::is_too_long(m_parts)) {
            return NavigateResult::NotFound;
        }
        return resolve() > 1 ? NavigateResult::Ambiguous : NavigateResult::Success;
    }

    [[nodiscard]] ConfigStatus getConfigStatus() const override {
        return m_files.read(fbl::config_file(m_parts)).has_value() ? ConfigStatus::PresentJson
                                                                   : ConfigStatus::Absent;
    }

    [[nodiscard]] ConfigBehavior getConfigBehavior() const override {
        return ConfigBehavior::GetConfig;
    }

    [[nodiscard]] std::optional<std::string> getConfig() const override {
        return m_files.read(fbl::config_file(m_parts));
    }

    // The module's first file, when it has one.
    std::optional<std::string> module() {
        if (resolve() == 0) {
            return std::nullopt;
        }
        return m_file;
    }

private:
    // How many of the module's files there are, the first kept.
    int resolve() {
        m_file.clear();
        if (m_parts.empty()) {
            return 0;
        }
        const std::string base = fbl::joined(m_parts);
        int count = 0;
        for (const std::string_view suffix : fbl::module_suffixes) {
            std::string file = base + std::string(suffix);
            if (m_files.read(file).has_value()) {
                if (count == 0) {
                    m_file = std::move(file);
                }
                count++;
            }
        }
        return count;
    }

    files_t& m_files;
    std::string m_requirer;
    std::vector<std::string> m_parts;
    std::string m_file;
};

class file_resolver_t : public Luau::FileResolver {
public:
    explicit file_resolver_t(files_t& files) : m_files(files) {}

    std::optional<Luau::SourceCode> readSource(const Luau::ModuleName& name) override {
        std::optional<std::string> text = m_files.read(name);
        if (!text) {
            return std::nullopt;
        }
        return Luau::SourceCode{ .source = std::move(*text), .type = Luau::SourceCode::Module };
    }

    std::optional<Luau::ModuleInfo>
    resolveModule(const Luau::ModuleInfo* context, Luau::AstExpr* node,
                  const Luau::TypeCheckLimits& /*limits*/) override {
        const auto* expression =
            node == nullptr ? nullptr : node->as<Luau::AstExprConstantString>();
        if (context == nullptr || expression == nullptr) {
            return std::nullopt;
        }
        std::string path(expression->value.data, expression->value.size);
        const size_t parts = static_cast<size_t>(std::ranges::count(path, '/')) + 1;
        if (path.size() > fbl::most_path_bytes || parts > fbl::most_path_parts ||
            path.contains('\0')) {
            return std::nullopt;
        }
        navigation_t navigation(m_files, context->name);
        Luau::Require::ErrorHandler ignored;
        Luau::Require::Navigator navigator(navigation, ignored);
        if (navigator.navigate(std::move(path)) != Luau::Require::Navigator::Status::Success) {
            return std::nullopt;
        }
        std::optional<std::string> file = navigation.module();
        if (!file) {
            return std::nullopt;
        }
        return Luau::ModuleInfo{ .name = *file };
    }

private:
    files_t& m_files;
};

// Each module's configuration: its folders' .luaurc files over the checker's own, from the root
// down, as luau-analyze reads them; a .config.luau, which would run code, isn't asked for.
class config_resolver_t : public Luau::ConfigResolver {
public:
    config_resolver_t(files_t& files, Luau::Mode mode) : m_files(files) {
        m_defaults.mode = mode;
    }

    const Luau::Config& getConfig(const Luau::ModuleName& name,
                                  const Luau::TypeCheckLimits& /*limits*/) const override {
        std::vector<std::string> folder;
        size_t start = 0;
        while (true) {
            const size_t end = name.find('/', start);
            if (end == std::string::npos) {
                break;
            }
            folder.emplace_back(name.substr(start, end - start));
            start = end + 1;
        }
        return of_folder(folder);
    }

    void forget() {
        m_cache.clear();
        m_errors.clear();
    }

    // Whether the checker's mode and lint stand whatever a .luaurc says, which then gives only
    // its aliases.
    void enforce(bool enforced) {
        m_enforced = enforced;
        forget();
    }

    [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& errors() const {
        return m_errors;
    }

private:
    const Luau::Config& of_folder(const std::vector<std::string>& folder) const {
        const std::string key = fbl::joined(folder);
        const auto cached = m_cache.find(key);
        if (cached != m_cache.end()) {
            return cached->second;
        }
        Luau::Config config = m_defaults;
        if (!folder.empty()) {
            std::vector<std::string> parent = folder;
            parent.pop_back();
            config = of_folder(parent);
        }
        const std::string file = fbl::config_file(folder);
        if (const std::optional<std::string> text = m_files.read(file)) {
            Luau::ConfigOptions::AliasOptions aliases;
            aliases.configLocation = file;
            aliases.overwriteAliases = true;
            Luau::ConfigOptions options;
            options.aliasOptions = std::move(aliases);
            if (std::optional<std::string> error = Luau::parseConfig(*text, config, options)) {
                m_errors.emplace_back(file, std::move(*error));
            }
            if (m_enforced) {
                config.mode = m_defaults.mode;
                config.enabledLint = m_defaults.enabledLint;
                config.fatalLint = m_defaults.fatalLint;
                config.lintErrors = m_defaults.lintErrors;
                config.typeErrors = m_defaults.typeErrors;
                config.globals = m_defaults.globals;
            }
        }
        return m_cache.insert_or_assign(key, std::move(config)).first->second;
    }

    files_t& m_files;
    Luau::Config m_defaults;
    bool m_enforced = false;
    mutable std::unordered_map<std::string, Luau::Config> m_cache;
    mutable std::vector<std::pair<std::string, std::string>> m_errors;
};

struct diagnostic_t {
    std::string module;
    std::string name;
    std::string message;
    std::array<int32_t, 6> numbers{};
};

Luau::FrontendOptions frontend_options(double seconds) {
    Luau::FrontendOptions options;
    options.runLintChecks = true;
    if (seconds > 0) {
        options.moduleTimeLimitSec = seconds;
    }
    return options;
}

using steady_t = std::chrono::steady_clock;

struct checker_t {
    checker_t(Luau::Mode mode, double limit)
        : resolver(files), configs(files, mode),
          frontend(Luau::SolverMode::New, &resolver, &configs, frontend_options(limit)), mode(mode),
          seconds(limit) {
        Luau::registerBuiltinGlobals(frontend, frontend.globals);
        Luau::freeze(frontend.globals.globalTypes);
    }

    std::mutex busy;
    files_t files;
    file_resolver_t resolver;
    config_resolver_t configs;
    Luau::Frontend frontend;
    Luau::Mode mode;
    // Whether the checker's mode and lint stand whatever a module's files say.
    bool enforced = false;
    double seconds;
    std::vector<diagnostic_t> diagnostics;
    // The check that wanted files, which the next run of it goes on with, and the time its
    // solver's taken so far: one check's modules share its time limit, however many runs it
    // takes the program to give its files.
    std::string pending;
    steady_t::duration spent{};
};

// Cancels a check at its deadline, from a thread of its own, unless the check's over first: the
// solver asks its token between constraints, so a check's modules end at the one deadline, not
// each at its own.
class deadline_t {
public:
    deadline_t(std::shared_ptr<Luau::FrontendCancellationToken> token, steady_t::time_point at)
        : m_token(std::move(token)), m_at(at), m_thread([this] { watch(); }) {}
    ~deadline_t() {
        {
            const std::scoped_lock lock(m_mutex);
            m_over = true;
        }
        m_woken.notify_one();
        m_thread.join();
    }
    deadline_t(const deadline_t&) = delete;
    deadline_t& operator=(const deadline_t&) = delete;
    deadline_t(deadline_t&&) = delete;
    deadline_t& operator=(deadline_t&&) = delete;

private:
    void watch() {
        std::unique_lock lock(m_mutex);
        if (!m_woken.wait_until(lock, m_at, [this] { return m_over; })) {
            m_token->cancel();
        }
    }

    std::shared_ptr<Luau::FrontendCancellationToken> m_token;
    steady_t::time_point m_at;
    std::mutex m_mutex;
    std::condition_variable m_woken;
    bool m_over = false;
    // Last, as it starts watching as it's made.
    std::thread m_thread;
};

// The heap's limit of every type function's VM, Luau's and the process's. A check's type functions
// read it as they run, so each check holds this shared, and a checker that lowers it waits for
// them. MinGW's winpthreads sets up a std::shared_mutex's lock on its first use, which fails for
// some of the threads that first use it at once (a lock_shared that gives EINVAL, which a release
// build passes over and then unlocks what it never held); so it's first used once, alone, under
// the guard of the static that makes it.
std::shared_mutex& type_function_heap() {
    struct warmed_t {
        std::shared_mutex mutex;
        warmed_t() {
            mutex.lock();
            mutex.unlock();
        }
    };
    static warmed_t made;
    return made.mutex;
}

// Lowers the type functions' heap limit to `bytes`, where it's higher, or none.
void limit_type_functions(int bytes) {
    {
        const std::shared_lock reading(type_function_heap());
        const int now = FInt::DebugLuauTypeFunctionRuntimeHeapLimit.value;
        if (now > 0 && now <= bytes) {
            return;
        }
    }
    const std::unique_lock writing(type_function_heap());
    const int now = FInt::DebugLuauTypeFunctionRuntimeHeapLimit.value;
    if (now <= 0 || now > bytes) {
        FInt::DebugLuauTypeFunctionRuntimeHeapLimit.value = bytes;
    }
}

std::mutex checkers_mutex;
// The checkers, by their handles, made as they're first asked for, which can't throw where it
// can't be caught.
std::unordered_map<uint64_t, std::shared_ptr<checker_t>>& checkers() {
    static std::unordered_map<uint64_t, std::shared_ptr<checker_t>> made;
    return made;
}
uint64_t next_checker = 0;

std::shared_ptr<checker_t> find_checker(uint64_t handle) {
    const std::scoped_lock lock(checkers_mutex);
    const auto found = checkers().find(handle);
    return found == checkers().end() ? nullptr : found->second;
}

// A checker's, held while a call uses it, or a misuse said when it isn't one, or is in use.
class using_t {
public:
    explicit using_t(uint64_t handle) : m_checker(find_checker(handle)) {
        if (m_checker == nullptr) {
            fbl::fail(fbl::formatted("{} isn't a checker's handle", handle));
            return;
        }
        if (!m_checker->busy.try_lock()) {
            fbl::fail("a checker is used by one thread at a time");
            m_checker = nullptr;
        }
    }
    ~using_t() {
        if (m_checker != nullptr) {
            m_checker->busy.unlock();
        }
    }
    using_t(const using_t&) = delete;
    using_t& operator=(const using_t&) = delete;
    using_t(using_t&&) = delete;
    using_t& operator=(using_t&&) = delete;

    [[nodiscard]] checker_t* get() const {
        return m_checker.get();
    }

private:
    std::shared_ptr<checker_t> m_checker;
};

std::array<int32_t, 6> numbers_of(const Luau::Location& location, int32_t severity, int32_t code) {
    return {
        static_cast<int32_t>(location.begin.line) + 1,
        static_cast<int32_t>(location.begin.column) + 1,
        static_cast<int32_t>(location.end.line) + 1,
        static_cast<int32_t>(location.end.column) + 1,
        severity,
        code,
    };
}

// A check past its time limit, at `module`.
diagnostic_t over_time(const checker_t& checker, const std::string& module) {
    diagnostic_t diagnostic;
    diagnostic.module = module;
    diagnostic.name = "TimeLimit";
    diagnostic.message = fbl::formatted(
        "checking took more than {} seconds, so it wasn't checked whole", checker.seconds);
    diagnostic.numbers = { 1, 1, 1, 1, FBL_CHECK_ERROR, 0 };
    return diagnostic;
}

// The diagnostics of what a check checked, as luau-analyze gives them: each module's type errors,
// then its lint's errors and warnings, then a module it gave up on, then a .luaurc that didn't
// parse. The modules of a cycle can come in another order than luau-analyze's, which names them
// by its root's name and by their full paths.
void collect(checker_t& checker, const std::vector<Luau::ModuleName>& checked) {
    std::vector<diagnostic_t>& out = checker.diagnostics;
    for (const Luau::ModuleName& name : checked) {
        const std::optional<Luau::CheckResult> result =
            checker.frontend.getCheckResult(name, false);
        if (!result) {
            continue;
        }
        for (const Luau::TypeError& error : result->errors) {
            diagnostic_t diagnostic;
            diagnostic.module = error.moduleName;
            if (const auto* syntax = Luau::get_if<Luau::SyntaxError>(&error.data)) {
                diagnostic.name = "SyntaxError";
                diagnostic.message = syntax->message;
            } else {
                diagnostic.name = "TypeError";
                diagnostic.message =
                    Luau::toString(error, Luau::TypeErrorToStringOptions{ &checker.resolver });
            }
            diagnostic.numbers = numbers_of(error.location, FBL_CHECK_ERROR, error.code());
            out.push_back(std::move(diagnostic));
        }
        const auto lint = [&](const Luau::LintWarning& warning, int32_t severity) {
            diagnostic_t diagnostic;
            diagnostic.module = name;
            diagnostic.name = Luau::LintWarning::getName(warning.code);
            diagnostic.message = warning.text;
            diagnostic.numbers =
                numbers_of(warning.location, severity, static_cast<int32_t>(warning.code));
            out.push_back(std::move(diagnostic));
        };
        for (const Luau::LintWarning& warning : result->lintResult.errors) {
            lint(warning, FBL_CHECK_ERROR);
        }
        for (const Luau::LintWarning& warning : result->lintResult.warnings) {
            lint(warning, FBL_CHECK_WARNING);
        }
        if (!result->timeoutHits.empty()) {
            out.push_back(over_time(checker, name));
        }
    }
    for (const auto& [file, error] : checker.configs.errors()) {
        diagnostic_t diagnostic;
        diagnostic.module = file;
        diagnostic.name = "ConfigError";
        diagnostic.message = error;
        diagnostic.numbers = { 1, 1, 1, 1, FBL_CHECK_ERROR, 0 };
        out.push_back(std::move(diagnostic));
    }
}

// Finds a parsed module's `type function`s.
struct type_functions_t : Luau::AstVisitor {
    std::vector<Luau::Location> found;

    bool visit(Luau::AstStatTypeFunction* node) override {
        found.push_back(node->location);
        return false;
    }
};

// Refuses a check whose modules have a `type function`, with a diagnostic at each, so that none
// is run: Luau's solver runs one as it checks, the mod's own code, with the host's strings,
// arena, and native stack, which no limit of the checker's holds. Whether it refused.
bool refused_type_functions(checker_t& checker) {
    std::vector<std::pair<std::string, Luau::Location>> found;
    for (const auto& [name, source] : checker.frontend.sourceModules) {
        if (source == nullptr || source->root == nullptr) {
            continue;
        }
        type_functions_t visitor;
        source->root->visit(&visitor);
        for (const Luau::Location& location : visitor.found) {
            found.emplace_back(name, location);
        }
    }
    std::ranges::sort(found, [](const auto& a, const auto& b) {
        return std::tie(a.first, a.second.begin.line, a.second.begin.column) <
               std::tie(b.first, b.second.begin.line, b.second.begin.column);
    });
    for (const auto& [name, location] : found) {
        diagnostic_t diagnostic;
        diagnostic.module = name;
        diagnostic.name = "TypeFunction";
        diagnostic.message = "type functions aren't checked: checking would run one, so a check "
                             "that reaches one is refused";
        diagnostic.numbers = numbers_of(location, FBL_CHECK_ERROR, 0);
        checker.diagnostics.push_back(std::move(diagnostic));
    }
    return !found.empty();
}

// Gives each parsed module the checker's mode, and takes out its `--!` lines that would change
// its mode or its lint, which the module's own .luaurc can't either.
void enforce(checker_t& checker) {
    for (const auto& [name, source] : checker.frontend.sourceModules) {
        if (source == nullptr) {
            continue;
        }
        source->mode = checker.mode;
        std::erase_if(source->hotcomments, [](const Luau::HotComment& comment) {
            return comment.header &&
                   (comment.content == "strict" || comment.content == "nonstrict" ||
                    comment.content == "nocheck" || comment.content.starts_with("nolint"));
        });
    }
}

// Runs the solver over what's queued, cancelled at what's left of the check's time limit, which
// it takes from. Whether it finished in time, and the modules it checked.
std::pair<bool, std::vector<Luau::ModuleName>> solved(checker_t& checker) {
    const auto limit = std::chrono::duration_cast<steady_t::duration>(
        std::chrono::duration<double>(checker.seconds));
    const steady_t::time_point started = steady_t::now();
    if (checker.spent >= limit) {
        return { false, {} };
    }
    auto token = std::make_shared<Luau::FrontendCancellationToken>();
    checker.frontend.options.cancellationToken = token;
    std::vector<Luau::ModuleName> checked;
    {
        const deadline_t deadline(token, started + limit - checker.spent);
        // A module whose solver finished after the deadline isn't cancelled, so none's begun
        // after it.
        checked = checker.frontend.checkQueuedModules(
            std::nullopt, {},
            [&token](size_t /*done*/, size_t /*all*/) { return !token->requested(); });
    }
    checker.frontend.options.cancellationToken = nullptr;
    checker.spent += steady_t::now() - started;
    return { !token->requested(), std::move(checked) };
}

// Checks `file`, giving how many files it wanted, or 0 with its diagnostics kept.
int32_t check(checker_t& checker, const std::string& file) {
    checker.frontend.clear();
    checker.configs.forget();
    // The graph first, which reads every file it can reach without checking: what it wants is
    // given before anything's checked.
    checker.frontend.parse(file);
    if (!checker.files.wanted().empty()) {
        return static_cast<int32_t>(checker.files.wanted().size());
    }
    if (refused_type_functions(checker)) {
        return 0;
    }
    if (checker.enforced) {
        enforce(checker);
    }
    checker.frontend.queueModuleCheck(file);
    std::pair<bool, std::vector<Luau::ModuleName>> result;
    try {
        result = solved(checker);
    } catch (const Luau::InternalCompilerError& error) {
        diagnostic_t diagnostic;
        diagnostic.module = error.moduleName.value_or(file);
        diagnostic.name = "InternalCompilerError";
        diagnostic.message = error.message;
        diagnostic.numbers =
            numbers_of(error.location.value_or(Luau::Location{}), FBL_CHECK_ERROR, 0);
        checker.diagnostics.push_back(std::move(diagnostic));
        return 0;
    }
    const auto& [in_time, checked] = result;
    if (!in_time) {
        // A cancelled check gives no module's results.
        checker.diagnostics.push_back(over_time(checker, file));
        return 0;
    }
    if (!checker.files.wanted().empty()) {
        return static_cast<int32_t>(checker.files.wanted().size());
    }
    collect(checker, checked);
    return 0;
}

// A C string's text, or nothing for null.
std::optional<std::string> text_of(const char* text) {
    if (text == nullptr) {
        return std::nullopt;
    }
    return std::string(text);
}

} // namespace

uint64_t fbl_check_new(int32_t mode, double seconds, int64_t heap) try {
    if (mode < FBL_CHECK_STRICT || mode > FBL_CHECK_NOCHECK) {
        fbl::fail(fbl::formatted("{} isn't a checker's mode", mode));
        return 0;
    }
    if (!(seconds > 0) || seconds > 1e9) {
        fbl::fail(fbl::formatted("{} seconds isn't a check's time limit", seconds));
        return 0;
    }
    if (heap <= 0 || heap > std::numeric_limits<int>::max()) {
        fbl::fail(fbl::formatted("{} bytes isn't a type function's heap limit", heap));
        return 0;
    }
    fbl::set_flags();
    limit_type_functions(static_cast<int>(heap));
    const std::array<Luau::Mode, 3> modes = {
        Luau::Mode::Strict,
        Luau::Mode::Nonstrict,
        Luau::Mode::NoCheck,
    };
    auto checker = std::make_shared<checker_t>(modes.at(static_cast<size_t>(mode)), seconds);
    const std::scoped_lock lock(checkers_mutex);
    const uint64_t handle = ++next_checker;
    checkers().emplace(handle, std::move(checker));
    return handle;
} catch (const std::bad_alloc&) {
    fbl::fail("not enough memory for a checker");
    return 0;
} catch (...) {
    fbl::escaped();
    return 0;
}

void fbl_check_free(uint64_t checker) try {
    std::shared_ptr<checker_t> gone;
    const std::scoped_lock lock(checkers_mutex);
    const auto found = checkers().find(checker);
    if (found != checkers().end()) {
        gone = std::move(found->second);
        checkers().erase(found);
    }
} catch (...) {
    fbl::escaped();
}

int32_t fbl_check_definitions(uint64_t checker, const uint8_t* text, int64_t length,
                              const char* name) try {
    const using_t used(checker);
    checker_t* found = used.get();
    if (found == nullptr) {
        return FBL_MISUSE;
    }
    if (length < 0 || (text == nullptr && length > 0) || name == nullptr) {
        fbl::fail("definitions need their text and a name");
        return FBL_MISUSE;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): C's bytes, as chars.
    const std::string_view source(length == 0 ? "" : reinterpret_cast<const char*>(text),
                                  static_cast<size_t>(length));
    Luau::GlobalTypes& globals = found->frontend.globals;
    Luau::unfreeze(globals.globalTypes);
    const Luau::LoadDefinitionFileResult result = found->frontend.loadDefinitionFile(
        globals, globals.globalScope, source, std::string("@") + name, false);
    Luau::freeze(globals.globalTypes);
    if (!result.parseResult.errors.empty()) {
        const Luau::ParseError& error = result.parseResult.errors.front();
        fbl::fail(fbl::formatted("{}:{}: {}", name, error.getLocation().begin.line + 1,
                                 error.getMessage()));
        return FBL_SYNTAX;
    }
    if (result.module != nullptr && !result.module->errors.empty()) {
        const Luau::TypeError& error = result.module->errors.front();
        fbl::fail(fbl::formatted("{}:{}: {}", name, error.location.begin.line + 1,
                                 Luau::toString(error)));
        return FBL_SYNTAX;
    }
    if (!result.success) {
        fbl::fail(fbl::formatted("{}: the definitions didn't load", name));
        return FBL_SYNTAX;
    }
    // What was checked before saw other definitions.
    found->frontend.clear();
    return FBL_OK;
} catch (const std::bad_alloc&) {
    fbl::fail("not enough memory for the definitions");
    return FBL_MEMORY;
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_check_enforce(uint64_t checker, int32_t enforced) try {
    const using_t used(checker);
    checker_t* found = used.get();
    if (found == nullptr) {
        return FBL_MISUSE;
    }
    found->enforced = enforced != 0;
    found->configs.enforce(found->enforced);
    // What was checked before was checked by other rules.
    found->frontend.clear();
    return FBL_OK;
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_check_give(uint64_t checker, const char* path, const uint8_t* text, int64_t length,
                       int32_t present) try {
    const using_t used(checker);
    checker_t* found = used.get();
    if (found == nullptr) {
        return FBL_MISUSE;
    }
    if (path == nullptr || length < 0 || (text == nullptr && length > 0)) {
        fbl::fail("a file needs its path and its text");
        return FBL_MISUSE;
    }
    std::optional<std::string> given;
    if (present != 0) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): C's bytes, as chars.
        given.emplace(length == 0 ? "" : reinterpret_cast<const char*>(text),
                      static_cast<size_t>(length));
    }
    if (!found->files.give(path, std::move(given))) {
        fbl::fail(fbl::formatted("a checker is given at most {} files", most_files));
        return FBL_MISUSE;
    }
    return FBL_OK;
} catch (const std::bad_alloc&) {
    fbl::fail("not enough memory for the file");
    return FBL_MEMORY;
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_check_forget(uint64_t checker) try {
    const using_t used(checker);
    checker_t* found = used.get();
    if (found == nullptr) {
        return FBL_MISUSE;
    }
    found->files.forget();
    found->frontend.clear();
    found->diagnostics.clear();
    found->pending.clear();
    found->spent = {};
    return FBL_OK;
} catch (...) {
    return fbl::escaped();
}

int32_t fbl_check_run(uint64_t checker, const char* module) try {
    const using_t used(checker);
    checker_t* found = used.get();
    if (found == nullptr) {
        return -1;
    }
    const std::optional<std::string> path = text_of(module);
    if (!path) {
        fbl::fail("a check needs its module's path");
        return -1;
    }
    std::optional<std::vector<std::string>> parts = fbl::module_parts(*path);
    if (!parts || parts->empty() || fbl::is_too_long(*parts)) {
        fbl::fail(fbl::formatted("'{}' isn't a module's path from the root", *path));
        return -1;
    }
    const std::shared_lock running(type_function_heap());
    found->files.clear_wanted();
    found->diagnostics.clear();
    // A run of another module, or of one whose check finished, starts its own time limit.
    if (found->pending != *path) {
        found->pending.clear();
        found->spent = {};
    }
    const std::string base = fbl::joined(*parts);
    std::string file;
    int count = 0;
    for (const std::string_view suffix : fbl::module_suffixes) {
        std::string candidate = base + std::string(suffix);
        if (found->files.read(candidate).has_value()) {
            if (count == 0) {
                file = std::move(candidate);
            }
            count++;
        }
    }
    if (!found->files.wanted().empty()) {
        found->pending = *path;
        return static_cast<int32_t>(found->files.wanted().size());
    }
    if (count != 1) {
        found->pending.clear();
        found->spent = {};
    }
    if (count == 0) {
        fbl::fail(fbl::formatted("there's no module '{}'", base));
        return -1;
    }
    if (count > 1) {
        fbl::fail(fbl::formatted("module '{}' has {} files, where it can have one", base, count));
        return -1;
    }
    const int32_t wanted = check(*found, file);
    if (wanted > 0) {
        found->pending = *path;
    } else {
        found->pending.clear();
        found->spent = {};
    }
    return wanted;
} catch (const std::bad_alloc&) {
    fbl::fail("not enough memory to check");
    return -1;
} catch (...) {
    fbl::escaped();
    return -1;
}

const char* fbl_check_wanted(uint64_t checker, int32_t index) try {
    const using_t used(checker);
    const checker_t* found = used.get();
    if (found == nullptr) {
        return nullptr;
    }
    const std::vector<std::string>& wanted = found->files.wanted();
    if (index < 0 || static_cast<size_t>(index) >= wanted.size()) {
        fbl::fail(fbl::formatted("the check wanted no file {}", index));
        return nullptr;
    }
    return wanted[static_cast<size_t>(index)].c_str();
} catch (...) {
    fbl::escaped();
    return nullptr;
}

int32_t fbl_check_count(uint64_t checker) try {
    const using_t used(checker);
    const checker_t* found = used.get();
    return found == nullptr ? -1 : static_cast<int32_t>(found->diagnostics.size());
} catch (...) {
    fbl::escaped();
    return -1;
}

int32_t fbl_check_number(uint64_t checker, int32_t index, int32_t which) try {
    const using_t used(checker);
    const checker_t* found = used.get();
    if (found == nullptr) {
        return -1;
    }
    if (index < 0 || static_cast<size_t>(index) >= found->diagnostics.size() || which < 0 ||
        which > 5) {
        fbl::fail(fbl::formatted("there's no number {} of diagnostic {}", which, index));
        return -1;
    }
    return found->diagnostics[static_cast<size_t>(index)].numbers.at(static_cast<size_t>(which));
} catch (...) {
    fbl::escaped();
    return -1;
}

const char* fbl_check_text(uint64_t checker, int32_t index, int32_t which) try {
    const using_t used(checker);
    const checker_t* found = used.get();
    if (found == nullptr) {
        return nullptr;
    }
    if (index < 0 || static_cast<size_t>(index) >= found->diagnostics.size() || which < 0 ||
        which > 2) {
        fbl::fail(fbl::formatted("there's no text {} of diagnostic {}", which, index));
        return nullptr;
    }
    const diagnostic_t& diagnostic = found->diagnostics[static_cast<size_t>(index)];
    const std::array<const std::string*, 3> texts = {
        &diagnostic.module,
        &diagnostic.name,
        &diagnostic.message,
    };
    return texts.at(static_cast<size_t>(which))->c_str();
} catch (...) {
    fbl::escaped();
    return nullptr;
}
