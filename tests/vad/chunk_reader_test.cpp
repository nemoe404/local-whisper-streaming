// ChunkReader over fake engines: startup, exact frames and the reset points. No ggml compute.
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <numeric>
#include <ranges>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "fake_vad.hpp"
#include "shell/chunk_reader.hpp"

namespace {

using lws::test::FakeCall;
using lws::test::FakeVadLog;

constexpr std::size_t kFrame = lws::kVadFrameSamples;

// The calls a successful create leaves in the log: the two probe calls, then the session-start
// reset.
std::vector<FakeCall> startup_log() {
    return {
        {FakeCall::Kind::Detect, kFrame},
        {FakeCall::Kind::Detect, kFrame + 1},
        {FakeCall::Kind::Reset, 0},
    };
}

// 40 whole frames with distinct probabilities in (0, 1), then 300 samples of a partial frame.
constexpr std::size_t kStreamFrames = 40;
constexpr std::size_t kPartialSamples = 300;
constexpr std::array<std::size_t, 8> kChunkSizes{1, 160, 511, 512, 513, 1000, 4096, 10000};

std::vector<float> frame_values() {
    return std::views::iota(std::size_t{0}, kStreamFrames) |
           std::views::transform([](std::size_t k) { return static_cast<float>(k + 1) / 64.0F; }) |
           std::ranges::to<std::vector>();
}

std::vector<float> test_stream() {
    auto stream = lws::test::scripted_stream(frame_values());
    stream.insert(stream.end(), kPartialSamples, 0.5F);
    return stream;
}

std::vector<std::uint32_t> bits_of(const std::vector<float>& values) {
    return values | std::views::transform([](float v) { return std::bit_cast<std::uint32_t>(v); }) |
           std::ranges::to<std::vector>();
}

// Everything one chunked pass over test_stream produced.
struct ChunkedRun {
    bool all_fed = true;
    std::vector<std::size_t> detect_sizes; // after the two probe calls
    std::vector<float> samples;
    std::vector<float> probs;
    std::size_t pending = 0;
};

ChunkedRun run_chunked(std::size_t chunk_size) {
    FakeVadLog log;
    auto reader = lws::ChunkReader::create(lws::test::fake_engine(log, kFrame));
    ChunkedRun run;
    if (!reader) {
        run.all_fed = false;
        return run;
    }
    const auto stream = test_stream();
    std::ranges::for_each(
        stream | std::views::chunk(static_cast<std::ptrdiff_t>(chunk_size)), [&](auto piece) {
            const auto scored = reader->feed(std::span<const float>{piece.begin(), piece.end()});
            if (!scored) {
                run.all_fed = false;
                return;
            }
            std::ranges::copy(scored->samples, std::back_inserter(run.samples));
            std::ranges::copy(scored->probs, std::back_inserter(run.probs));
        });
    run.detect_sizes =
        lws::test::detect_sizes(log) | std::views::drop(2) | std::ranges::to<std::vector>();
    run.pending = reader->pending();
    return run;
}

// One whole frame delivered as 511 samples then 1: what the reader did after each piece.
struct SplitFrame {
    bool fed = false;
    std::size_t frames_after_partial = 0;
    std::size_t detect_calls_after_partial = 0;
    std::size_t pending_after_partial = 0;
    std::size_t frames_after_last = 0;
    std::size_t pending_after_last = 0;
    std::vector<std::size_t> detect_sizes;

    friend bool operator==(const SplitFrame&, const SplitFrame&) = default;
};

SplitFrame feed_frame_in_two_pieces() {
    SplitFrame split;
    FakeVadLog log;
    auto reader = lws::ChunkReader::create(lws::test::fake_engine(log, kFrame));
    if (!reader) {
        return split;
    }
    const std::vector<float> samples(kFrame, 0.25F);
    const std::span<const float> all{samples};

    const auto partial = reader->feed(all.first(kFrame - 1));
    split.frames_after_partial = partial ? partial->size() : 0;
    split.detect_calls_after_partial = lws::test::detect_sizes(log).size();
    split.pending_after_partial = reader->pending();
    const auto completed = reader->feed(all.last(1));
    split.frames_after_last = completed ? completed->size() : 0;
    split.pending_after_last = reader->pending();
    split.detect_sizes = lws::test::detect_sizes(log);
    split.fed = partial.has_value() && completed.has_value();
    return split;
}

// One failure message per chunk size for which the predicate is false.
template <typename Check>
std::vector<std::string> failures_over_sizes(const char* what, const Check& check) {
    return kChunkSizes | std::views::filter([&](std::size_t size) { return !check(size); }) |
           std::views::transform(
               [&](std::size_t size) { return "chunk " + std::to_string(size) + ": " + what; }) |
           std::ranges::to<std::vector>();
}

} // namespace

