# shellcheck shell=bash
# tests/gates/lib.sh: the shared harness of the seeded-defect proofs (D-04). Sourced, never executed;
# callers run under `set -euo pipefail`. leg_matrix.sh and skip_red.sh source it, and 01-06's
# static_gates_red.sh sources it later, which is why gate_write and gate_patch_append exist here.
#
# Every proof inserts a real defect into a test-only file (never src/), runs under LWS_BUILD_ID=gate-seed
# so build/gate/ (check.sh's own tree) is never touched, asserts the gate's own token rather than any
# non-zero exit, and restores the tree byte for byte without git checkout.
#
# Skips are counted only through lws_count_skips from scripts/lib/skips.sh, the helper check.sh uses, so
# the matrix and check.sh cannot count differently.

LWS_GATE_ROOT=$(git rev-parse --show-toplevel)
cd "$LWS_GATE_ROOT" || exit 3
# shellcheck source=scripts/lib/skips.sh
source "$LWS_GATE_ROOT/scripts/lib/skips.sh"
export LWS_BUILD_ID=gate-seed

GATE_LOGS="$LWS_GATE_ROOT/build/gate-seed/logs"
GATE_TMP=""
GATE_SNAPSHOT=""
GATE_CREATED=()
GATE_PATCHED=()

# gate_begin: refuse to start over leftovers, make the log directories, snapshot the tree, install the trap.
gate_begin() {
    if [[ -e "$LWS_GATE_ROOT/CMakeUserPresets.json" ]]; then
        echo "GATE-HARNESS: CMakeUserPresets.json exists; a previous run died or a user preset would change the legs" >&2
        exit 3
    fi
    if compgen -G "$LWS_GATE_ROOT/tests/smoke/zz_seed_*" >/dev/null; then
        echo "GATE-HARNESS: tests/smoke/zz_seed_* exists; a previous run died. Remove the leftover by hand." >&2
        exit 3
    fi
    mkdir -p "$GATE_LOGS/gates" "$GATE_LOGS/matrix"
    GATE_TMP=$(mktemp -d)
    GATE_SNAPSHOT="$GATE_TMP/status.before"
    git status --porcelain=v1 --untracked-files=all >"$GATE_SNAPSHOT"
    trap gate_cleanup EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
}

# gate_insert {source} {dest}: dest must not exist; copy; remember dest for deletion.
gate_insert() {
    local src=$1 dest=$2
    if [[ -e $dest ]]; then
        echo "GATE-HARNESS: insert target exists: $dest" >&2
        exit 3
    fi
    GATE_CREATED+=("$dest")
    cp -- "$src" "$dest"
}

# gate_write {dest} {text}: write a new file from text; remember it for deletion.
gate_write() {
    local dest=$1 text=$2
    if [[ -e $dest ]]; then
        echo "GATE-HARNESS: write target exists: $dest" >&2
        exit 3
    fi
    GATE_CREATED+=("$dest")
    printf '%s\n' "$text" >"$dest"
}

# gate_patch_append {tracked-file} {line}: save the original and its sha256, append the line, remember it.
gate_patch_append() {
    local file=$1 line=$2 saved
    saved="$GATE_TMP/patched.${#GATE_PATCHED[@]}"
    cp -p -- "$file" "$saved"
    sha256sum -- "$file" | cut -d' ' -f1 >"$saved.sha256"
    GATE_PATCHED+=("$file")
    printf '%s\n' "$line" >>"$file"
}

# gate_restore_now: delete remembered files, copy saved originals back, check each sha256 equals the saved
# value. Returns 1 (and prints the path) when a file did not come back byte for byte.
gate_restore_now() {
    local rc=0 path i saved want got
    for path in "${GATE_CREATED[@]}"; do
        rm -f -- "$path"
    done
    GATE_CREATED=()
    for i in "${!GATE_PATCHED[@]}"; do
        path=${GATE_PATCHED[$i]}
        saved="$GATE_TMP/patched.$i"
        cp -p -- "$saved" "$path"
        want=$(cat "$saved.sha256")
        got=$(sha256sum -- "$path" | cut -d' ' -f1)
        if [[ $want != "$got" ]]; then
            echo "GATE-HARNESS: tree not restored: $path (sha256 differs)" >&2
            rc=1
        fi
    done
    GATE_PATCHED=()
    return "$rc"
}

