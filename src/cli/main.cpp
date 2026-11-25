#include "cli/error_text.hpp"
#include "cli/run.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int kUsageExit = 2;

void say(const std::string& line) { static_cast<void>(std::fputs(line.c_str(), stderr)); }

int usage_failure() {
    say("usage: lws-cli --model PATH --vad-model PATH\n");
    return kUsageExit;
}

std::optional<std::string_view> value_of(const std::vector<std::string_view>& args,
                                         std::string_view flag) {
    const auto found = std::ranges::find(args, flag);
    if (found == args.end() || std::next(found) == args.end()) {
        return std::nullopt;
    }
    return *std::next(found);
}

} // namespace

int main(int argc, char** argv) {
    const std::span<char*> raw(argv, static_cast<std::size_t>(argc));
    const std::vector<std::string_view> args =
        raw | std::views::drop(1) |
        std::views::transform([](const char* word) { return std::string_view(word); }) |
        std::ranges::to<std::vector>();
    const auto model = value_of(args, "--model");
    const auto vad_model = value_of(args, "--vad-model");
    if (args.size() != 4 || !model || !vad_model) {
        return usage_failure();
    }
    lws_cli::RunConfig config;
    config.model_path = std::filesystem::path(*model);
    config.vad_model_path = std::filesystem::path(*vad_model);

    const lws_cli::RunIo io{.input_fd = 0, .out = stdout, .err = stderr, .stop_flag = nullptr};
    const auto report = lws_cli::run(config, io, lws::StopMode::Drain);
    if (!report) {
        say(fmt::format("lws-cli: {}\n", lws_cli::error_text(report.error().error).message));
        return lws_cli::error_text(report.error().error).exit_code;
    }
    return report->input_failed ? 1 : 0;
}