TEST(ChunkReaderStartup, ProbeThenOneReset) {
    FakeVadLog log;
    const auto reader = lws::ChunkReader::create(lws::test::fake_engine(log, kFrame));
    ASSERT_TRUE(reader.has_value());
    EXPECT_EQ(log.calls, startup_log());
}

TEST(ChunkReaderStartup, FrameSizeOtherThan512FailsCreate) {
    constexpr std::array<std::size_t, 2> kWindows{256, 1024};
    const auto failures =
        kWindows | std::views::filter([](std::size_t window) {
            FakeVadLog log;
            const auto reader = lws::ChunkReader::create(lws::test::fake_engine(log, window));
            const bool wrong_error =
                reader.has_value() || reader.error() != lws::Error::FrameSizeMismatch;
            return wrong_error || lws::test::resets(log) != 0;
        }) |
        std::ranges::to<std::vector>();
    EXPECT_TRUE(failures.empty());
}

TEST(ChunkReaderStartup, OneWholeFrameIsScored) {
    FakeVadLog log;
    auto reader = lws::ChunkReader::create(lws::test::fake_engine(log, kFrame));
    ASSERT_TRUE(reader.has_value());

    const std::array<float, 1> values{0.75F};
    const auto input = lws::test::scripted_stream(values);
    const auto scored = reader->feed(input);

    ASSERT_TRUE(scored.has_value());
    ASSERT_EQ(scored->size(), 1U);
    EXPECT_EQ(scored->probs.front(), 0.75F);
    EXPECT_EQ(scored->samples, input);
    EXPECT_EQ(reader->pending(), 0U);
}

TEST(ChunkReaderFrames, EveryDetectCallAfterStartupIsWholeFrames) {
    const auto failures =
        failures_over_sizes("detect size not a positive multiple of 512", [](auto size) {
            const ChunkedRun run = run_chunked(size);
            const bool whole = std::ranges::all_of(
                run.detect_sizes, [](std::size_t n) { return n > 0 && n % kFrame == 0; });
            const std::size_t total =
                std::accumulate(run.detect_sizes.begin(), run.detect_sizes.end(), std::size_t{0});
            return run.all_fed && whole && total == kStreamFrames * kFrame;
        });
    EXPECT_EQ(failures, std::vector<std::string>{});
}

TEST(ChunkReaderFrames, ScoredSamplesAreTheWholeFramePrefixBitExact) {
    const auto stream = test_stream();
    const std::vector<float> prefix(stream.begin(), stream.begin() + kStreamFrames * kFrame);
    const auto failures =
        failures_over_sizes("scored samples or probabilities differ", [&](auto size) {
            const ChunkedRun run = run_chunked(size);
            return run.all_fed && bits_of(run.samples) == bits_of(prefix) &&
                   bits_of(run.probs) == bits_of(frame_values());
        });
    EXPECT_EQ(failures, std::vector<std::string>{});
}

TEST(ChunkReaderFrames, PartialFrameStaysBufferedUnpadded) {
    const auto failures = failures_over_sizes("pending is not the remainder", [](auto size) {
        const ChunkedRun run = run_chunked(size);
        return run.all_fed && run.pending == kPartialSamples;
    });
    EXPECT_EQ(failures, std::vector<std::string>{});

    const SplitFrame expected{
        .fed = true,
        .frames_after_partial = 0,
        .detect_calls_after_partial = 2, // the two probe calls only
        .pending_after_partial = kFrame - 1,
        .frames_after_last = 1,
        .pending_after_last = 0,
        .detect_sizes = {kFrame, kFrame + 1, kFrame},
    };
    EXPECT_EQ(feed_frame_in_two_pieces(), expected);
}

TEST(ChunkReaderFrames, ResultIsIndependentOfChunkSize) {
    const ChunkedRun reference = run_chunked(kChunkSizes.front());
    const auto failures = failures_over_sizes("result differs from chunk 1", [&](auto size) {
        const ChunkedRun run = run_chunked(size);
        return run.all_fed && bits_of(run.samples) == bits_of(reference.samples) &&
               bits_of(run.probs) == bits_of(reference.probs) && run.pending == reference.pending;
    });
    EXPECT_TRUE(reference.all_fed);
    EXPECT_EQ(failures, std::vector<std::string>{});
}

TEST(ChunkReaderFrames, FrameViewIsOneFrameAtItsOffset) {
    FakeVadLog log;
    auto reader = lws::ChunkReader::create(lws::test::fake_engine(log, kFrame));
    ASSERT_TRUE(reader.has_value());
    const std::array<float, 3> values{0.25F, 0.5F, 0.75F};
    const auto scored = reader->feed(lws::test::scripted_stream(values));
    ASSERT_TRUE(scored.has_value());
    ASSERT_EQ(scored->size(), 3U);

    const auto failures =
        std::views::iota(std::size_t{0}, scored->size()) | std::views::filter([&](std::size_t i) {
            const auto view = scored->frame(i);
            return view.size() != kFrame || view.data() != &scored->samples.at(i * kFrame);
        }) |
        std::ranges::to<std::vector>();
    EXPECT_TRUE(failures.empty());
}