# gate_cleanup: the trap. Restores, compares git status with the snapshot, and forces exit 3 on any
# difference. A clean run keeps the caller's own exit status.
gate_cleanup() {
    local rc=$?
    trap - EXIT INT TERM
    gate_restore_now || rc=3
    local after="$GATE_TMP/status.after" line
    git status --porcelain=v1 --untracked-files=all >"$after"
    if ! cmp -s "$GATE_SNAPSHOT" "$after"; then
        while IFS= read -r line; do
            echo "GATE-HARNESS: tree not restored: ${line:3}" >&2
        done < <(diff <(cat "$GATE_SNAPSHOT") <(cat "$after") | sed -n 's/^[<>] //p')
        rc=3
    fi
    rm -rf -- "$GATE_TMP"
    exit "$rc"
}

# gate_expect_red {name} {token} -- {cmd...}
# Passes only when the command exits non-zero AND the token is in its combined output. A command that
# exits 127 because a path is wrong has no token and fails the proof.
gate_expect_red() {
    local name=$1 token=$2 status=0 log
    shift 2
    [[ ${1:-} == -- ]] && shift
    log="$GATE_LOGS/gates/$name.log"
    "$@" >"$log" 2>&1 || status=$?
    if ((status == 0)); then
        echo "RED-FAIL $name: the command exited 0, a seeded defect must turn it red"
        return 1
    fi
    if ! grep -qF -- "$token" "$log"; then
        echo "RED-FAIL $name: exit $status without the gate's own token '$token' (see $log)"
        return 1
    fi
    echo "RED-OK $name"
}

# gate_expect_green {name} -- {cmd...}
gate_expect_green() {
    local name=$1 status=0 log
    shift
    [[ ${1:-} == -- ]] && shift
    log="$GATE_LOGS/gates/$name.log"
    "$@" >"$log" 2>&1 || status=$?
    if ((status != 0)); then
        echo "GREEN-FAIL $name: exit $status (see $log)"
        return 1
    fi
    echo "GREEN-OK $name"
}

# gate_leg_run {row} {leg}: one matrix cell. Installs the Conan dependencies for the leg's build type
# through scripts/deps.sh, configures, builds and runs ctest, each logged under
# build/gate-seed/logs/matrix/{row}.{leg}.{stage}.log. Prints exactly one word on stdout: green,
# red:configure, red:build, red:ctest or red:skip. Harness errors exit 3, so callers capture the cell with
# `cell=$(gate_leg_run row leg) || exit 3`.
gate_leg_run() {
    local row=$1 leg=$2 type=Debug status=0 cache have log_base skipped skip_status=0
    [[ $leg == release ]] && type=Release
    log_base="$GATE_LOGS/matrix/$row.$leg"

    scripts/deps.sh "$type" >"$log_base.deps.log" 2>&1 || status=$?
    if ((status != 0)); then
        echo "GATE-HARNESS: $row.$leg: deps failed (exit $status); see $log_base.deps.log" >&2
        exit 3
    fi

    status=0
    cmake --preset "$leg" >"$log_base.configure.log" 2>&1 || status=$?
    if ((status != 0)); then
        echo "red:configure"
        return 0
    fi
    cache="$LWS_GATE_ROOT/build/gate-seed/$leg/CMakeCache.txt"
    have=$(sed -n 's/^CMAKE_BUILD_TYPE:[A-Z]*=//p' "$cache")
    if [[ $have != "$type" ]]; then
        echo "GATE-HARNESS: $row.$leg: cache build type '$have' differs from the installed '$type'" >&2
        exit 3
    fi

    status=0
    cmake --build --preset "$leg" >"$log_base.build.log" 2>&1 || status=$?
    if ((status != 0)); then
        echo "red:build"
        return 0
    fi

    status=0
    log="$log_base.ctest.log"
    ctest --preset "$leg" >"$log" 2>&1 || status=$?
    skipped=$(lws_count_skips "$log") || skip_status=$?
    if ((skip_status != 0)); then
        echo "GATE-HARNESS: $row.$leg: ctest log unreadable" >&2
        exit 3
    fi
    if ((status != 0)); then
        echo "red:ctest"
    elif ((skipped > 0)); then
        echo "red:skip"
    else
        echo "green"
    fi
}
