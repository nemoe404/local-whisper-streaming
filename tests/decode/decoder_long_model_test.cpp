// Whisper decodes an input longer than one 30 s window by moving through it in sequential windows
// inside a single whisper_full call. These tests pin that one Decoder call covers a whole segment
// that is longer than a window, and that the end of the input survives: the last span ends near
// the end of the segment and the last words of the clip are in the text. A second test walks the
// same input through whisper directly and counts the encoder windows, so the log shows that more
// than one window was really walked.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <ranges>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <fmt/base.h>
#include <whisper.h>

#include <gtest/gtest.h>

#include "decode/decode_support.hpp"
#include "lws/decode_policy.hpp"
#include "lws/segment.hpp"
#include "shell/decoder.hpp"
#include "shell/whisper_log.hpp"

namespace {

constexpr std::size_t kLongSegmentSamples = 532960;
// The last span must end within one second of the segment's end.
constexpr std::int64_t kTailMarginSamples = 16000;
constexpr const char* kDroppedTailMessage =
    "whisper dropped the tail of a segment longer than its window";

struct ContextFree {
    void operator()(whisper_context* ctx) const noexcept { whisper_free(ctx); }
};

using Context = std::unique_ptr<whisper_context, ContextFree>;

std::string joined_text(const std::vector<lws::TextSpan>& spans) {
    return spans | std::views::transform([](const lws::TextSpan& s) { return s.text + " "; }) |
           std::views::join | std::ranges::to<std::string>();
}

bool ends_with_last_words(const lws::test::Words& words) {
    const lws::test::Words expected{"SEE", "HIS", "WAY", "OUT"};
    return words.size() >= expected.size() &&
           std::ranges::equal(std::span(words).last(expected.size()), expected);
}

bool spans_are_ordered_inside(const std::vector<lws::TextSpan>& spans, std::uint64_t samples) {
    const bool inside = std::ranges::all_of(
        spans, [samples](const lws::TextSpan& s) { return s.begin <= s.end && s.end <= samples; });
    const bool ordered = std::ranges::is_sorted(spans, {}, &lws::TextSpan::begin);
    return inside && ordered;
}

void print_spans(const std::vector<lws::TextSpan>& spans) {
    std::ranges::for_each(spans, [](const lws::TextSpan& s) {
        fmt::println("LONG-SPAN model={} begin={} end={} text={}", LWS_WHISPER_MODEL_NAME, s.begin,
                     s.end, s.text);
    });
}

std::int64_t last_end_of(const std::vector<lws::TextSpan>& spans) {
    return spans.empty() ? -1 : static_cast<std::int64_t>(spans.back().end);
}

struct LongDecode {
    lws::DecodeResult result;
    std::uint64_t calls = 0;
};

// The three-copy segment through one Decoder call with a token that is never stopped.
std::expected<LongDecode, std::string> decode_long_segment() {
    const auto clip = lws::test::load_fixture_wav("clean_en_sample.wav");
    if (!clip) {
        return std::unexpected(clip.error());
    }
    const lws::Segment segment{.samples = lws::test::three_copies(*clip)};
    if (segment.samples.size() != kLongSegmentSamples) {
        return std::unexpected("the long segment has an unexpected length");
    }
    auto decoder = lws::Decoder::open(LWS_WHISPER_MODEL_PATH, lws::DecoderOptions{});
    if (!decoder) {
        return std::unexpected("model load failed");
    }
    const std::stop_source running;
    auto decoded = decoder->decode(segment, running.get_token());
    if (!decoded) {
        return std::unexpected("decode returned an error");
    }
    return LongDecode{.result = std::move(*decoded), .calls = decoder->whisper_calls()};
}

bool tail_is_kept(const std::vector<lws::TextSpan>& spans) {
    return last_end_of(spans) >=
               static_cast<std::int64_t>(kLongSegmentSamples) - kTailMarginSamples &&
           ends_with_last_words(lws::test::normalised_words(joined_text(spans)));
}

struct RawWalk {
    int rc = 0;
    std::int32_t windows = 0;
    int segments = 0;
    std::int64_t last_t1_cs = -1;
};

// One whisper_full call over the three-copy input, counting the encoder windows it walks.
std::expected<RawWalk, std::string> walk_windows() {
    const auto clip = lws::test::load_fixture_wav("clean_en_sample.wav");
    if (!clip) {
        return std::unexpected(clip.error());
    }
    const std::vector<float> samples = lws::test::three_copies(*clip);
    lws::install_whisper_log_route();
    const Context ctx{whisper_init_from_file_with_params(LWS_WHISPER_MODEL_PATH,
                                                         whisper_context_default_params())};
    if (!ctx) {
        return std::unexpected("model load failed");
    }

    lws::AbortHook hook;
    // The parameters borrow the language string from the options, so the options outlive the call.
    const lws::DecoderOptions options;
    whisper_full_params params = lws::build_decode_params(options, hook);
    RawWalk walk;
    // True means continue: the callback only counts, it never cancels.
    params.encoder_begin_callback = [](whisper_context* /*ctx*/, whisper_state* /*state*/,
                                       void* user_data) {
        ++*static_cast<std::int32_t*>(user_data);
        return true;
    };
    params.encoder_begin_callback_user_data = &walk.windows;

    walk.rc = whisper_full(ctx.get(), params, samples.data(), static_cast<int>(samples.size()));
    walk.segments = whisper_full_n_segments(ctx.get());
    if (walk.segments > 0) {
        walk.last_t1_cs = whisper_full_get_segment_t1(ctx.get(), walk.segments - 1);
    }
    return walk;
}

} // namespace

TEST(DecoderLongModel, OverThirtySecondsDecodesInOneCallAndKeepsTheTail) {
    const auto run = decode_long_segment();
    ASSERT_TRUE(run.has_value()) << run.error();
    const std::vector<lws::TextSpan>& spans = run->result.spans;
    ASSERT_EQ(run->result.status, lws::DecodeStatus::Decoded);
    EXPECT_EQ(run->calls, 1U);
    ASSERT_FALSE(spans.empty());
    EXPECT_TRUE(spans_are_ordered_inside(spans, kLongSegmentSamples));

    const bool kept = tail_is_kept(spans);
    print_spans(spans);
    fmt::println("LONG-DECODE model={} samples={} spans={} last_end={} tail={}",
                 LWS_WHISPER_MODEL_NAME, kLongSegmentSamples, spans.size(), last_end_of(spans),
                 kept ? "kept" : "dropped");
    EXPECT_TRUE(kept) << kDroppedTailMessage;
}

TEST(DecoderLongModel, WhisperWalksAtLeastTwoWindowsInOneCall) {
    const auto walk = walk_windows();
    ASSERT_TRUE(walk.has_value()) << walk.error();
    fmt::println("LONG-WINDOWS model={} windows={} last_t1_cs={}", LWS_WHISPER_MODEL_NAME,
                 walk->windows, walk->last_t1_cs);

    ASSERT_EQ(walk->rc, 0);
    ASSERT_GE(walk->windows, 2);
    ASSERT_GE(walk->segments, 1);
    EXPECT_GE(walk->last_t1_cs * 160, 516960) << kDroppedTailMessage;
}
