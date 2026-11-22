#!/usr/bin/env bash
# Fetch model weights into models/ without ever leaving an unverified file there.
#
# Usage:   scripts/fetch-models.sh [model...]     (no argument: every model, in table order)
#
# Every model comes from a Hugging Face URL pinned to a 40-hex commit revision (never a branch).
# The bytes go into a private mktemp directory inside models/ (same filesystem as the destination),
# are verified there with `sha256sum -c --strict` against scripts/models.sha256, and only then are
# renamed into place. A bad download therefore never reaches models/{file}. An existing model file
# is re-verified on every run and never trusted by name; a mismatching one is deleted and fetched again.
#
# Exit codes:
#   0  every requested model is present and verified
#   1  EXIT_DOWNLOAD  a download failed (source unreachable, HTTP error)
#   2  EXIT_CHECKSUM  a download did not match its pin (altered or truncated)
#   3  EXIT_USAGE     unknown model name, missing or malformed pin, missing tool
#
# Environment overrides:
#   LWS_MODELS_DIR     destination directory (default: <repo>/models)
#   LWS_MODELS_SHA256  checksum file in sha256sum format (default: <repo>/scripts/models.sha256)
#   LWS_MODELS_SOURCE  test only: take {file} from "$LWS_MODELS_SOURCE/{file}" instead of the pinned URL
set -euo pipefail

readonly EXIT_DOWNLOAD=1
readonly EXIT_CHECKSUM=2
readonly EXIT_USAGE=3

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
readonly root
readonly MODELS_DIR=${LWS_MODELS_DIR:-$root/models}
readonly SUMS_FILE=${LWS_MODELS_SHA256:-$root/scripts/models.sha256}
readonly SOURCE_OVERRIDE=${LWS_MODELS_SOURCE:-}

# Model name -> revision-pinned URL. One entry per line, in the order MODEL_ORDER lists them.
# The file name is ggml-{name}.bin and its hash lives in scripts/models.sha256.
readonly -a MODEL_ORDER=(silero-v5.1.2 tiny.en base.en small.en large-v3-turbo-q8_0)
declare -A MODEL_URL=(
    [silero-v5.1.2]="https://huggingface.co/ggml-org/whisper-vad/resolve/9ffd54a1e1ee413ddf265af9913beaf518d1639b/ggml-silero-v5.1.2.bin"
)
readonly -A MODEL_URL

declare -a TMP_DIRS=()

log() { printf 'lws: fetch-models: %s\n' "$*"; }
err() { printf 'lws: fetch-models: %s\n' "$*" >&2; }

