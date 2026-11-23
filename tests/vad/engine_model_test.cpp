// VadEngine over the fetched Silero v5.1.2 model: opening, scoring and the parameters it runs with.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <numbers>
#include <ranges>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "shell/vad_engine.hpp"
#include "shell/whisper_log.hpp"
#include "whisper.h"

namespace {

constexpr const char* kMissingModelHint =
    "model missing: run scripts/fetch-models.sh silero-v5.1.2";

bool model_present() { return std::filesystem::exists(LWS_SILERO_MODEL_PATH); }

// Eight voiced frames (a 0.25-amplitude 220 Hz sine, phase running across frames) then eight zero
// frames, repeated to fill the requested number of frames.
std::vector<float> speech_pattern(std::size_t frames) {
    return std::views::iota(std::size_t{0}, frames * lws::kVadFrameSamples) |
           std::views::transform([](std::size_t i) {
               const bool voiced = (i / lws::kVadFrameSamples) % 16 < 8;
               const double seconds = static_cast<double>(i) / lws::kSampleRateHz;
               const double wave = 0.25 * std::sin(2.0 * std::numbers::pi * 220.0 * seconds);
               return voiced ? static_cast<float>(wave) : 0.0F;
           }) |
           std::ranges::to<std::vector>();
}

// One detect call per frame after a reset, optionally with a reset before every frame. A failed
// call scores NaN, which no bit comparison against a real run can match. Each borrowed span is
// copied out before the next call.
std::vector<float> score_frame_by_frame(lws::VadEngine& engine, std::span<const float> signal,
                                        bool reset_every_frame) {
    engine.reset();
    const std::size_t frames = signal.size() / lws::kVadFrameSamples;
    return std::views::iota(std::size_t{0}, frames) | std::views::transform([&](std::size_t i) {
               if (reset_every_frame) {
                   engine.reset();
               }
               const auto probs =
                   engine.detect(signal.subspan(i * lws::kVadFrameSamples, lws::kVadFrameSamples));
               return probs && probs->size() == 1 ? probs->front()
                                                  : std::numeric_limits<float>::quiet_NaN();
           }) |
           std::ranges::to<std::vector>();
}

// One detect call over every frame after a reset.
std::vector<float> score_in_one_call(lws::VadEngine& engine, std::span<const float> signal) {
    engine.reset();
    const auto probs = engine.detect(signal);
    return probs ? std::vector<float>(probs->begin(), probs->end()) : std::vector<float>{};
}

std::vector<std::uint32_t> bits_of(const std::vector<float>& values) {
    return values | std::views::transform([](float v) { return std::bit_cast<std::uint32_t>(v); }) |
           std::ranges::to<std::vector>();
}

} // namespace

TEST(VadEngineModel, OpenScoresOneFrame) {
    if (!model_present()) {
        GTEST_SKIP() << kMissingModelHint;
    }
    auto engine = lws::VadEngine::open(LWS_SILERO_MODEL_PATH);
    ASSERT_TRUE(engine.has_value());

    engine->reset();
    const std::vector<float> frame(lws::kVadFrameSamples, 0.0F);
    const auto probs = engine->detect(frame);
    ASSERT_TRUE(probs.has_value());
    ASSERT_EQ(probs->size(), 1U);
    const float p = probs->front();
    EXPECT_TRUE(std::isfinite(p));
    EXPECT_GE(p, 0.0F);
    EXPECT_LE(p, 1.0F);
}

TEST(VadEngineModel, MissingModelIsModelLoad) {
    const auto missing =
        std::filesystem::temp_directory_path() / "lws-no-such-directory" / "ggml-silero-v5.1.2.bin";
    const auto engine = lws::VadEngine::open(missing);
    ASSERT_FALSE(engine.has_value());
    EXPECT_EQ(engine.error(), lws::Error::ModelLoad);
}

TEST(VadEngineParams, CpuAndOneThreadInEveryBuild) {
    if (!model_present()) {
        GTEST_SKIP() << kMissingModelHint;
    }
    const auto params = lws::vad_context_params();
    EXPECT_FALSE(params.use_gpu);
    EXPECT_EQ(params.n_threads, 1);
    EXPECT_EQ(params.gpu_device, 0);
}

