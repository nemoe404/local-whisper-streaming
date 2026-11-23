#!/usr/bin/env bash
# Offline fail-closed proof for scripts/fetch-models.sh (BLD-05, Pitfall 10).
#
# No arguments. Never touches the real models/ directory and never uses the network: every call of the
# fetch script goes through run_fetch, which sets LWS_MODELS_DIR, LWS_MODELS_SHA256 and LWS_MODELS_SOURCE
# (file:// fixtures). The case functions below (case_c1 ... case_cN) are the only enumeration of cases:
# they run in numeric order from one loop over the functions this file declares.
#
# Self-proof: a copy of the script with the CONS ordering (move into place before verifying) must make
# C3 or C4 fail. A test that cannot fail proves nothing.
#
# Ends with: FETCH-TEST-OK cases={n} self-proof=red
set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
readonly repo
readonly SCRIPT=$repo/scripts/fetch-models.sh
readonly REAL_SUMS=$repo/scripts/models.sha256
readonly MODEL=silero-v5.1.2
readonly FILE=ggml-silero-v5.1.2.bin

fixture=$(mktemp -d)
readonly fixture
trap 'rm -rf -- "$fixture"' EXIT

# Independent second witness of the pinned hashes (STACK.md "Model files"). Editing only one copy turns C10 red.
declare -A WITNESS=(
    [ggml-silero-v5.1.2.bin]=29940d98d42b91fbd05ce489f3ecf7c72f0a42f027e4875919a28fb4c04ea2cf
    [ggml-tiny.en.bin]=921e4cf8686fdd993dcd081a5da5b6c365bfde1162e72b08d75ac75289920b1f
    [ggml-base.en.bin]=a03779c86df3323075f5e796cb2ce5029f00ec8869eee3fdfb897afe36c6d002
    [ggml-small.en.bin]=c6138d6d58ecc8322097e0f987c32f1be8bb0a18532a3f88f734d1bbf9c41e5d
    [ggml-large-v3-turbo-q8_0.bin]=317eb69c11673c9de1e1f0d459b253999804ec71ac4c23c17ecf5fbe24e259a1
)
readonly -A WITNESS

# State of the case being run.
FETCH=$SCRIPT
CASE=setup
DEST=
SRC=
SUMS=
OUT=
RC=0

fail() {
    printf 'FAIL %s: %s\n' "$CASE" "$*" >&2
    exit 1
}

# Run the fetch script with the three overrides set; the exit status lands in RC, all output in OUT.
run_fetch() {
    set +e
    LWS_MODELS_DIR=$DEST LWS_MODELS_SHA256=$SUMS LWS_MODELS_SOURCE=$SRC "$FETCH" "$@" >"$OUT" 2>&1
    RC=$?
    set -e
}

expect_rc() {
    [ "$RC" -eq "$1" ] || fail "expected exit $1, got $RC; output: $(cat "$OUT")"
}

expect_no_model() {
    local found
    found=$(find "$DEST" -maxdepth 1 -name 'ggml-*' -print -quit 2>/dev/null || true)
    [ -z "$found" ] || fail "a model file exists: $found"
}

expect_no_tmp() {
    local found
    found=$(find "$DEST" -maxdepth 1 -name '.fetch.*' -print -quit 2>/dev/null || true)
    [ -z "$found" ] || fail "a temp entry remains: $found"
}

expect_verified() {
    [ -f "$DEST/$FILE" ] || fail "$DEST/$FILE does not exist"
    (cd -- "$DEST" && sha256sum -c --strict --quiet -- "$SUMS") >/dev/null 2>&1 \
        || fail "$DEST/$FILE does not verify against the pin"
}

new_case() {
    CASE=$1
    DEST=$fixture/dest-$1
    OUT=$fixture/out-$1.txt
    SRC=file://$fixture/src-good
    SUMS=$fixture/good.sha256
}

