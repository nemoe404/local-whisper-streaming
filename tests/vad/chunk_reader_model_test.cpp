// ChunkReader over the fetched Silero v5.1.2 model: the startup probe and frames fed in pieces.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "shell/chunk_reader.hpp"
#include "shell/vad_engine.hpp"

namespace {

constexpr const char* kMissingModelHint =
    "model missing: run scripts/fetch-models.sh silero-v5.1.2";

bool model_present() { return std::filesystem::exists(LWS_SILERO_MODEL_PATH); }

// What one feed call did: whether it succeeded, how many frames it scored, how many samples stayed
// buffered, and whether every probability is a finite value in [0, 1].
struct Step {
    bool ok;
    std::size_t frames;
    std::size_t pending;
    bool probabilities_valid;

    friend bool operator==(const Step&, const Step&) = default;
};

Step feed_zeros(lws::ChunkReader& reader, std::size_t count) {
    const auto scored = reader.feed(std::vector<float>(count, 0.0F));
    if (!scored) {
        return {
            .ok = false, .frames = 0, .pending = reader.pending(), .probabilities_valid = false};
    }
    const bool valid = std::ranges::all_of(
        scored->probs, [](float p) { return std::isfinite(p) && p >= 0.0F && p <= 1.0F; });
    return {.ok = true,
            .frames = scored->size(),
            .pending = reader.pending(),
            .probabilities_valid = valid};
}

} // namespace

TEST(ChunkReaderModel, StartupProbePassesAndPiecesAreScored) {
    if (!model_present()) {
        GTEST_SKIP() << kMissingModelHint;
    }
    auto engine = lws::VadEngine::open(LWS_SILERO_MODEL_PATH);
    ASSERT_TRUE(engine.has_value());
    auto reader = lws::ChunkReader::create(std::move(*engine));
    ASSERT_TRUE(reader.has_value());

    const std::vector<Step> steps{
        feed_zeros(*reader, 300),
        feed_zeros(*reader, 212),
        feed_zeros(*reader, 1024),
    };
    const std::vector<Step> expected{
        {.ok = true, .frames = 0, .pending = 300, .probabilities_valid = true},
        {.ok = true, .frames = 1, .pending = 0, .probabilities_valid = true},
        {.ok = true, .frames = 2, .pending = 0, .probabilities_valid = true},
    };
    EXPECT_EQ(steps, expected);
}
