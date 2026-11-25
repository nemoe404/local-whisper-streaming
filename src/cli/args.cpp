#include "cli/args.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>

namespace lws_cli {

namespace {

// Longest ring the tool accepts: a day of audio. A longer one only reflects a typo.
constexpr double kMaxRingSeconds = 86400.0;
// Slowest pacing the tool accepts. Slower than this overflows the nanosecond schedule on a long
// input, and nobody replays a recording at a thousandth of real time.
constexpr double kMinSpeed = 0.001;

using Applied = std::expected<void, std::string>;

// A whole number or decimal with nothing after it. Text that is not a number, or that does not fit
// the type, gives nullopt. A float also accepts "nan" and "inf", so the callers check for those.
template <typename Number> std::optional<Number> parse_number(std::string_view text) {
    Number value{};
    const auto [stop, error] = std::from_chars(text.begin(), std::to_address(text.end()), value);
    if (error != std::errc{} || stop != std::to_address(text.end())) {
        return std::nullopt;
    }
    return value;
}

Applied apply_model(RunConfig& config, std::string_view value) {
    if (value.empty()) {
        return std::unexpected("the path is empty");
    }
    config.model_path = std::filesystem::path(value);
    return {};
}

Applied apply_vad_model(RunConfig& config, std::string_view value) {
    if (value.empty()) {
        return std::unexpected("the path is empty");
    }
    config.vad_model_path = std::filesystem::path(value);
    return {};
}

Applied apply_language(RunConfig& config, std::string_view value) {
    if (value.empty()) {
        return std::unexpected("the language is empty");
    }
    config.language = std::string(value);
    return {};
}

Applied apply_threads(RunConfig& config, std::string_view value) {
    const auto threads = parse_number<std::int32_t>(value);
    if (!threads || *threads < 1) {
        return std::unexpected("must be a whole number of at least 1");
    }
    config.threads = *threads;
    return {};
}

Applied apply_cpu(RunConfig& config, std::string_view /*value*/) {
    config.use_gpu = false;
    return {};
}

Applied apply_ring_seconds(RunConfig& config, std::string_view value) {
    const auto seconds = parse_number<double>(value);
    if (!seconds || !std::isfinite(*seconds) || *seconds <= 0.0 || *seconds > kMaxRingSeconds) {
        return std::unexpected(
            fmt::format("must be a number above 0 and at most {}", kMaxRingSeconds));
    }
    config.ring_seconds = *seconds;
    return {};
}

Applied apply_speed(RunConfig& config, std::string_view value) {
    const auto speed = parse_number<double>(value);
    if (!speed || !std::isfinite(*speed) || (*speed != 0.0 && *speed < kMinSpeed)) {
        return std::unexpected(fmt::format("must be 0 or a number of at least {}", kMinSpeed));
    }
    config.speed = *speed;
    return {};
}

Applied apply_queue(RunConfig& config, std::string_view value) {
    const auto segments = parse_number<std::size_t>(value);
    if (!segments || *segments < 1) {
        return std::unexpected("must be a whole number of at least 1");
    }
    config.queue_segments = *segments;
    return {};
}

Applied apply_stats(RunConfig& config, std::string_view /*value*/) {
    config.stats = true;
    return {};
}

Applied apply_help(RunConfig& /*config*/, std::string_view /*value*/) { return {}; }

enum class Kind : std::uint8_t { Value, Switch };

struct Flag {
    std::string_view name;
    Kind kind;
    Applied (*apply)(RunConfig&, std::string_view);
    std::string_view usage;
};

// The whole command line: the parser and the usage text both read this table.
constexpr std::array kFlags{
    Flag{.name = "--model",
         .kind = Kind::Value,
         .apply = apply_model,
         .usage = "--model PATH         whisper model file (required)"},
    Flag{.name = "--vad-model",
         .kind = Kind::Value,
         .apply = apply_vad_model,
         .usage = "--vad-model PATH     Silero voice detector model file (required)"},
    Flag{.name = "--language",
         .kind = Kind::Value,
         .apply = apply_language,
         .usage = "--language CODE      spoken language (default en)"},
    Flag{.name = "--threads",
         .kind = Kind::Value,
         .apply = apply_threads,
         .usage = "--threads N          decoder threads, at least 1 (default 4)"},
    Flag{.name = "--cpu",
         .kind = Kind::Switch,
         .apply = apply_cpu,
         .usage = "--cpu                decode on the CPU, not the GPU"},
    Flag{.name = "--ring-seconds",
         .kind = Kind::Value,
         .apply = apply_ring_seconds,
         .usage = "--ring-seconds S     audio the ring holds, a decimal above 0 (default 60)"},
    Flag{.name = "--speed",
         .kind = Kind::Value,
         .apply = apply_speed,
         .usage = "--speed X            push at X times real time; 0 pushes as fast as input "
                  "arrives (default 0)"},
    Flag{.name = "--queue",
         .kind = Kind::Value,
         .apply = apply_queue,
         .usage = "--queue N            finished segments that may wait for the decoder, at "
                  "least 1 (default 8)"},
    Flag{.name = "--stats",
         .kind = Kind::Switch,
         .apply = apply_stats,
         .usage = "--stats              print one line of measurements on stderr at the end"},
    Flag{.name = "--help",
         .kind = Kind::Switch,
         .apply = apply_help,
         .usage = "--help               print this text and exit"},
};

constexpr std::size_t kModelIndex = 0;
constexpr std::size_t kVadModelIndex = 1;
constexpr std::size_t kHelpIndex = 9;
static_assert(kFlags[kModelIndex].name == "--model");
static_assert(kFlags[kVadModelIndex].name == "--vad-model");
static_assert(kFlags[kHelpIndex].name == "--help");
constexpr std::array kRequired{kModelIndex, kVadModelIndex};

// One step of the parse: the config so far, the flags given, and the flag waiting for its value.
struct ParseState {
    RunConfig config;
    std::bitset<kFlags.size()> given;
    std::optional<std::size_t> waiting;
};

using Step = std::expected<ParseState, UsageError>;

UsageError refusal(std::string_view flag, std::string_view reason) {
    return UsageError{.flag = std::string(flag), .reason = std::string(reason)};
}

Step take_value(ParseState state, std::size_t index, std::string_view word) {
    state.waiting.reset();
    if (const Applied applied = kFlags.at(index).apply(state.config, word); !applied) {
        return std::unexpected(refusal(kFlags.at(index).name, applied.error()));
    }
    return state;
}

Step take_flag(ParseState state, std::string_view word) {
    const auto* const found = std::ranges::find(kFlags, word, &Flag::name);
    if (found == kFlags.end()) {
        return std::unexpected(refusal(word, "unknown option"));
    }
    const auto index = static_cast<std::size_t>(std::distance(kFlags.begin(), found));
    if (state.given.test(index)) {
        return std::unexpected(refusal(word, "given more than once"));
    }
    state.given.set(index);
    if (found->kind == Kind::Value) {
        state.waiting = index;
        return state;
    }
    return take_value(std::move(state), index, {});
}

Step step(Step state, std::string_view word) {
    if (!state) {
        return state;
    }
    if (const std::optional<std::size_t> waiting = state->waiting) {
        return take_value(*std::move(state), *waiting, word);
    }
    return take_flag(*std::move(state), word);
}

} // namespace

std::expected<Command, UsageError> parse_args(std::span<const std::string_view> args) {
    const Step end = std::ranges::fold_left(args, Step{ParseState{}}, step);
    if (!end) {
        return std::unexpected(end.error());
    }
    if (end->waiting) {
        return std::unexpected(refusal(kFlags.at(*end->waiting).name, "needs a value"));
    }
    if (end->given.test(kHelpIndex)) {
        return Command{ShowHelp{}};
    }
    const auto* const missing =
        std::ranges::find_if(kRequired, [&](std::size_t index) { return !end->given.test(index); });
    if (missing != kRequired.end()) {
        return std::unexpected(refusal(kFlags.at(*missing).name, "is required"));
    }
    return Command{end->config};
}

std::string usage_text() {
    const std::string flags =
        std::ranges::fold_left(kFlags, std::string{}, [](std::string text, const Flag& flag) {
            return std::move(text) + "  " + std::string(flag.usage) + "\n";
        });
    return "usage: lws-cli --model PATH --vad-model PATH [options]\n\n"
           "Reads raw 16 kHz mono f32le samples from stdin and prints a transcript on stdout.\n"
           "A producer such as pwcap must write that format (for pwcap, give it --format "
           "f32le).\n\n"
           "options:\n" +
           flags +
           "\nexit codes: 0 the input ended or a signal arrived and the stream was drained,\n"
           "            1 a runtime error, 2 a usage error\n";
}

} // namespace lws_cli
