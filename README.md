# local-whisper-streaming

Real-time speech-to-text on a live audio stream with [whisper.cpp](https://github.com/ggml-org/whisper.cpp).
A lock-free ring buffer takes audio in, a Silero voice-activity detector decides which parts are speech,
and only those parts reach Whisper. Silence costs (almost) nothing.

> **Status: pre-alpha.** The pure core, the lock-free ring and the VAD engine are implemented and tested.
> Segmentation on recorded audio is in progress; the decoder, the streaming API and the CLI are not started.
> See [Roadmap](#roadmap).

## Why

`whisper-cli` and `whisper_full` work on finished files. Live audio is different: you must cut it into
utterances without waiting for the speaker to stop, skip the silence between them, and keep the inference
cost bounded. Running Whisper on a fixed timer wastes compute on quiet stretches and cuts words in half.
This library does the cutting:

```
audio in ──► ring buffer ──► VAD (Silero, whisper.cpp) ──► speech segments ──► whisper.cpp ──► text events
  (push)      (lock-free)       open / hangover / close        (pre-roll padded)      (worker thread)
```

## What is implemented

- **Pure core** (`include/lws`, `src/core`): `Options` with validation, ring index arithmetic, the VAD
  windower (onset, hangover, pre-roll, ceiling cut with overlap and continuation), and seam merge for
  repeated words at window joins. No I/O, no threads, no whisper dependency.
- **Lock-free ring** (`include/lws/spsc_ring.hpp`): single-producer / single-consumer, drop-newest
  all-or-nothing `push`, counters for offered and dropped samples, and a discontinuity flag. The push path
  does not allocate, does not block and contains no compare-and-swap.
- **VAD engine** (`src/shell`): Silero through whisper.cpp's `whisper_vad_*` API only, with a startup
  frame-size probe, CPU and one thread, and whisper's log routed away from stderr.

Errors are returned as `std::expected`; no exceptions cross the public API.

## Usage

Not yet available. The target API:

```cpp
#include <lws/streamer.hpp>

lws::Streamer streamer{lws::Options{.model_path = "ggml-small.bin", .vad_model_path = "silero.bin"}};
streamer.on_text([](lws::TextEvent event) { std::println("{}", event.text); });
streamer.start();
// from an audio callback, 16 kHz mono float32:
streamer.push(samples);
```

## Build

Requirements: Linux, clang 21, CMake 3.29+, Conan 2. CUDA is optional.

```sh
git submodule update --init --recursive
./scripts/fetch-models.sh ggml-silero-v5.1.2.bin   # Silero VAD weights; fetched, never committed
export LWS_BUILD_ID=dev                             # build/dev/ holds the Conan output and every preset's tree
./scripts/deps.sh Debug
cmake --preset asan-ubsan && cmake --build --preset asan-ubsan
ctest --preset asan-ubsan
```

The presets are `release`, `asan-ubsan` and `tsan`. `./scripts/check.sh` runs the full gate: format,
toolchain, tuning values, core isolation, ring wait-free, models, dependencies, every leg's build and tests,
cppcheck and clang-tidy.

## Built on a [whisper.cpp](https://github.com/nemoe404/whisper.cpp/tree/consigliere-v1.9.5) fork

The build uses `nemoe404/whisper.cpp`, branch `consigliere-v1.9.5`: upstream v1.9.5 plus the patches that
[consigliere](https://consigliere.observer) needs (CUDA kernel set, CUDA device visibility, abort handling
inside the graph). Upstream whisper.cpp is MIT-licensed; so is the fork's delta.

## Related

[**consigliere**](https://consigliere.observer) is a visible interview-preparation assistant for Linux
desktops. It runs this same pipeline (ring buffer, VAD, local Whisper) for live interviewer audio, and
adds transcription, OCR and LLM answer drafting on top. This library is the speech core, extracted for
anyone who needs real-time Whisper without the rest.

## Roadmap

1. [x] Pure core: options, ring index, VAD windower, seam merge, replayed on plain values.
2. [x] Lock-free SPSC ring with counters, discontinuity flag and a ThreadSanitizer proof.
3. [ ] VAD on recorded audio: segments with word onsets intact, golden boundaries, chunking invariance.
4. [ ] Whisper shell: model loading, per-segment decode, worker thread, timed text events.
5. [ ] Public streamer API and a `lws-cli` that reads raw PCM from stdin
   (so `pwcap <app> | lws-cli` works).
6. [ ] CI: static gate on hosted runners (CPU build), GPU build by hand.

## License

Apache License 2.0. See [LICENSE](LICENSE).
