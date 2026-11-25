# local-whisper-streaming

[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE) [![C++23](https://img.shields.io/badge/C%2B%2B-23-00599C.svg)](CMakeLists.txt) [![clang 21](https://img.shields.io/badge/clang-21-262D3A.svg)](CMakePresets.json) [![Linux](https://img.shields.io/badge/platform-Linux-lightgrey.svg)](README.md#build) [![Version: 0.1.0](https://img.shields.io/badge/version-0.1.0-blue.svg)](CHANGELOG.md)

Real-time speech-to-text on a live audio stream with [whisper.cpp](https://github.com/ggml-org/whisper.cpp).
A lock-free ring buffer takes audio in, a Silero voice-activity detector decides which parts are speech,
and only those parts reach Whisper. Silence costs (almost) nothing (M4).

> **Status: 0.1.0.** Everything listed below is implemented and tested: the pure core, the lock-free ring, the
> VAD engine, segmentation, the per-segment decoder (CPU and CUDA), the streaming API (`lws::Streamer`, two
> threads, timed text events), the `lws-cli` binary and the installable CMake package. The gate runs on the
> developer's machine with `./scripts/check.sh`; CUDA is built and tested by hand. Every number in this file is a
> row of the [Measured](#measured) table or is marked as a design constant.
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

Silence is cheap because the VAD is the only thing that runs on it: in M4, 60 s of silence cost 0.36 s of CPU
against 18.53 s for 60 s of speech (M4).

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
  detection fails, and a pre-roll that keeps a quiet word onset intact (measured lead-in, M8). whisper.cpp
  scores each frame without the previous frame's samples that the reference Silero prepends. The difference was
  measured against the reference and accepted; the figures are in
  [`tests/fixtures/audio/README.md`](tests/fixtures/audio/README.md).
- **Decoder** (`src/shell`, policy in `src/core`): one `whisper_full` call per closed segment, whisper's own VAD
  off in every call, segments under 100 ms return an empty result without a call <!-- const -->, and `stop`
  interrupts an in-flight decode through the abort callback (measured stop-to-return, M6). A segment longer
  than 30 s is decoded in one call and its tail is kept <!-- const -->; whisper walks its own windows inside
  that call. Text spans are timed in samples from the segment start.
- **CUDA** (built by hand, see below): the same decode on the GPU gives the same text as the CPU build on the
  recorded segments. Its measured speed is row M7.
- **Streamer** (`include/lws/streamer.hpp`, `src/shell`): a pump thread cuts utterances with the VAD and a
  decoder thread decodes them, joined by a bounded segment queue. `push` is wait-free on the producer thread.
  Events arrive in stream order on the decoder thread: decoded text, and reports of an overrun, a VAD failure,
  a dropped segment or a failed decode, each with an exact sample range. Times count the offered stream,
  dropped samples included. `StopMode::Drain` decodes everything accepted before the stop and implies `flush()`;
  `StopMode::Immediate` interrupts an in-flight decode. A streamer is single-use. Threads are `jthread`s, and
  a text gate refuses a recursive mutex or a raw thread in the library.
- **`lws-cli`** (`src/cli`): reads raw samples from stdin and prints one line per text event, so
  `pwcap capture ... | lws-cli ...` transcribes a live application. See [Command line](#command-line).
- **Install package**: `cmake --install` of the release build gives `find_package(lws)` and the target
  `lws::lws`, with the whisper and ggml archives inside. See [Install](#install).

Errors are returned as `std::expected`; no exceptions cross the public API.

## Usage

Error results are shown as comments here.

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

### Command line

```
usage: lws-cli --model PATH --vad-model PATH [options]

  --model PATH         whisper model file (required)
  --vad-model PATH     Silero voice detector model file (required)
  --language CODE      spoken language (default en)
  --threads N          decoder threads, at least 1 (default 4)
  --cpu                decode on the CPU, not the GPU
  --ring-seconds S     audio the ring holds, a decimal above 0 (default 60)
  --speed X            push at X times real time; 0 pushes as fast as input arrives (default 0)
  --queue N            finished segments that may wait for the decoder, at least 1 (default 8)
  --stats              print one line of measurements on stderr at the end
  --help               print this text and exit
```

`lws-cli --help` prints the full text. Standard input is raw 16 kHz mono little-endian `f32` samples, nothing
else: no WAV header. Each text event is one line on stdout, stamped with the stream time of the segment:

```
[00:00:00.064 --> 00:00:10.704]  By the time he reached his own street, ...
```

Losses and failures (an overrun, a dropped segment, a failed decode, a decode that follows a gap) are reported on
stderr, so stdout holds text only. The exit code is 0 when the input ended or a signal arrived and the stream
was drained, 1 on a runtime error and 2 on a usage error. `--stats` adds one `stats:` line on stderr with the
audio length, wall time, speed, load time, lag percentiles, peak memory and CPU time; the Measured table is made
with it.

To transcribe one application's output with [pwcap](https://github.com/nemoe404/pipewire-app-capture), ask pwcap
for the format `lws-cli` reads. pwcap defaults to `s16le`, which `lws-cli` would take for noise:

```sh
pwcap capture --app NAME --rate 16000 --channels 1 --format f32le \
  | lws-cli --model models/ggml-base.en.bin --vad-model models/ggml-silero-v5.1.2.bin
```

To replay a file, convert it first. The repository's converter reads a 16-bit WAV, or use ffmpeg:

```sh
python3 -I scripts/make-f32.py in.wav out.f32
ffmpeg -i in.wav -f f32le -ac 1 -ar 16000 - | lws-cli --model models/ggml-base.en.bin --vad-model models/ggml-silero-v5.1.2.bin
```

The library has no backpressure: `push` rejects audio when the ring is full. A replay that is read faster than
real time should therefore use `--speed 1` or a larger `--ring-seconds`; the ring holds 60 s by default <!-- const -->.

## Install

The release preset builds the installable package. Only that preset is packaged.

```sh
export LWS_BUILD_ID=dev
./scripts/deps.sh Release
cmake --preset release
cmake --build --preset release
cmake --install build/dev/release --prefix /opt/lws
```

A project that uses it:

```cmake
find_package(lws 0.1 CONFIG REQUIRED)
target_link_libraries(app PRIVATE lws::lws)
```

The consumer needs fmt, found as a CMake config package, and Threads. The whisper and ggml archives ship inside
the package, so the consumer does not build or find them. A consumer that also links its own whisper gets
duplicate symbols. `bin/lws-cli` is installed next to the library.

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
toolchain, tuning values, core isolation, ring wait-free, VAD API, threads, public API, README numbers,
models, dependencies, every leg's build and tests, the install check, cppcheck and clang-tidy. There is no
hosted CI.

### CUDA, built by hand

The `cuda` preset needs a CUDA 13 toolkit at `/usr/local/cuda` and clang 21. It is built and tested by hand,
never by `./scripts/check.sh`.

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
and remove that file before running the seeded-defect proofs, which refuse to start while one exists. A cold build
compiles every CUDA kernel for that architecture; its measured time is row M9.

## Measured

Every number the library claims about itself is a row below, with the machine, the models, the commits, the command
and the date it was taken. Rows are rerun when the code they measure changes.

| Id | What | Value | Machine | Model | lws commit | fork commit | Command | Date |
|----|------|-------|---------|-------|------------|-------------|---------|------|
| M1 | Lag from the push that held an utterance's last sample to its text, paced 1x, 20 utterances. Includes the 700 ms hangover, the queue wait, the decode and up to one 32 ms poll. CPU build. | median 1491.4 ms, p95 1704.3 ms, max 1704.5 ms (text_events=20) | i5-12500H, 16 logical cores, Ubuntu 26.04.1 | base.en `a03779c8` with Silero `29940d98` | `a65b834` | `0d0f7531` | `$L --model $M --vad-model $V --speed 1 --stats < $IN/speech20.f32` | 2026-11-25 |
| M2 | Processing speed unpaced (audio seconds over start-to-stop seconds), median of 3, CPU build. No loss line in any run. | median 14.77x (14.84, 14.77, 14.56) | i5-12500H, 16 logical cores, Ubuntu 26.04.1 | base.en `a03779c8` with Silero `29940d98` | `a65b834` | `0d0f7531` | `$L --model $M --vad-model $V --ring-seconds 300 --queue 32 --stats < $IN/speech20.f32` | 2026-11-25 |
| M3 | Peak resident memory, CPU build | 251.0 MiB paced (M1); 274.4, 274.7, 274.9 MiB unpaced (M2); 195.4 to 195.8 MiB load only (M5) | i5-12500H, 16 logical cores, Ubuntu 26.04.1 | base.en `a03779c8` with Silero `29940d98` | `a65b834` | `0d0f7531` | as M1, M2, M5 | 2026-11-25 |
| M4 | CPU seconds (user + sys) for 60 s of input at 1x, CPU build | silence 0.36 s (0.25 + 0.11); speech 18.53 s (18.32 + 0.21) | i5-12500H, 16 logical cores, Ubuntu 26.04.1 | base.en `a03779c8` with Silero `29940d98` | `a65b834` | `0d0f7531` | `$L --model $M --vad-model $V --speed 1 --stats < $IN/zeros60.f32` and `< $IN/speech60.f32` | 2026-11-25 |
| M5 | Model load time (first statement of main to `start()` returning), median of 5, CPU build | 98.1 ms (101.6, 98.8, 96.3, 98.1, 96.3) | i5-12500H, 16 logical cores, Ubuntu 26.04.1 | base.en `a03779c8` with Silero `29940d98` | `a65b834` | `0d0f7531` | `$L --model $M --vad-model $V --stats < /dev/null` | 2026-11-25 |
| M6 | Decoder stop-to-return, 3 repeats of 3 delays | 0.320 ms to 17.918 ms (delay 20: 7.467, 11.890, 3.684; delay 50: 9.265, 0.320, 8.072; delay 100: 17.918, 7.250, 16.311) | i5-12500H, 16 logical cores, Ubuntu 26.04.1 | base.en `a03779c8` | `a65b834` | `0d0f7531` | `build/gate/release/tests/decode_decoder_stop_model_test --gtest_repeat=3` | 2026-11-25 |
| M7 | GPU lag and speed on the cuda preset binary, and the same binary with `--cpu`. | Lag paced 1x on the GPU: median 674.3 ms, p95 929.0 ms, max 933.0 ms (text_events=20). Speed unpaced on the GPU: median 112.82x (111.85, 113.26, 112.82). Same binary with `--cpu`: median 13.92x (13.80, 13.92, 14.57). GPU over CPU: 8.1x. Peak RSS 446.7 MiB paced GPU, 472.7 to 473.3 MiB unpaced GPU, 390.1 to 390.6 MiB `--cpu` on this binary. During the paced GPU run `nvidia-smi --query-compute-apps` listed the process with 406 MiB. | i5-12500H, 16 logical cores, Ubuntu 26.04.1, RTX 3050 Laptop 4096 MiB | base.en `a03779c8` with Silero `29940d98` | `a65b834` | `0d0f7531` | `$C --model $M --vad-model $V --speed 1 --stats < $IN/speech20.f32` (GPU lag); `$C ... --ring-seconds 300 --queue 32 --stats < $IN/speech20.f32` x3 (GPU speed); the same with `--cpu` x3 | 2026-11-25 |
| M8 | Pre-roll lead-in on the recorded fixture | lead_in_ms 320 for onset_frames 1, 2, 4 and 8 (plant_ms 192, expected_ms 320, consigliere_300ms_pad_ms 76) | i5-12500H, 16 logical cores, Ubuntu 26.04.1 | Silero `29940d98` | `a65b834` | `0d0f7531` | `build/gate/release/tests/vad_preroll_model_test --gtest_filter='PrerollModel.QuietOnsetSurvivesForEveryOnsetFrameCount'` | 2026-11-25 |
| M9 | Cold build of the cuda preset (324 ninja steps, includes the preset's test executables) | 5:41.86 wall (342 s), max RSS 867076 kB, exit 0, then `ctest --preset cuda`: 363 passed, 0 failed, 0 skipped | i5-12500H, 16 logical cores, Ubuntu 26.04.1, RTX 3050 Laptop 4096 MiB | none for the build; the tests load the pinned models | `a65b834` | `0d0f7531` | `LWS_BUILD_ID=measure CCACHE_DISABLE=1`, `rm -rf build/measure/cuda`, `scripts/deps.sh Release && cmake --preset cuda`, then `/usr/bin/time -v cmake --build --preset cuda` | 2026-11-25 |

Inputs. `IN` is any directory. The speech inputs repeat the fixture clip with silence between the copies, and the
silence input is zeros. They are made with `scripts/make-f32.py`:

```sh
python3 -I scripts/make-f32.py tests/fixtures/audio/clean_en_sample.wav $IN/speech20.f32 --copies 20 --gap-seconds 3
python3 -I scripts/make-f32.py tests/fixtures/audio/clean_en_sample.wav $IN/speech60.f32 --copies 5 --gap-seconds 1.23
python3 -I scripts/make-f32.py --zeros-seconds 60 $IN/zeros60.f32
```

The commands name `L=build/gate/release/lws-cli` (the CPU build), `C=build/measure/cuda/lws-cli` (the cuda
preset, built with `LWS_BUILD_ID=measure`), `M=models/ggml-base.en.bin` and `V=models/ggml-silero-v5.1.2.bin`.
The release build has no CUDA backend, so `L` decodes on the CPU. An unpaced run pushes the whole input at once,
so its lag figures measure the queue and are not a latency; only the paced rows (M1, M7) are lags.

The machine is an Intel Core i5-12500H (12th generation, 16 logical cores), 32073476 kB of memory and a GeForce
RTX 3050 Laptop GPU with 4096 MiB (driver 615.71.09, rows M7 and M9), running Ubuntu 26.04.1 with kernel
7.0.0-43-generic, clang 21.1.8 and CUDA 13.4.92. Models: `ggml-base.en.bin` (sha256 prefix `a03779c8`) and
`ggml-silero-v5.1.2.bin` (`29940d98`), pinned in `scripts/models.sha256`. All rows were taken on the same day with
the lws commit and fork commit shown.

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
4. [x] Decoder: one `whisper_full` call per closed segment, VAD off, 100 ms floor <!-- const -->, stop through the abort
   callback, CPU and CUDA builds.
5. [x] Streamer: pump and decoder threads, bounded segment queue, timed text events, Drain and Immediate stop,
   overrun and VAD-failure reports, a thread gate.
6. [x] Public streamer API (`lws::Streamer`, pimpl header, no thread type in `include/lws`).
7. [x] `lws-cli` that reads raw PCM from stdin (so `pwcap <app> | lws-cli` works), and the install check.

## License

Apache License 2.0. See [LICENSE](LICENSE).
