#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <span>
#include <stop_token>
#include <vector>

#include "lws/decode_policy.hpp"
#include "lws/options.hpp"
#include "lws/segment.hpp"

// Shell header: the decoder over whisper.cpp's whisper_full. One Decoder owns one whisper context
// and decode is called from one thread at a time; the stop token is the only thing another thread
// touches. It includes no whisper header, so tests that inject a backend never need one.

// whisper.cpp's C struct. It is complete only where whisper.h is included.
struct whisper_full_params;

namespace lws {

// One decode of the whole span. Returns 0 on success, whisper's non-zero code otherwise.
using DecodeRunFn = std::move_only_function<std::int32_t(std::span<const float>, std::stop_token)>;

// The segments of the last successful run, in order, times in 10 ms units.
using DecodeSpansFn = std::move_only_function<std::vector<RawSpan>()>;

// A backend's two members are used together and are never empty.
struct DecodeBackend {
    DecodeRunFn run;
    DecodeSpansFn spans;
};

class Decoder {
public:
    // Both members of the backend must be non-empty.
    explicit Decoder(DecodeBackend backend) noexcept;

    // Validates the options first and returns its error as is, before any model is touched, then
    // loads the model. Error::ModelLoad if it cannot be loaded.
    [[nodiscard]] static std::expected<Decoder, Error> open(const std::filesystem::path& model,
                                                            const DecoderOptions& options);

    // Decodes one closed segment. The order is fixed:
    //  - fewer samples than whisper accepts: DecodeStatus::TooShort, no spans, no call;
    //  - a sample count that does not fit whisper's int: Error::DecodeFailed, no call;
    //  - otherwise exactly one run call, counted, and its result is classified before any span is
    //    read: Ok reads and maps the spans, Cancelled returns no spans, Failed is
    //    Error::DecodeFailed.
    // Span times count samples from the segment's first sample.
    [[nodiscard]] std::expected<DecodeResult, Error> decode(const Segment& segment,
                                                            const std::stop_token& stop);

    // Run calls made so far, and nothing else.
    [[nodiscard]] std::uint64_t whisper_calls() const noexcept;

private:
    DecodeBackend backend_;
    std::uint64_t calls_ = 0;
};

// What whisper's abort callback reads. It lives on the stack of one run call.
struct AbortHook {
    std::stop_token token;
};

// Whisper's abort callback: true means abort now. Allocation-free and silent, because whisper
// calls it from inside its compute loop.
[[nodiscard]] bool abort_poll(void* hook) noexcept;

// The parameters of the one whisper_full call. The result borrows options.language and hook, which
// must outlive the call.
[[nodiscard]] whisper_full_params build_decode_params(const DecoderOptions& options,
                                                      AbortHook& hook) noexcept;

} // namespace lws
