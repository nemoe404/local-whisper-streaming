# Changelog

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.0] - 2026-11-25

Built and measured on Ubuntu 26.04.1 with clang 21.1.8.

### Added

- `lws::Streamer`: audio pushed from any thread goes through a lock-free ring and a Silero voice-activity
  detector, and only the speech reaches Whisper. Text, overrun, VAD-failure, dropped-segment and decode-failure
  events arrive in stream order with exact sample ranges. `StopMode::Drain` decodes what was accepted;
  `StopMode::Immediate` interrupts a decode in flight.
- `lws-cli`: reads raw 16 kHz mono f32le from stdin and prints one line per text event with its stream time.
  It drains on end of input and on a signal, and reports losses on stderr. `--stats` prints one line of
  measurements; `--queue`, `--ring-seconds`, `--speed`, `--threads`, `--language` and `--cpu` tune a run.
- The installable CMake package: `find_package(lws 0.1 CONFIG REQUIRED)` and the target `lws::lws`, with the
  whisper and ggml archives included. An out-of-tree consumer and `lws-cli` run from the install prefix in the
  gate.
- The gate, by stage name: `clang-format`, `toolchain (presets)`, `tuning values`, `core isolation`,
  `ring wait-free`, `vad api`, `threads`, `public api`, `readme`, `models`, one leg per preset
  (`asan-ubsan`, `tsan`, `release`), `install`, `cppcheck` and `clang-tidy`. `readme` refuses a README number
  that is not a traced row of the Measured table or a marked constant.

### Measured

The README's Measured table, rows M1 to M9, holds every number: the machine, the models, the commits, the command
and the date of each. All rows were measured at commit `a65b834` on the machine the table names. The numbers are
not repeated here, so the two cannot drift.

### Decisions

- `lws-cli` ships in 0.1.0. The public headers under `include/lws` are byte-identical to commit 2cb3149, so the
  command line is a plain consumer of the library and adds nothing to its interface. Checked with
  `git diff --exit-code 2cb3149 894cfb04d834afa100c5544f8712b590d139cd94 -- include/lws`: exit 0, no output,
  2026-11-25. The `public api` stage of `scripts/check.sh` keeps it so at every later commit, including the
  tagged one.
- No spdlog: the installed library links fmt and the whisper and ggml archives only.
- CUDA is built and tested by hand and is not packaged.

### Known limits

- `push` has no backpressure, so a replay that is read faster than real time can overrun the ring. Losses are
  reported on stderr; use `--speed` or `--ring-seconds`.
- The segment queue drops the oldest segment when the decoder falls behind. `--queue` sizes it.
- pwcap defaults to s16le and has to be given `--format f32le` to feed `lws-cli`.
- There is no hosted CI. The gate is `./scripts/check.sh`, run bare on the machine that builds the release, and
  CUDA is built and tested by hand.
- A consumer that also links its own whisper gets duplicate symbols from `lws::whisper`.
- Only the release preset is packaged.