namespace {

using ResetEvent = void (lws::ChunkReader::*)();

bool fed_ok(lws::ChunkReader& reader, std::size_t count) {
    return reader.feed(std::vector<float>(count, 0.25F)).has_value();
}

// What a reset event did to a reader that held a partial frame, and what the next whole frame cost.
struct ResetOutcome {
    bool fed = false;
    std::size_t resets_before_event = 0;
    std::size_t pending_before_event = 0;
    std::size_t resets_after_event = 0;
    std::size_t pending_after_event = 0;
    std::vector<std::size_t> detect_sizes; // after the two probe calls

    friend bool operator==(const ResetOutcome&, const ResetOutcome&) = default;
};

ResetOutcome partial_frame_then_event(ResetEvent event) {
    ResetOutcome outcome;
    FakeVadLog log;
    auto reader = lws::ChunkReader::create(lws::test::fake_engine(log, kFrame));
    if (!reader) {
        return outcome;
    }
    const bool first_fed = fed_ok(*reader, 700); // one frame, 188 samples left over
    outcome.resets_before_event = lws::test::resets(log);
    outcome.pending_before_event = reader->pending();
    (*reader.*event)();
    outcome.resets_after_event = lws::test::resets(log);
    outcome.pending_after_event = reader->pending();
    const bool second_fed = fed_ok(*reader, kFrame);
    outcome.detect_sizes =
        lws::test::detect_sizes(log) | std::views::drop(2) | std::ranges::to<std::vector>();
    outcome.fed = first_fed && second_fed;
    return outcome;
}

// The partial frame's 188 samples must not be spliced in front of the 512 that follow the event, so
// the second detect call is exactly one frame.
ResetOutcome expected_outcome() {
    return {
        .fed = true,
        .resets_before_event = 1,
        .pending_before_event = 188,
        .resets_after_event = 2,
        .pending_after_event = 0,
        .detect_sizes = {kFrame, kFrame},
    };
}

} // namespace

TEST(ChunkReaderReset, OverrunResetsOnceAndDropsThePartialFrame) {
    EXPECT_EQ(partial_frame_then_event(&lws::ChunkReader::notify_overrun), expected_outcome());
}

TEST(ChunkReaderReset, RelinkResetsOnceAndDropsThePartialFrame) {
    EXPECT_EQ(partial_frame_then_event(&lws::ChunkReader::notify_relink), expected_outcome());
}

TEST(ChunkReaderReset, FeedingNeverResets) {
    FakeVadLog log;
    auto reader = lws::ChunkReader::create(lws::test::fake_engine(log, kFrame));
    ASSERT_TRUE(reader.has_value());

    constexpr std::array<std::size_t, 4> kSizes{1, 160, 513, 4096};
    const auto failed_feeds = std::views::iota(std::size_t{0}, std::size_t{1000}) |
                              std::views::filter([&](std::size_t i) {
                                  return !fed_ok(*reader, kSizes.at(i % kSizes.size()));
                              }) |
                              std::ranges::to<std::vector>();

    EXPECT_TRUE(failed_feeds.empty());
    EXPECT_EQ(lws::test::resets(log), 1U);
}

TEST(ChunkReaderReset, SessionOverrunRelinkGiveExactlyThreeResets) {
    FakeVadLog log;
    auto reader = lws::ChunkReader::create(lws::test::fake_engine(log, kFrame));
    ASSERT_TRUE(reader.has_value());

    const bool fed = fed_ok(*reader, kFrame);
    reader->notify_overrun();
    const bool fed_after_overrun = fed_ok(*reader, kFrame);
    reader->notify_relink();
    const bool fed_after_relink = fed_ok(*reader, kFrame);

    const std::vector<FakeCall> expected{
        {FakeCall::Kind::Detect, kFrame}, {FakeCall::Kind::Detect, kFrame + 1},
        {FakeCall::Kind::Reset, 0}, // session start, after the probe
        {FakeCall::Kind::Detect, kFrame}, {FakeCall::Kind::Reset, 0}, // overrun
        {FakeCall::Kind::Detect, kFrame}, {FakeCall::Kind::Reset, 0}, // relink
        {FakeCall::Kind::Detect, kFrame},
    };
    EXPECT_TRUE(fed && fed_after_overrun && fed_after_relink);
    EXPECT_EQ(lws::test::resets(log), 3U);
    EXPECT_EQ(log.calls, expected);
}
