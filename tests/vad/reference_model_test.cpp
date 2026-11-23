// The fork's Silero probabilities against the reference trace made with the ONNX model, on the
// recorded fixture.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <ranges>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "fixture_io.hpp"
#include "shell/vad_engine.hpp"

namespace {

constexpr const char* kMissingModelHint =
    "model missing: run scripts/fetch-models.sh silero-v5.1.2";

constexpr std::size_t kFrames = 336;
constexpr std::size_t kScoredSamples = kFrames * lws::kVadFrameSamples;

bool model_present() { return std::filesystem::exists(LWS_SILERO_MODEL_PATH); }

// The fork's probabilities for the first kFrames frames: one reset, then one no-reset call.
std::vector<float> fork_probabilities(lws::VadEngine& engine, std::span<const float> signal) {
    engine.reset();
    const auto probs = engine.detect(signal.first(kScoredSamples));
    return probs ? std::vector<float>(probs->begin(), probs->end()) : std::vector<float>{};
}

std::vector<float> abs_differences(const std::vector<float>& a, const std::vector<float>& b) {
    return std::views::zip_transform([](float x, float y) { return std::fabs(x - y); }, a, b) |
           std::ranges::to<std::vector>();
}

double mean_of(const std::vector<float>& values) {
    return values.empty() ? 0.0
                          : std::accumulate(values.begin(), values.end(), 0.0) /
                                static_cast<double>(values.size());
}

} // namespace

TEST(VadReferenceModel, FrameCountsMatch) {
    if (!model_present()) {
        GTEST_SKIP() << kMissingModelHint;
    }
    const auto wav = lws::test::load_fixture_wav("clean_en_sample.wav");
    ASSERT_TRUE(wav.has_value()) << wav.error();
    ASSERT_EQ(wav->size(), 172320U);
    const auto reference = lws::test::load_probability_trace("clean_en_sample.silero-ref.txt");
    ASSERT_TRUE(reference.has_value()) << reference.error();
    ASSERT_EQ(reference->size(), kFrames);

    auto engine = lws::VadEngine::open(LWS_SILERO_MODEL_PATH);
    ASSERT_TRUE(engine.has_value());
    const auto fork = fork_probabilities(*engine, *wav);
    ASSERT_EQ(fork.size(), kFrames);

    const auto diffs = abs_differences(fork, *reference);
    std::cout << "NCTX-TRACER frames=" << fork.size()
              << " max_abs=" << *std::ranges::max_element(diffs) << " mean_abs=" << mean_of(diffs)
              << '\n';
}
