#include "cli/args.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using lws_cli::Command;
using lws_cli::RunConfig;
using lws_cli::ShowHelp;

constexpr std::string_view kModel = "--model";
constexpr std::string_view kVad = "--vad-model";

std::expected<Command, lws_cli::UsageError> parse(std::initializer_list<std::string_view> words) {
    const std::vector<std::string_view> args(words);
    return lws_cli::parse_args(args);
}

// The run configuration of a line that must parse as a run.
RunConfig parsed_run(std::initializer_list<std::string_view> words) {
    const auto command = parse(words);
    EXPECT_TRUE(command.has_value());
    const RunConfig* config = command ? std::get_if<RunConfig>(&*command) : nullptr;
    EXPECT_NE(config, nullptr);
    return config != nullptr ? *config : RunConfig{};
}

void expect_refused(std::initializer_list<std::string_view> words, std::string_view flag) {
    const auto command = parse(words);
    ASSERT_FALSE(command.has_value()) << "parsed a line that must be refused";
    EXPECT_EQ(command.error().flag, flag);
    EXPECT_FALSE(command.error().reason.empty());
}

} // namespace

TEST(CliArgs, TheMinimalLineParsesInEitherOrder) {
    const RunConfig first = parsed_run({kModel, "base.bin", kVad, "silero.bin"});
    const RunConfig second = parsed_run({kVad, "silero.bin", kModel, "base.bin"});
    EXPECT_EQ(first.model_path, "base.bin");
    EXPECT_EQ(first.vad_model_path, "silero.bin");
    EXPECT_EQ(second.model_path, first.model_path);
    EXPECT_EQ(second.vad_model_path, first.vad_model_path);
}

TEST(CliArgs, DefaultsAreEnglishFourThreadsGpuSixtySecondsAndNoPacing) {
    const RunConfig config = parsed_run({kModel, "m", kVad, "v"});
    EXPECT_EQ(config.language, "en");
    EXPECT_EQ(config.threads, 4);
    EXPECT_TRUE(config.use_gpu);
    EXPECT_DOUBLE_EQ(config.ring_seconds, 60.0);
    EXPECT_DOUBLE_EQ(config.speed, 0.0);
}

TEST(CliArgs, HelpGivesTheHelpCommand) {
    const auto command = parse({"--help"});
    ASSERT_TRUE(command.has_value());
    EXPECT_TRUE(std::holds_alternative<ShowHelp>(*command));
}

TEST(CliArgs, AMissingModelOrVadModelNamesTheFlag) {
    expect_refused({kVad, "v"}, kModel);
    expect_refused({kModel, "m"}, kVad);
    expect_refused({}, kModel);
}

TEST(CliArgs, AnUnknownFlagIsRefused) {
    expect_refused({kModel, "m", kVad, "v", "--bogus", "1"}, "--bogus");
    expect_refused({kModel, "m", kVad, "v", "stray"}, "stray");
}

TEST(CliArgs, AFlagWithoutItsValueIsRefused) {
    expect_refused({kModel, "m", kVad}, kVad);
    expect_refused({kModel, "m", kVad, "v", "--threads"}, "--threads");
}

TEST(CliArgs, ARepeatedFlagIsRefused) {
    expect_refused({kModel, "m", kModel, "n", kVad, "v"}, kModel);
    expect_refused({kModel, "m", kVad, "v", "--cpu", "--cpu"}, "--cpu");
}

TEST(CliArgs, ThreadsMustBeAWholeNumberOfAtLeastOne) {
    expect_refused({kModel, "m", kVad, "v", "--threads", "0"}, "--threads");
    expect_refused({kModel, "m", kVad, "v", "--threads", "4x"}, "--threads");
    expect_refused({kModel, "m", kVad, "v", "--threads", "-2"}, "--threads");
    expect_refused({kModel, "m", kVad, "v", "--threads", ""}, "--threads");
    EXPECT_EQ(parsed_run({kModel, "m", kVad, "v", "--threads", "8"}).threads, 8);
}

