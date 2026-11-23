#!/usr/bin/env bash
# The gate entry point. Stages in order: format, toolchain (presets), tuning values, core isolation, ring wait-free, vad api, models, deps, then one leg per preset
# (configure, toolchain, isa, sanitizer, build, ctest, skip count), then cppcheck and clang-tidy over the owned
# translation units. Takes no arguments: a leg name would silently
# narrow the sweep, so any argument is refused. Every exit status is read directly; no gate decision
# reads a pipe.
set -euo pipefail

if (($# != 0)); then
    echo "check.sh takes no arguments: run it bare so every stage runs" >&2
    exit 2
fi

root=$(git rev-parse --show-toplevel)
cd "$root"
# shellcheck source=scripts/lib/skips.sh
source "$root/scripts/lib/skips.sh"

id=${LWS_BUILD_ID:-gate}
export LWS_BUILD_ID="$id"
logs="build/$id/logs"
# Before the first stage: format runs first and must not depend on deps.sh having made this.
mkdir -p "$logs"

stage() { printf '\n== %s\n' "$1"; }

stage "clang-format"
status=0
scripts/format.sh || status=$?
if ((status != 0)); then
    echo "check: format failed (exit $status)" >&2
    exit "$status"
fi

# Static half of the toolchain gate: every declared preset resolves to clang 21. It needs no
# configure, so it runs before deps and before any leg.
stage "toolchain (presets)"
status=0
cmake -DLWS_ROOT="$root" -P "$root/scripts/toolchain-gate.cmake" >"$logs/toolchain-presets.log" 2>&1 || status=$?
if ((status != 0)); then
    echo "check: toolchain gate (presets) failed (exit $status); see $logs/toolchain-presets.log" >&2
    exit 1
fi

# Tuning values stay out of constexpr under include/lws. A text scan of the public headers: no
# configure needed, so it runs before models and deps.
stage "tuning values"
status=0
cmake -DLWS_ROOT="$root" -P "$root/scripts/tuning-gate.cmake" >"$logs/tuning.log" 2>&1 || status=$?
if ((status != 0)); then
    echo "check: tuning-values gate failed (exit $status); see $logs/tuning.log" >&2
    exit 1
fi

# The pure core stays free of whisper, spdlog and thread includes. A text scan: no configure needed.
# The link closure of lws_core is checked by the core_link_closure ctest inside the release leg.
stage "core isolation"
status=0
cmake -DLWS_ROOT="$root" -P "$root/scripts/core-isolation-gate.cmake" >"$logs/isolation.log" 2>&1 || status=$?
if ((status != 0)); then
    echo "check: core isolation gate failed (exit $status); see $logs/isolation.log" >&2
    exit 1
fi

# The ring stays wait-free by construction: no compare-and-swap, plain slots, no lock. A text
# scan of the ring header: no configure needed.
stage "ring wait-free"
status=0
cmake -DLWS_ROOT="$root" -P "$root/scripts/ring-gate.cmake" >"$logs/ring.log" 2>&1 || status=$?
if ((status != 0)); then
    echo "check: ring wait-free gate failed (exit $status); see $logs/ring.log" >&2
    exit 1
fi

# Silero is the only detector and runs through whisper's no-reset call: no reset variant, no whisper
# segmentation, no signal-level substitute anywhere under src/ or include/. A text scan: no configure
# needed.
stage "vad api"
status=0
cmake -DLWS_ROOT="$root" -P "$root/scripts/vad-gate.cmake" >"$logs/vad.log" 2>&1 || status=$?
if ((status != 0)); then
    echo "check: vad api gate failed (exit $status); see $logs/vad.log" >&2
    exit 1
fi

# The Silero smoke test loads models/ggml-silero-v5.1.2.bin. fetch-models.sh is idempotent and re-verifies
# an existing file against scripts/models.sha256, so the legs only ever load verified bytes.
stage "models"
status=0
scripts/fetch-models.sh silero-v5.1.2 >"$logs/models.log" 2>&1 || status=$?
if ((status != 0)); then
    echo "check: model fetch failed (exit $status); see $logs/models.log" >&2
    exit 1
fi

stage "dependencies (Debug)"
status=0
scripts/deps.sh Debug || status=$?
if ((status != 0)); then
    echo "check: deps failed (exit $status); see $logs/conan-Debug.log" >&2
    exit "$status"
fi

# Measured legs: measurements/ctest-legs.md (debug has no defect only it catches). Measured tsan scope:
# Outcome A (measurements/sanitizer-scope.md). tsan installs Debug deps like asan-ubsan.
legs=(asan-ubsan tsan release)
for leg in "${legs[@]}"; do
    if [[ $leg == release ]]; then
        # The Conan output folder is shared by every preset of this id, so it is reinstalled for the
        # build type of the leg that follows (Release here).
        stage "dependencies (Release)"
        status=0
        scripts/deps.sh Release || status=$?
        if ((status != 0)); then
            echo "check: deps failed (exit $status); see $logs/conan-Release.log" >&2
            exit "$status"
        fi
    fi
    stage "leg $leg"

    status=0
    cmake --preset "$leg" >"$logs/$leg.configure.log" 2>&1 || status=$?
    if ((status != 0)); then
        echo "check: configure failed for $leg (exit $status); see $logs/$leg.configure.log" >&2
        exit 1
    fi

    # Configured half of the toolchain gate: the compiler identity this leg recorded and the compiler of
    # every compile-database entry must be clang 21.
    status=0
    cmake -DLWS_ROOT="$root" -DLWS_BUILD_ID="$id" -DLWS_LEGS="$leg" -P "$root/scripts/toolchain-gate.cmake" \
        >"$logs/$leg.toolchain.log" 2>&1 || status=$?
    if ((status != 0)); then
        echo "check: toolchain gate failed for $leg (exit $status); see $logs/$leg.toolchain.log" >&2
        exit 1
    fi

    # Native-ISA sweep of this leg's compile database and cache.
    status=0
    cmake -DLWS_ROOT="$root" -DLWS_BUILD_ID="$id" -DLWS_LEGS="$leg" -P "$root/scripts/isa-gate.cmake" \
        >"$logs/$leg.isa.log" 2>&1 || status=$?
    if ((status != 0)); then
        echo "check: ISA gate failed for $leg (exit $status); see $logs/$leg.isa.log" >&2
        exit 1
    fi

    # Sanitizer gate: an asan-ubsan or tsan leg must carry its -fsanitize option on every compile entry,
    # ggml included, with scope all; a leg with no sanitizer must carry none.
    status=0
    cmake -DLWS_ROOT="$root" -DLWS_BUILD_ID="$id" -DLWS_LEGS="$leg" -P "$root/scripts/sanitizer-gate.cmake" \
        >"$logs/$leg.sanitizer.log" 2>&1 || status=$?
    if ((status != 0)); then
        echo "check: sanitizer gate failed for $leg (exit $status); see $logs/$leg.sanitizer.log" >&2
        exit 1
    fi

    status=0
    cmake --build --preset "$leg" >"$logs/$leg.build.log" 2>&1 || status=$?
    if ((status != 0)); then
        echo "check: build failed for $leg (exit $status); see $logs/$leg.build.log" >&2
        exit 1
    fi

    status=0
    ctest --preset "$leg" --output-on-failure >"$logs/$leg.ctest.log" 2>&1 || status=$?

    skip_status=0
    skipped=$(lws_count_skips "$logs/$leg.ctest.log") || skip_status=$?
    if ((skip_status != 0)); then
        echo "check: leg $leg: a skip is never a pass: log unreadable (see $logs/$leg.ctest.log)" >&2
        exit 1
    fi

    summary=$(grep -E 'tests (passed|failed)' "$logs/$leg.ctest.log" || true)
    echo "leg $leg: ${summary%%$'\n'*} skipped=$skipped"

    if ((status != 0)); then
        echo "check: tests failed in leg $leg (exit $status); see $logs/$leg.ctest.log" >&2
        exit 1
    fi
    if ((skipped > 0)); then
        echo "check: leg $leg: $skipped test(s) skipped; a skip is never a pass (see $logs/$leg.ctest.log)" >&2
        exit 1
    fi
done

stage "cppcheck"
status=0
scripts/cppcheck.sh || status=$?
if ((status != 0)); then
    echo "check: cppcheck failed (exit $status); see $logs/cppcheck.log" >&2
    exit 1
fi

stage "clang-tidy"
status=0
scripts/tidy.sh --all || status=$?
if ((status != 0)); then
    echo "check: clang-tidy failed (exit $status); see $logs/tidy-*.log" >&2
    exit 1
fi

legs_joined=$(IFS=,; printf '%s' "${legs[*]}")
printf 'CHECK-OK stages=format,toolchain-presets,tuning-values,core-isolation,ring-wait-free,vad-api,models,legs(%s),cppcheck,clang-tidy leg-stages=configure,toolchain,isa,sanitizer,build,ctest\n' "$legs_joined"
