# local-whisper-streaming

[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE) [![C++23](https://img.shields.io/badge/C%2B%2B-23-00599C.svg)](CMakeLists.txt) [![clang 21](https://img.shields.io/badge/clang-21-262D3A.svg)](CMakePresets.json) [![Linux](https://img.shields.io/badge/platform-Linux-lightgrey.svg)](README.md#build) [![Status: pre-alpha](https://img.shields.io/badge/status-pre--alpha-orange.svg)](#roadmap)

Real-time speech-to-text on a live audio stream with [whisper.cpp](https://github.com/ggml-org/whisper.cpp).
A lock-free ring buffer takes audio in, a Silero voice-activity detector decides which parts are speech,
and only those parts reach Whisper. Silence costs (almost) nothing.

> **Status: pre-alpha.** The pure core, the lock-free ring, the VAD engine, segmentation, the per-segment decoder
> (CPU and CUDA) and the streaming API (`lws::Streamer`, two threads, timed text events) are implemented and
> tested. The `lws-cli` and hosted CI are not started.
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
- **Segmentation** (`src/shell`): exact 512-sample frames, a segmenter that force-closes the open window when
  detection fails, and a pre-roll that keeps a quiet word onset intact (measured lead-in of 320 ms on recorded
  audio). The n_context difference between whisper.cpp's per-frame probabilities and the reference Silero is
  measured and accepted: maximum difference 0.20 on one frame, mean 0.002, no segment boundary shift.
- **Decoder** (`src/shell`, policy in `src/core`): one `whisper_full` call per closed segment, whisper's own VAD
  off in every call, segments under 100 ms return an empty result without a call, and `stop` interrupts an
  in-flight decode through the abort callback (measured stop-to-return: 3 to 19 ms on CPU). A segment longer
  than 30 s is decoded in one call and its tail is kept (whisper walks its 30 s windows inside that call).
  Text spans are timed in samples from the segment start.
- **CUDA** (built by hand, see below): the same decode on the GPU gives the same text as the CPU build on the
  recorded segments.
- **Streamer** (`include/lws/streamer.hpp`, `src/shell`): a pump thread cuts utterances with the VAD and a
  decoder thread decodes them, joined by a bounded segment queue. `push` is wait-free on the producer thread.
  Events arrive in stream order on the decoder thread: decoded text, and reports of an overrun, a VAD failure,
  a dropped segment or a failed decode, each with an exact sample range. Times count the offered stream,
  dropped samples included. `StopMode::Drain` decodes everything accepted before the stop and implies `flush()`;
  `StopMode::Immediate` interrupts an in-flight decode. A streamer is single-use. Threads are `jthread`s, and
  a text gate refuses a recursive mutex or a raw thread in the library.

Errors are returned as `std::expected`; no exceptions cross the public API.

## Usage

The streaming API is implemented; the `lws-cli` binary is not. Error results are shown as comments here.

```cpp
#include <lws/streamer.hpp>
#include <print>

auto created = lws::Streamer::create(lws::StreamerOptions{
    .model_path = "models/ggml-base.en.bin",
    .vad_model_path = "models/ggml-silero-v5.1.2.bin"});
if (!created) { /* created.error(): ModelLoad, InvalidStreamerOptions, ... */ }
lws::Streamer streamer = std::move(*created);

streamer.on_text([](const lws::TextEvent& event) {
    if (event.kind == lws::EventKind::Text) std::println("{}-{}: {}", event.t_start, event.t_end, event.text);
});
streamer.start();           // NotRunning / AlreadyStarted on misuse
// from an audio callback, 16 kHz mono float32; the span is copied before push returns:
streamer.push(samples);     // Overrun when the ring is full; the loss is reported as an event
streamer.stop(lws::StopMode::Drain);  // decodes everything accepted so far, then returns
```

Call `on_text` before `start`. Create a new streamer for each stream: a streamer cannot be restarted after `stop`.

## Build

Requirements: Linux, clang 21, CMake 3.29+, Conan 2. CUDA is optional.

```sh
git submodule update --init --recursive
./scripts/fetch-models.sh silero-v5.1.2 base.en tiny.en   # weights fetched by name; never committed
export LWS_BUILD_ID=dev                             # build/dev/ holds the Conan output and every preset's tree
./scripts/deps.sh Debug
cmake --preset asan-ubsan && cmake --build --preset asan-ubsan
ctest --preset asan-ubsan
```

The presets are `release`, `asan-ubsan` and `tsan`. `./scripts/check.sh` runs the full gate: format,
toolchain, tuning values, core isolation, ring wait-free, VAD API, threads, models, dependencies, every leg's
build and tests, cppcheck and clang-tidy.

### CUDA, built by hand

The `cuda` preset needs a CUDA 13 toolkit at `/usr/local/cuda` and clang 21. It is built and tested by hand,
never by `./scripts/check.sh`, and CUDA never runs in hosted CI.

```sh
export LWS_BUILD_ID=dev
./scripts/deps.sh Release
cmake --preset cuda
cmake --build --preset cuda
ctest --preset cuda
```

The preset pins nvcc because an older toolkit on `PATH` may reject clang 21. The host compiler is pinned to
`clang++-21`, and configure stops if CUDA would use another host. The GPU architecture is pinned for the card it
was built on (`86-real`); override it, or the toolkit path, in a local `CMakeUserPresets.json` that inherits `cuda`,
and remove that file before running the seeded-defect proofs, which refuse to start while one exists. A cold build compiles every CUDA kernel for that architecture and took about five
minutes on the machine it was written on.

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
3. [x] VAD on recorded audio: segments with word onsets intact, golden boundaries, chunking invariance.
4. [x] Decoder: one `whisper_full` call per closed segment, VAD off, 100 ms floor, stop through the abort
   callback, CPU and CUDA builds.
5. [x] Streamer: pump and decoder threads, bounded segment queue, timed text events, Drain and Immediate stop,
   overrun and VAD-failure reports, a thread gate.
6. [x] Public streamer API (`lws::Streamer`, pimpl header, no thread type in `include/lws`).
7. [ ] `lws-cli` that reads raw PCM from stdin (so `pwcap <app> | lws-cli` works), and the install check.

## License

Apache License 2.0. See [LICENSE](LICENSE).
