#!/usr/bin/env bash
# The install check: installs the release build to a temporary prefix and checks what a user of the
# package would get.
#
# Usage: scripts/install-check.sh        (no arguments; LWS_BUILD_ID picks the tree, default gate)
#
# What it proves, in plain words:
#   - the installed package is complete and closed: exactly the expected files, no test or internal
#     file, no header outside include/lws, and no text file that holds this checkout's path;
#   - a consumer project copied out of the repository, configured with only the install prefix and
#     the Conan toolchain that provides fmt, finds the package, links the static archives and decodes
#     the fixture, and its compile commands never reach this source tree;
#   - the installed lws-cli reads the same fixture on stdin and prints the transcript.
#
# The scan for a leaked path reads text files only. ggml and whisper embed their source file names in
# the archives (abort messages), so a scan of the binary files would always match.
#
# Exit codes: 0 the package holds (prints INSTALL-OK files=<n>); 1 refused (prints INSTALL-REFUSED
# <count>, after one `[install] <reason>` line per refusal); 2 an input is missing
# (`[install] missing input: <reason>`).
#
# The one test seam is LWS_INSTALL_CHECK_DROP=<path under the prefix>: the named installed file is
# deleted before the consumer is configured. Test only; the file-set comparison is skipped when it is
# set, so the red comes from the consumer.
set -euo pipefail