make_fixture() {
    local orig repl
    mkdir -p "$fixture/src-good" "$fixture/src-altered" "$fixture/src-truncated"
    head -c 4096 /dev/urandom >"$fixture/src-good/$FILE"
    (cd -- "$fixture/src-good" && sha256sum -- "$FILE") >"$fixture/good.sha256"

    cp -- "$fixture/src-good/$FILE" "$fixture/src-altered/$FILE"
    orig=$(od -An -tx1 -j100 -N1 -- "$fixture/src-good/$FILE")
    repl='\001'
    if [ "${orig// /}" != "00" ]; then
        repl='\000'
    fi
    printf '%b' "$repl" | dd of="$fixture/src-altered/$FILE" bs=1 seek=100 conv=notrunc status=none
    if cmp -s "$fixture/src-good/$FILE" "$fixture/src-altered/$FILE"; then
        echo "fixture: the altered copy equals the good one" >&2
        exit 1
    fi

    head -c 2048 "$fixture/src-good/$FILE" >"$fixture/src-truncated/$FILE"
}

case_c1() {
    new_case C1
    run_fetch "$MODEL"
    expect_rc 0
    expect_verified
    [ "$(cat "$DEST/.gitignore")" = "*" ] || fail "destination .gitignore does not hold '*'"
    grep -qx 'FETCH-OK models=1' "$OUT" || fail "no FETCH-OK line"
    expect_no_tmp
}

case_c2() {
    new_case C2
    local before after
    run_fetch "$MODEL"
    expect_rc 0
    before=$(stat -c '%y' "$DEST/$FILE")
    run_fetch "$MODEL"
    expect_rc 0
    grep -q 'present and verified' "$OUT" || fail "second run did not verify in place"
    after=$(stat -c '%y' "$DEST/$FILE")
    [ "$before" = "$after" ] || fail "destination mtime changed: $before -> $after"
    expect_verified
    expect_no_tmp
}

case_c3() {
    new_case C3
    SRC=file://$fixture/src-altered
    run_fetch "$MODEL"
    expect_rc 2
    expect_no_model
    expect_no_tmp
}

case_c4() {
    new_case C4
    SRC=file://$fixture/src-truncated
    run_fetch "$MODEL"
    expect_rc 2
    expect_no_model
    expect_no_tmp
}

case_c5() {
    new_case C5
    SRC=file://$fixture/missing
    run_fetch "$MODEL"
    expect_rc 1
    expect_no_model
    expect_no_tmp
}

case_c6() {
    new_case C6
    run_fetch "$MODEL"
    expect_rc 0
    printf 'x' >>"$DEST/$FILE"
    run_fetch "$MODEL"
    expect_rc 0
    expect_verified
    expect_no_tmp
}

case_c7() {
    new_case C7
    run_fetch "$MODEL"
    expect_rc 0
    printf 'x' >>"$DEST/$FILE"
    SRC=file://$fixture/src-altered
    run_fetch "$MODEL"
    expect_rc 2
    expect_no_model
    expect_no_tmp
}

case_c8() {
    new_case C8
    local entries
    SUMS=$fixture/other.sha256
    printf '%s  other.bin\n' "${WITNESS[$FILE]}" >"$SUMS"
    run_fetch "$MODEL"
    expect_rc 3
    expect_no_model
    expect_no_tmp
    entries=$(find "$DEST" -mindepth 1 -maxdepth 1 ! -name '.gitignore' -print -quit)
    [ -z "$entries" ] || fail "unexpected entry in the destination: $entries"
}

case_c9() {
    new_case C9
    run_fetch no-such-model
    expect_rc 3
    expect_no_model
    expect_no_tmp
    grep -q 'known models' "$OUT" || fail "the error does not list the known models"
}

