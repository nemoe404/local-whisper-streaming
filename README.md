# local-whisper-streaming

Real-time speech-to-text on a live audio stream with [whisper.cpp](https://github.com/ggml-org/whisper.cpp).
A lock-free ring buffer takes audio in, a Silero voice-activity detector decides which parts are speech,
and only those parts reach Whisper. Silence costs (almost) nothing.

> **Status: pre-alpha.** Nothing is implemented yet. This repository currently holds the plan and the
> build skeleton. See [Roadmap](#roadmap).

## Why

`whisper-cli` and `whisper_full` work on finished files. Live audio is different: you must cut it into
utterances without waiting for the speaker to stop, skip the silence between them, and keep the inference
cost bounded. Running Whisper on a fixed timer wastes compute on quiet stretches and cuts words in half.
This library does the cutting:

```
audio in ──► ring buffer ──► VAD (Silero, whisper.cpp) ──► speech segments ──► whisper.cpp ──► text events
  (push)      (lock-free)       open / hangover / close        (pre-roll padded)      (worker thread)
```

## What it does

- Lock-free single-producer / single-consumer ring for audio pushed from a capture callback.
- Silero VAD with onset, hangover and minimum-speech thresholds; no inference on silence.
- Segment assembly with pre-roll padding, so the first syllable is not lost.
- A worker thread that runs whisper.cpp per segment and emits text events through a callback.
- Errors as `std::expected`; no exceptions across the public API.

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

Requirements: Linux, clang 21, CMake 3.28+, Conan 2. CUDA is optional.

```sh
git submodule update --init --recursive
./scripts/fetch-models.sh   # Silero VAD weights; fetched, never committed
cmake --preset debug && cmake --build --preset debug
ctest --preset debug
```

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

1. Ring buffer and VAD windower as pure value types, tested without mocks.
2. whisper.cpp shell: model loading, segment decode, worker thread.
3. Public streamer API and a `lws-cli` that reads raw PCM from stdin
   (so `pwcap <app> | lws-cli` works).
4. CI: static gate on hosted runners (CPU build), GPU build by hand.

## License

Apache License 2.0. See [LICENSE](LICENSE).