if (($# != 0)); then
    echo "install-check: takes no arguments" >&2
    exit 2
fi

root=$(git rev-parse --show-toplevel)
cd "$root"
id=${LWS_BUILD_ID:-gate}
tree=build/$id/release
toolchain=$root/build/$id/conan/conan_toolchain.cmake
whisper_model=models/ggml-base.en.bin
vad_model=models/ggml-silero-v5.1.2.bin
fixture=tests/fixtures/audio/clean_en_sample.wav
phrase='keenly alive'

missing() {
    echo "[install] missing input: $1" >&2
    exit 2
}

[[ -f $tree/CMakeCache.txt ]] || missing "$tree/CMakeCache.txt does not exist (configure the release preset first)"
sanitizer=$(sed -n 's/^LWS_SANITIZER:[A-Z]*=//p' "$tree/CMakeCache.txt")
[[ $sanitizer == off ]] || missing "$tree is built with LWS_SANITIZER='$sanitizer'; only a plain release build is installed"
[[ -f $toolchain ]] || missing "$toolchain does not exist (run scripts/deps.sh Release)"
[[ -f $whisper_model ]] || missing "$whisper_model does not exist (run scripts/fetch-models.sh)"
[[ -f $vad_model ]] || missing "$vad_model does not exist (run scripts/fetch-models.sh)"
[[ -f $fixture ]] || missing "$fixture does not exist"

tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
prefix=$tmp/prefix
refusals=0

# refuse {message}: records one refusal and prints its reason.
refuse() {
    echo "[install] $1"
    refusals=$((refusals + 1))
}

# finish_refused: prints the verdict line for a refused run and exits 1.
finish_refused() {
    echo "INSTALL-REFUSED $refusals"
    exit 1
}

# tail_of {log}: the last lines of a log, so the failing symbol or file is in the output.
tail_of() {
    tail -n 30 -- "$1"
}

if ! cmake --install "$tree" --prefix "$prefix" >"$tmp/install.log" 2>&1; then
    refuse "install failed"
    tail_of "$tmp/install.log"
    finish_refused
fi

drop=${LWS_INSTALL_CHECK_DROP:-}
if [[ -n $drop ]]; then
    case "$drop" in
        *..* | /*)
            echo "install-check: LWS_INSTALL_CHECK_DROP must be a path under the prefix" >&2
            exit 2
            ;;
    esac
    rm -f -- "${prefix:?}/$drop"
fi

# The file set: exactly this list, nothing more.
expected=$(
    LC_ALL=C sort <<'EOF'
bin/lws-cli
include/lws/decode_policy.hpp
include/lws/options.hpp
include/lws/ring_index.hpp
include/lws/seam_merge.hpp
include/lws/segment.hpp
include/lws/spsc_ring.hpp
include/lws/streamer.hpp
include/lws/streamer_options.hpp
include/lws/text_event.hpp
include/lws/vad_windower.hpp
lib/cmake/lws/lws-config-version.cmake
lib/cmake/lws/lws-config.cmake
lib/cmake/lws/lws-targets-release.cmake
lib/cmake/lws/lws-targets.cmake
lib/libggml-base.a
lib/libggml-cpu.a
lib/libggml.a
lib/liblws_core.a
lib/liblws_shell.a
lib/libwhisper.a
share/doc/lws/LICENSE
EOF
)
actual=$(cd "$prefix" && find . -type f | sed 's|^\./||' | LC_ALL=C sort)
file_count=$(printf '%s\n' "$actual" | grep -c . || true)

if [[ -z $drop ]]; then
    while IFS= read -r path; do
        refuse "missing file: $path"
    done < <(comm -23 <(printf '%s\n' "$expected") <(printf '%s\n' "$actual"))
    while IFS= read -r path; do
        refuse "unexpected file: $path"
    done < <(comm -13 <(printf '%s\n' "$expected") <(printf '%s\n' "$actual"))
fi

# Nothing from the test or internal tree may ship, and no header outside include/lws.
while IFS= read -r path; do
    case "$path" in
        *tests* | *smoke* | */shell/* | */core/* | *.cpp)
            refuse "internal file installed: $path"
            ;;
        *.hpp | *.h)
            case "$path" in
                include/lws/*) ;;
                *) refuse "header outside include/lws: $path" ;;
            esac
            ;;
    esac
done <<<"$actual"

# The leaked-path scan: no text file in the prefix holds this checkout's path. grep exits 1 when
# nothing matches; any status above 1 is a harness error.
scan_status=0
leaks=$(grep -rIlF -- "$root" "$prefix" 2>"$tmp/scan.err") || scan_status=$?
case "$scan_status" in
    0)
        while IFS= read -r path; do
            refuse "source path in ${path#"$prefix"/}"
        done <<<"$leaks"
        ;;
    1) ;;
    *)
        echo "install-check: the path scan failed (grep exit $scan_status)" >&2
        cat "$tmp/scan.err" >&2
        exit 2
        ;;
esac

if ((refusals > 0)); then
    finish_refused
fi

# Inputs: the fixture is data, copied as floats. The consumer includes and links nothing from the tree.
python3 -I scripts/make-f32.py "$fixture" "$tmp/clean.f32"

# The consumer: a copy outside the repository, configured with the prefix and the Conan toolchain only.
cp -r tests/install "$tmp/consumer"
if ! cmake -S "$tmp/consumer" -B "$tmp/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=clang++-21 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_PREFIX_PATH="$prefix" >"$tmp/consumer-configure.log" 2>&1; then
    refuse "consumer configure failed"
    tail_of "$tmp/consumer-configure.log"
    finish_refused
fi
if ! cmake --build "$tmp/build" >"$tmp/consumer-build.log" 2>&1; then
    refuse "consumer build failed"
    tail_of "$tmp/consumer-build.log"
    finish_refused
fi
for tree_dir in src include tests; do
    if grep -qF -- "$root/$tree_dir" "$tmp/build/compile_commands.json"; then
        refuse "consumer compile line reaches the source tree ($tree_dir)"
    fi
done
if ((refusals > 0)); then
    finish_refused
fi

consumer_status=0
"$tmp/build/consumer" "$whisper_model" "$vad_model" "$tmp/clean.f32" >"$tmp/consumer.out" 2>"$tmp/consumer.err" || consumer_status=$?
if ((consumer_status != 0)) || ! tr '[:upper:]' '[:lower:]' <"$tmp/consumer.out" | grep -qF -- "$phrase"; then
    refuse "consumer output lacks the transcript"
    tail_of "$tmp/consumer.err"
    finish_refused
fi

# The shipped tool, run from the prefix.
cli_status=0
"$prefix/bin/lws-cli" --model "$whisper_model" --vad-model "$vad_model" <"$tmp/clean.f32" \
    >"$tmp/cli.out" 2>"$tmp/cli.err" || cli_status=$?
if ((cli_status != 0)) || ! tr '[:upper:]' '[:lower:]' <"$tmp/cli.out" | grep -qF -- "$phrase"; then
    refuse "installed lws-cli failed"
    tail_of "$tmp/cli.err"
    finish_refused
fi

echo "INSTALL-OK files=$file_count"