case_c10() {
    new_case C10
    local urls url line_count url_count file hash want
    local -a url_list
    urls=$(awk '!/^[[:space:]]*#/ { s = $0; while (match(s, /https:\/\/huggingface\.co\/[^"[:space:]]+/)) { print substr(s, RSTART, RLENGTH); s = substr(s, RSTART + RLENGTH) } }' "$SCRIPT")
    mapfile -t url_list <<<"$urls"
    url_count=${#url_list[@]}
    [ -n "$urls" ] || fail "the script holds no Hugging Face URL"
    for url in "${url_list[@]}"; do
        [[ $url =~ ^https://huggingface\.co/[^/]+/[^/]+/resolve/[0-9a-f]{40}/(ggml-[^/]+\.bin)$ ]] \
            || fail "URL is not pinned to a 40-hex revision: $url"
        file=${BASH_REMATCH[1]}
        [ "$(awk -v f="$file" '$2 == f { n++ } END { print n + 0 }' "$REAL_SUMS")" -eq 1 ] \
            || fail "$file has no single pin in scripts/models.sha256"
    done
    line_count=$(awk 'NF == 2 && $1 ~ /^[0-9a-f]{64}$/ { k++ } END { print k + 0 }' "$REAL_SUMS")
    [ "$line_count" -eq "$url_count" ] || fail "$url_count URLs but $line_count pins"
    [ "$line_count" -eq "${#WITNESS[@]}" ] || fail "$line_count pins but the test witnesses ${#WITNESS[@]}"
    [ "$(wc -l <"$REAL_SUMS")" -eq "$line_count" ] || fail "scripts/models.sha256 holds a line that is not a pin"
    for file in "${!WITNESS[@]}"; do
        want=${WITNESS[$file]}
        hash=$(awk -v f="$file" '$2 == f { print $1 }' "$REAL_SUMS")
        [ "$hash" = "$want" ] || fail "$file pin differs from the witness: $hash vs $want"
    done
}

# Run one case in a subshell with errexit really active (set -e is ignored under `if` and `||`).
run_case_status() {
    local rc
    set +e
    (
        set -e
        "$1"
    )
    rc=$?
    set -e
    return "$rc"
}

mapfile -t cases < <(compgen -A function case_c | sort -V)
[ "${#cases[@]}" -ge 10 ] || {
    echo "FAIL: only ${#cases[@]} case functions declared, expected at least 10" >&2
    exit 1
}

make_fixture

for c in "${cases[@]}"; do
    status=0
    run_case_status "$c" || status=$?
    id=${c#case_c}
    [ "$status" -eq 0 ] || {
        echo "FAIL C$id (status $status)" >&2
        exit 1
    }
    echo "PASS C$id"
done

# Self-proof: the CONS ordering (move into place, then verify) on a copy only, never on the tracked file.
broken_dir=$fixture/broken/scripts
mkdir -p "$broken_dir"
cp -- "$SCRIPT" "$broken_dir/fetch-models.sh"
# The pattern is literal text for sed, so the single quotes are intended.
# shellcheck disable=SC2016
sed -i 's|^    cp -- "\$pinfile" "\$tmpdir/pin.sha256"$|    mv -f -- "$tmpdir/$file" "$dest"\n&|' "$broken_dir/fetch-models.sh"
if cmp -s "$SCRIPT" "$broken_dir/fetch-models.sh"; then
    echo "SELF-PROOF FAIL"
    echo "the sed edit did not change the copy; the self-proof cannot run" >&2
    exit 1
fi
chmod +x "$broken_dir/fetch-models.sh"

FETCH=$broken_dir/fetch-models.sh
red=()
for c in case_c3 case_c4; do
    status=0
    run_case_status "$c" >/dev/null 2>&1 || status=$?
    if [ "$status" -ne 0 ]; then
        red+=("C${c#case_c}")
    fi
done
FETCH=$SCRIPT
if [ "${#red[@]}" -eq 0 ]; then
    echo "SELF-PROOF FAIL"
    exit 1
fi
echo "SELF-PROOF red: ${red[*]}"

echo "FETCH-TEST-OK cases=${#cases[@]} self-proof=red"