cleanup() {
    if [ "${#TMP_DIRS[@]}" -gt 0 ]; then
        rm -rf -- "${TMP_DIRS[@]}"
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

model_file() { printf 'ggml-%s.bin' "$1"; }

# Print the sha256 of $1, or "unreadable" when sha256sum cannot read it.
actual_sha256() {
    local sum
    sum=$(sha256sum -- "$1" 2>/dev/null) || {
        printf 'unreadable'
        return 0
    }
    printf '%s' "${sum%% *}"
}

# Write the single pin line for file $1 into $2, or exit EXIT_USAGE when the pin is missing,
# duplicated or malformed. Nothing is downloaded without a pin.
write_pin() {
    local file=$1 pinfile=$2 count line
    count=$(awk -v f="$file" '$2 == f { n++ } END { print n + 0 }' "$SUMS_FILE")
    if [ "$count" -ne 1 ]; then
        err "$SUMS_FILE holds $count pin lines for $file, expected exactly 1"
        exit "$EXIT_USAGE"
    fi
    line=$(awk -v f="$file" '$2 == f' "$SUMS_FILE")
    if ! [[ $line =~ ^[0-9a-f]{64}\ \ [^[:space:]]+$ ]]; then
        err "malformed pin for $file in $SUMS_FILE (want: 64 lowercase hex, two spaces, file name)"
        exit "$EXIT_USAGE"
    fi
    printf '%s\n' "$line" >"$pinfile"
}

fetch_one() {
    local name=$1 pindir=$2
    local file dest pinfile url tmpdir
    file=$(model_file "$name")
    dest=$MODELS_DIR/$file
    pinfile=$pindir/$file.sha256

    if [ -e "$dest" ] || [ -L "$dest" ]; then
        if (cd -- "$MODELS_DIR" && sha256sum -c --strict --quiet -- "$pinfile") >/dev/null 2>&1; then
            log "$file present and verified"
            return 0
        fi
        err "existing $file does not match its pin: expected $(cut -d' ' -f1 "$pinfile"), got $(actual_sha256 "$dest"); deleting it"
        rm -f -- "$dest"
    fi

    # Private directory on the same filesystem as the destination: the final mv is an atomic rename,
    # and the unpredictable name rules out a symlink race on a guessable temp path.
    tmpdir=$(mktemp -d "$MODELS_DIR/.fetch.XXXXXX")
    TMP_DIRS+=("$tmpdir")

    if [ -n "$SOURCE_OVERRIDE" ]; then
        url=$SOURCE_OVERRIDE/$file
    else
        url=${MODEL_URL[$name]}
    fi
    log "downloading $file from $url"
    if ! curl -fsSL --retry 3 --proto '=https,file' --proto-redir '=https' -o "$tmpdir/$file" -- "$url"; then
        err "download failed for $url"
        exit "$EXIT_DOWNLOAD"
    fi

    cp -- "$pinfile" "$tmpdir/pin.sha256"
    if ! (cd -- "$tmpdir" && sha256sum -c --strict --quiet -- pin.sha256) >/dev/null 2>&1; then
        err "SHA256 mismatch for $file from $url"
        err "expected $(cut -d' ' -f1 "$pinfile"), got $(actual_sha256 "$tmpdir/$file"); the file was not moved into $MODELS_DIR"
        exit "$EXIT_CHECKSUM"
    fi

    mv -f -- "$tmpdir/$file" "$dest"
    rm -rf -- "$tmpdir"
    log "$file fetched and verified (sha256 matches scripts/models.sha256)"
}

main() {
    local name known
    local -a selected=()

    for tool in curl sha256sum mktemp awk; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            err "$tool is not available; nothing can be proven without it"
            exit "$EXIT_USAGE"
        fi
    done

    if [ "$#" -eq 0 ]; then
        selected=("${MODEL_ORDER[@]}")
    else
        for name in "$@"; do
            if [ -z "$name" ] || [ -z "${MODEL_URL[$name]:-}" ]; then
                known=${MODEL_ORDER[*]}
                err "unknown model '$name'; known models: $known"
                exit "$EXIT_USAGE"
            fi
            selected+=("$name")
        done
    fi

    if [ ! -r "$SUMS_FILE" ]; then
        err "checksum file $SUMS_FILE is not readable"
        exit "$EXIT_USAGE"
    fi
    if [ -n "$SOURCE_OVERRIDE" ]; then
        err "TEST SOURCE OVERRIDE ACTIVE: models come from $SOURCE_OVERRIDE, not from their pinned URLs"
    fi

    mkdir -p -- "$MODELS_DIR"
    # Second ignore layer: the directory ignores itself and its contents in any clone.
    if [ ! -e "$MODELS_DIR/.gitignore" ]; then
        printf '*\n' >"$MODELS_DIR/.gitignore"
    fi

    # Every pin is checked before the first download.
    local pindir
    pindir=$(mktemp -d "$MODELS_DIR/.fetch.XXXXXX")
    TMP_DIRS+=("$pindir")
    for name in "${selected[@]}"; do
        write_pin "$(model_file "$name")" "$pindir/$(model_file "$name").sha256"
    done

    for name in "${selected[@]}"; do
        fetch_one "$name" "$pindir"
    done
    echo "FETCH-OK models=${#selected[@]}"
}

main "$@"
