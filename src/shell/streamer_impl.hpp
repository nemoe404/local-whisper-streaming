#pragma once

#include <expected>

#include "lws/options.hpp"
#include "lws/streamer.hpp"
#include "lws/streamer_options.hpp"
#include "shell/decoder.hpp"
#include "shell/vad_engine.hpp"

// Shell header. StreamerAccess is the factory behind Streamer::create and the seam tests use to
// inject a fake VAD engine and a fake decoder: it builds a streamer from engines that are already
// open, so no model file is needed.

// Who owns what. Three threads meet in a streamer: the caller's (it creates, controls and
// destroys the streamer and may push from a producer thread), the pump thread and the decoder
// thread. Both models are opened on the calling thread before either worker exists, so the whisper
// log route and ggml's one-time setup run on one thread. At start the Segmenter is moved into the
// pump's callable and the Decoder into the decoder's callable; after that the streamer object
// holds neither. A Segment is moved into the queue by the pump and out of it by the decoder and is
// never shared. The only state two threads both reach is the ring's atomics, the queue behind its
// mutex, the pump's wake state behind its mutex, the run state atomic and each worker's stop token.
//
// clang-format off
//   object             owner thread  created on      destroyed on    reached by others through
//   -----------------  ------------  --------------  --------------  ------------------------------
//   ring               none, shared  calling thread  calling thread  producer push and pump pop, by
//                                                                    the ring's atomic indices only
//   Segmenter, with    pump          calling thread  pump thread,    nothing: it moves in at start
//   its reader,                      before start    when its        and no other thread holds it
//   windower and                                     callable ends
//   Silero context
//   Segment            the holder    pump thread     decoder thread  moved by value through the
//                                                                    queue, never shared
//   SegmentQueue       none, shared  calling thread  calling thread  pump push and close, decoder
//                                                                    pop, all behind its mutex
//   Decoder, with its  decoder       calling thread  decoder thread, nothing: it moves in at start;
//   whisper context                  before start    when its        its stop token is all another
//                                                    callable ends   thread touches
//   callback           decoder       calling thread  decoder thread  nothing: it moves in at start
//                                    in on_text                      and runs on the decoder thread
//   pump wake state    none, shared  calling thread  calling thread  control calls and the pump,
//                                                                    behind the pump's mutex
//   run state          none, shared  calling thread  calling thread  one atomic, acquire/release
// clang-format on
//
// Stopping and destroying a streamer both end with the two workers joined, before the queue or the
// ring is released, so no worker outlives what it uses.
namespace lws {

struct StreamerAccess {
    // Builds the segmenter from the engine and the options' VAD settings (its error is returned
    // unchanged) and wraps everything in a streamer that has not started.
    [[nodiscard]] static std::expected<Streamer, Error> assemble(VadEngine engine, Decoder decoder,
                                                                 const StreamerOptions& options);
};

} // namespace lws