TEST(CliArgs, RingSecondsMustBeFiniteAndAboveZero) {
    expect_refused({kModel, "m", kVad, "v", "--ring-seconds", "-1"}, "--ring-seconds");
    expect_refused({kModel, "m", kVad, "v", "--ring-seconds", "0"}, "--ring-seconds");
    expect_refused({kModel, "m", kVad, "v", "--ring-seconds", "1e999"}, "--ring-seconds");
    expect_refused({kModel, "m", kVad, "v", "--ring-seconds", "inf"}, "--ring-seconds");
    expect_refused({kModel, "m", kVad, "v", "--ring-seconds", "nan"}, "--ring-seconds");
    expect_refused({kModel, "m", kVad, "v", "--ring-seconds", "2s"}, "--ring-seconds");
    expect_refused({kModel, "m", kVad, "v", "--ring-seconds", "1e9"}, "--ring-seconds");
}

TEST(CliArgs, SpeedMustBeFiniteAndNotNegative) {
    expect_refused({kModel, "m", kVad, "v", "--speed", "nan"}, "--speed");
    expect_refused({kModel, "m", kVad, "v", "--speed", "-0.5"}, "--speed");
    expect_refused({kModel, "m", kVad, "v", "--speed", "fast"}, "--speed");
    expect_refused({kModel, "m", kVad, "v", "--speed", "1e999"}, "--speed");
    expect_refused({kModel, "m", kVad, "v", "--speed", "1e-9"}, "--speed");
    EXPECT_DOUBLE_EQ(parsed_run({kModel, "m", kVad, "v", "--speed", "0"}).speed, 0.0);
}

TEST(CliArgs, TheValuesOfTheOptionalFlagsLandInTheConfig) {
    const RunConfig config = parsed_run({kModel, "m", kVad, "v", "--cpu", "--language", "de",
                                         "--ring-seconds", "0.05", "--speed", "1"});
    EXPECT_FALSE(config.use_gpu);
    EXPECT_EQ(config.language, "de");
    EXPECT_DOUBLE_EQ(config.ring_seconds, 0.05);
    EXPECT_DOUBLE_EQ(config.speed, 1.0);
}

TEST(CliArgs, StatsIsOffByDefaultAndASwitchTurnsItOn) {
    EXPECT_FALSE(parsed_run({kModel, "m", kVad, "v"}).stats);
    EXPECT_TRUE(parsed_run({kModel, "m", kVad, "v", "--stats"}).stats);
    expect_refused({kModel, "m", kVad, "v", "--stats", "--stats"}, "--stats");
}

TEST(CliArgs, QueueIsAWholeNumberOfAtLeastOne) {
    EXPECT_EQ(parsed_run({kModel, "m", kVad, "v"}).queue_segments, 8U);
    EXPECT_EQ(parsed_run({kModel, "m", kVad, "v", "--queue", "32"}).queue_segments, 32U);
    expect_refused({kModel, "m", kVad, "v", "--queue", "0"}, "--queue");
    expect_refused({kModel, "m", kVad, "v", "--queue", "x"}, "--queue");
    expect_refused({kModel, "m", kVad, "v", "--queue", "-1"}, "--queue");
    expect_refused({kModel, "m", kVad, "v", "--queue", "2.5"}, "--queue");
    expect_refused({kModel, "m", kVad, "v", "--queue", ""}, "--queue");
}

TEST(CliArgs, AnEmptyLanguageIsRefused) {
    expect_refused({kModel, "m", kVad, "v", "--language", ""}, "--language");
}

TEST(CliArgs, TheUsageTextNamesEveryFlagAndTheInputFormat) {
    const std::string text = lws_cli::usage_text();
    constexpr std::array<std::string_view, 11> kFlags{
        "--model", "--vad-model", "--language", "--threads", "--cpu", "--ring-seconds",
        "--speed", "--queue",     "--stats",    "--help",    "f32le"};
    std::ranges::for_each(kFlags, [&](std::string_view flag) {
        EXPECT_NE(text.find(flag), std::string::npos) << flag;
    });
    EXPECT_NE(text.find("--format f32le"), std::string::npos);
}
