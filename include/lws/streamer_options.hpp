#pragma once

#include "lws/decode_policy.hpp"
#include "lws/options.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>

// Public header. No whisper, spdlog or thread type crosses it. Every default is a member
// initializer, so a caller overrides a field by name and leaves the rest alone.
namespace lws {

struct StreamerOptions {
    // The Whisper model file the decoder loads.
    std::filesystem::path model_path;
    // The Silero VAD model file the voice detector loads.
    std::filesystem::path vad_model_path;
    // Voice-detection and segmentation tuning.
    Options vad;
    // Decoder tuning.
    DecoderOptions decoder;
    // Slots of the sample ring between the producer and the pump: 160000 floats hold ten seconds
    // at 16 kHz. Size it from the largest push you will make, because a span longer than the ring
    // is always rejected.
    std::size_t ring_capacity_samples = 160000;
    // Segments that may wait for the decoder, besides the one being decoded. When the decoder
    // falls behind, the oldest waiting segment is discarded and reported.
    std::size_t queue_segments = 8;
    // How long the pump sleeps on an empty ring before it looks again. It adds at most this to
    // an event that already waits for the speaker to stop.
    std::chrono::milliseconds poll{32};
};

} // namespace lws