TEST(VadEngineModel, ProbeAcceptsTheShippedModel) {
    if (!model_present()) {
        GTEST_SKIP() << kMissingModelHint;
    }
    auto engine = lws::VadEngine::open(LWS_SILERO_MODEL_PATH);
    ASSERT_TRUE(engine.has_value());
    EXPECT_TRUE(lws::probe_frame_size(*engine).has_value());
}

TEST(VadEngineModel, StateCarriesAcrossNoResetCalls) {
    if (!model_present()) {
        GTEST_SKIP() << kMissingModelHint;
    }
    auto engine = lws::VadEngine::open(LWS_SILERO_MODEL_PATH);
    ASSERT_TRUE(engine.has_value());

    constexpr std::size_t kFrames = 32;
    const auto signal = speech_pattern(kFrames);

    const auto one_call = score_in_one_call(*engine, signal);
    const auto carried = score_frame_by_frame(*engine, signal, false);
    const auto reset_each = score_frame_by_frame(*engine, signal, true);

    ASSERT_EQ(one_call.size(), kFrames);
    ASSERT_EQ(carried.size(), kFrames);
    EXPECT_TRUE(std::ranges::all_of(carried, [](float p) { return std::isfinite(p); }));
    EXPECT_EQ(bits_of(one_call), bits_of(carried));
    // The control: with the state wiped before every frame the probabilities change, so the
    // equality above is a statement about carried state and not about a model that ignores it.
    EXPECT_NE(bits_of(carried), bits_of(reset_each));
}

namespace {

// What a run of detect calls wrote to stderr and how many log lines the route dropped meanwhile.
struct LoggedRun {
    bool opened = false;
    std::size_t probabilities = 0;
    std::uint64_t dropped = 0;
    std::string captured;
};

// Opens the engine and scores the signal frame by frame with stderr captured from before the model
// load, so the loader's own lines are covered too.
LoggedRun run_with_stderr_captured(std::span<const float> signal) {
    LoggedRun run;
    testing::internal::CaptureStderr();
    const std::uint64_t dropped_before = lws::whisper_log_lines_dropped();
    auto engine = lws::VadEngine::open(LWS_SILERO_MODEL_PATH);
    run.opened = engine.has_value();
    if (run.opened) {
        run.probabilities = score_frame_by_frame(*engine, signal, false).size();
    }
    run.captured = testing::internal::GetCapturedStderr();
    run.dropped = lws::whisper_log_lines_dropped() - dropped_before;
    return run;
}

} // namespace

TEST(VadEngineLog, SixtySecondsPrintNoWhisperVadLines) {
    if (!model_present()) {
        GTEST_SKIP() << kMissingModelHint;
    }
    constexpr std::size_t kCalls = 1875; // 60 s of 32 ms frames
    const auto signal = speech_pattern(kCalls);
    ASSERT_EQ(signal.size(), 960000U);

    const LoggedRun run = run_with_stderr_captured(signal);

    ASSERT_TRUE(run.opened);
    EXPECT_EQ(run.probabilities, kCalls);
    EXPECT_EQ(run.captured.find("whisper_vad"), std::string::npos);
    EXPECT_EQ(run.captured.find("vad time"), std::string::npos);
    EXPECT_GE(run.dropped, 4 * kCalls);
    std::cout << "LOG-ROUTE calls=" << kCalls << " dropped=" << run.dropped
              << " stderr_bytes=" << run.captured.size() << '\n';
}

// The control for the test above: with whisper's own hook restored the same call does print, so a
// capture that sees nothing there would show the check cannot fail.
TEST(VadEngineLog, DefaultHookPrintsVadLines) {
    if (!model_present()) {
        GTEST_SKIP() << kMissingModelHint;
    }
    auto engine = lws::VadEngine::open(LWS_SILERO_MODEL_PATH);
    ASSERT_TRUE(engine.has_value());

    whisper_log_set(nullptr, nullptr);
    testing::internal::CaptureStderr();
    const std::vector<float> frame(lws::kVadFrameSamples, 0.0F);
    const auto probs = engine->detect(frame);
    const std::string captured = testing::internal::GetCapturedStderr();
    lws::install_whisper_log_route();

    EXPECT_TRUE(probs.has_value());
    EXPECT_NE(captured.find("whisper_vad_detect_speech_no_reset"), std::string::npos);
}
