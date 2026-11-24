#pragma once

#include "lws/options.hpp"
#include "lws/streamer_options.hpp"
#include "lws/text_event.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>

// Public header. No whisper, spdlog or thread type crosses it: the implementation sits behind a
// pointer, so including this header pulls in no thread header.
namespace lws {

// How stop ends the stream. Drain finishes the work already accepted; Immediate gives it up.
enum class StopMode : std::uint8_t { Immediate, Drain };

// Receives every event, in stream order. It is called only on the decoder thread, never twice at
// once, and never while the library holds a lock. It must not throw, and it must not call stop or
// destroy the streamer.
using TextCallback = std::move_only_function<void(const TextEvent&) noexcept>;

// Turns pushed audio into timed text events: a voice detector picks the speech, only speech goes
// to the decoder, and one event comes out per utterance. Two threads run inside: the pump reads
// samples and cuts utterances, the decoder decodes them and calls back.
//
// Threading: push comes from one producer thread and is wait-free. The control calls (on_text,
// start, flush, stop, destruction) come from one thread at a time and never from the callback.
//
// Lifecycle: a streamer is used once. It is created, then running, then stopped. To go again,
// create a new one: create, start, stop, destroy.
class Streamer {
public:
    // Opens both models on the calling thread. Nothing runs yet.
    [[nodiscard]] static std::expected<Streamer, Error> create(const StreamerOptions& options);

    Streamer(Streamer&&) noexcept;
    Streamer& operator=(Streamer&&) noexcept;
    Streamer(const Streamer&) = delete;
    Streamer& operator=(const Streamer&) = delete;
    // Stops as Immediate and joins both threads, from any state.
    ~Streamer();

    // Sets the callback. Call it before start; once the streamer runs it has no effect. Without a
    // callback, events are dropped.
    [[nodiscard]] std::expected<void, Error> on_text(TextCallback callback);

    // Starts the pump and decoder threads.
    [[nodiscard]] std::expected<void, Error> start();

    // Copies the samples into the ring before it returns and keeps no reference to the span.
    // Wait-free: no lock and no wake-up, the pump finds the samples by polling. A span that does
    // not fit returns Error::Overrun and nothing is stored. The pump then reports the loss as an
    // Overrun event whose length is exact. Its start is placed where the pump first notices the
    // loss: it may be late by less than the rejected span plus one piece (2048 samples), or early
    // by the samples accepted between the pump's two consecutive reads of the ring.
    //
    // Cross-thread rule: a push that races stop from another thread is either refused with
    // NotRunning or accepted into the ring. An accepted push that a Drain stop does not cover, and
    // every sample still in the ring at an Immediate stop, is discarded with the ring and gives
    // no event. Only pushes ordered before the stop call are promised to a Drain stop, so such a
    // success return is a stated outcome and never a silent loss.
    [[nodiscard]] std::expected<void, Error> push(std::span<const float> samples) noexcept;

    // Closes the open utterance at the samples accepted before the call. Every flush is honoured,
    // in order, at its exact sample position. The last partial VAD frame (under 32 ms) is never
    // scored.
    [[nodiscard]] std::expected<void, Error> flush();

    // Ends the stream and joins both threads. Drain reads the samples accepted before the call as
    // its target, drains the ring to that target, closes the open window, decodes every queued
    // segment and delivers every queued event before it returns. Immediate interrupts an
    // in-flight decode and discards queued segments and notices without events.
    //
    // Cross-thread rule: a push that races this call from another thread is either refused or
    // accepted; an accepted push the Drain target does not cover is discarded with the ring, and
    // only pushes ordered before the call are promised to a Drain stop.
    [[nodiscard]] std::expected<void, Error> stop(StopMode mode);

private:
    class Impl;
    friend struct StreamerAccess;
    explicit Streamer(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace lws
