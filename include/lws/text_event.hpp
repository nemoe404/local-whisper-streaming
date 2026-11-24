#pragma once

#include <cstdint>
#include <string>

// Public header. No whisper, spdlog or thread type crosses it. One struct carries every report the
// library makes, so text and losses share one order: the order of the stream.
namespace lws {

// What an event reports. Text is the only kind that carries words; every other kind reports a loss.
enum class EventKind : std::uint8_t {
    // Decoded words for one utterance.
    Text,
    // The ring buffer refused audio because it was full. The range is the dropped samples. The
    // dropped length is exact. The reported start is not: it may be late by less than the rejected
    // span plus one pump piece (2048 samples), or early by the samples accepted between the pump's
    // two consecutive reads of the ring. Every position after that stretch is exact.
    Overrun,
    // The voice detector failed on some frames. The range is the frames the failed call lost.
    VadFailure,
    // The decode queue was full and the oldest waiting segment was discarded. The range is that
    // segment.
    SegmentDropped,
    // The decoder failed on a segment. The range is that segment.
    DecodeFailure,
};

struct TextEvent {
    EventKind kind = EventKind::Text;
    // Times count samples of the offered stream at 16 kHz, dropped samples included, so a time
    // stays right after a loss. t_end is one past the last sample. For a kind other than Text the
    // range is what was lost.
    std::uint64_t t_start = 0;
    std::uint64_t t_end = 0;
    // Empty unless the kind is Text.
    std::string text;
    // Set only on the first Text event after an overrun: audio was lost just before it.
    bool after_gap = false;

    friend bool operator==(const TextEvent&, const TextEvent&) = default;
};

} // namespace lws
