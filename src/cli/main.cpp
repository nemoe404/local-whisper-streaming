#include "cli/args.hpp"
#include "cli/error_text.hpp"
#include "cli/run.hpp"

#include <fmt/format.h>

#include <array>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

namespace {

constexpr int kRuntimeExit = 1;
constexpr int kUsageExit = 2;

// Set by the signal handler, read by the run loop between reads. A signal handler can only reach
// static storage, so this flag is the one non-const global.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile std::sig_atomic_t g_stop_requested = 0;

extern "C" void request_stop(int /*signal*/) { g_stop_requested = 1; }

// SIGINT and SIGTERM end the input. SA_RESTART stays off, so a read that is blocked on stdin
// returns EINTR and the run loop sees the flag. SIGPIPE keeps its default.
bool install_stop_handlers() {
    struct sigaction action{};
    action.sa_handler = request_stop;
    sigemptyset(&action.sa_mask);
    return sigaction(SIGINT, &action, nullptr) == 0 && sigaction(SIGTERM, &action, nullptr) == 0;
}

void say(std::FILE* stream, const std::string& text) {
    static_cast<void>(std::fputs(text.c_str(), stream));
}

int usage_failure(const lws_cli::UsageError& error) {
    const std::string subject = error.flag.empty() ? std::string{} : error.flag + ": ";
    say(stderr, fmt::format("lws-cli: {}{}\n\n{}", subject, error.reason, lws_cli::usage_text()));
    return kUsageExit;
}

bool model_exists(const std::filesystem::path& path) {
    std::error_code ignored;
    return std::filesystem::exists(path, ignored);
}

// The first model path that is not a file, if any.
const std::filesystem::path* missing_model(const lws_cli::RunConfig& config) {
    const std::array<const std::filesystem::path*, 2> paths{&config.model_path,
                                                            &config.vad_model_path};
    const auto* const missing = std::ranges::find_if(
        paths, [](const std::filesystem::path* path) { return !model_exists(*path); });
    return missing == paths.end() ? nullptr : *missing;
}

int run_stream(const lws_cli::RunConfig& config) {
    if (const std::filesystem::path* missing = missing_model(config)) {
        say(stderr, fmt::format("lws-cli: cannot load model: {}\n", missing->string()));
        return kRuntimeExit;
    }
    if (!install_stop_handlers()) {
        say(stderr, "lws-cli: cannot install the signal handlers\n");
        return kRuntimeExit;
    }
    const lws_cli::RunIo io{
        .input_fd = 0, .out = stdout, .err = stderr, .stop_flag = &g_stop_requested};
    const auto report = lws_cli::run(config, io, lws::StopMode::Drain);
    if (!report) {
        const lws_cli::ErrorText text = lws_cli::error_text(report.error().error);
        say(stderr, fmt::format("lws-cli: {}\n", text.message));
        return text.exit_code;
    }
    return report->input_failed ? kRuntimeExit : 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::span<char*> raw(argv, static_cast<std::size_t>(argc));
    const std::vector<std::string_view> args =
        raw | std::views::drop(1) |
        std::views::transform([](const char* word) { return std::string_view(word); }) |
        std::ranges::to<std::vector>();
    const auto command = lws_cli::parse_args(args);
    if (!command) {
        return usage_failure(command.error());
    }
    if (std::holds_alternative<lws_cli::ShowHelp>(*command)) {
        say(stdout, lws_cli::usage_text());
        return 0;
    }
    return run_stream(std::get<lws_cli::RunConfig>(*command));
}
