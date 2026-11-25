#!/usr/bin/env bash
# tests/gates/release_gates_red.sh: the gates a release depends on, shown red on seeded defects and green on
# the same command without them. No arguments.
#
# Every proof is a function named proof_{name}, the name's dashes written as underscores, defined at column 0.
# The runner derives the list of proofs from those definitions, in file order, so the functions are the one
# enumeration. A proof runs a green control (the gate's command on the clean tree), the seeded red (the same
# command shape with one defect inserted into a tracked file outside src/ or a temporary file) and a
# byte-for-byte restore. The red check asserts the gate's own token and not merely a non-zero exit, so a
# command that dies for another reason cannot pass as red.
#
# Everything runs under LWS_BUILD_ID=gate-seed (set by lib.sh), so build/gate/, check.sh's tree, is never
# touched. Exit 0 prints RELEASE-GATES-PROVEN n={count}; exit 1 prints RELEASE-GATES-FAILED with the names;
# exit 3 is a harness error (leftovers, tree not restored).
set -euo pipefail

if (($# != 0)); then
    echo "release_gates_red: takes no arguments" >&2
    exit 2
fi

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=tests/gates/lib.sh
source "$here/lib.sh"

gate_begin

# mark_green, mark_red and red_with keep the contract they have in static_gates_red.sh.
declare -A green_result=()
declare -A red_result=()
mark_green() { green_result[$1]=ok; }
mark_red() { red_result[$1]=ok; }

# red_with {name} {token}... -- {cmd...}: gate_expect_red for a gate that must show more than one token
# (the gate's own marker and the seeded defect's name). The first token goes through gate_expect_red; every
# further token must also be in the red run's log, or the proof fails.
red_with() {
    local name=$1 first out status=0 token
    shift
    first=$1
    local rest=()
    shift
    while [[ ${1:-} != -- ]]; do
        rest+=("$1")
        shift
    done
    shift
    out=$(gate_expect_red "$name" "$first" -- "$@") || status=$?
    if ((status != 0)); then
        printf '%s\n' "$out"
        return 1
    fi
    for token in "${rest[@]}"; do
        if ! grep -qF -- "$token" "$GATE_LOGS/gates/$name.log"; then
            echo "RED-FAIL $name: the log lacks '$token' (see $GATE_LOGS/gates/$name.log)"
            return 1
        fi
    done
    printf '%s\n' "$out"
}

# step {label} -- {cmd...}: a setup command whose failure is a harness failure, not a red or a green.
step() {
    local label=$1 log status=0
    shift
    [[ ${1:-} == -- ]] && shift
    log="$GATE_LOGS/gates/setup-$label.log"
    "$@" >"$log" 2>&1 || status=$?
    if ((status != 0)); then
        echo "SETUP-FAIL $label: exit $status (see $log)"
        return 1
    fi
}

# The public header gate. Three seeds: a declaration appended to the entry-point header, a comment-only line
# appended to another header, and a new file under include/lws. The second seed is why the gate compares
# bytes: a comment changes no declaration, and the gate still refuses it.
proof_api_gate() {
    local rc=0
    gate_expect_green api-gate -- cmake "-DLWS_ROOT=$LWS_GATE_ROOT" \
        -P "$LWS_GATE_ROOT/scripts/api-gate.cmake" || return 1
    mark_green api-gate
    gate_patch_append include/lws/streamer.hpp 'inline constexpr int lws_seed_api_added = 1;'
    red_with api-gate '[api]' 'include/lws/streamer.hpp: changed' -- cmake "-DLWS_ROOT=$LWS_GATE_ROOT" \
        -P "$LWS_GATE_ROOT/scripts/api-gate.cmake" || rc=1
    gate_restore_now || return 1
    ((rc == 0)) || return 1
    gate_patch_append include/lws/ring_index.hpp '// a comment-only edit'
    red_with api-gate '[api]' 'include/lws/ring_index.hpp: changed' -- cmake "-DLWS_ROOT=$LWS_GATE_ROOT" \
        -P "$LWS_GATE_ROOT/scripts/api-gate.cmake" || rc=1
    gate_restore_now || return 1
    ((rc == 0)) || return 1
    gate_write include/lws/zz_seed_api.hpp '#pragma once'
    red_with api-gate '[api]' 'include/lws/zz_seed_api.hpp: added' -- cmake "-DLWS_ROOT=$LWS_GATE_ROOT" \
        -P "$LWS_GATE_ROOT/scripts/api-gate.cmake" || rc=1
    gate_restore_now || return 1
    ((rc == 0)) || return 1
    mark_red api-gate
}

# The install check. It builds the release preset under the seed tree once, runs the check green, then
# runs four seeds that each break the package a different way: an installed archive removed, the consumer
# including an internal header, a public include directory under the source tree, and the source path
# written into the package config. Each is refused with its own token. Then the tree is restored and the
# check runs green again.
proof_install() {
    local rc=0
    step install-deps -- scripts/deps.sh Release || return 1
    step install-configure -- cmake --preset release || return 1
    step install-build -- cmake --build --preset release || return 1
    gate_expect_green install -- scripts/install-check.sh || return 1
    mark_green install

    red_with install '[install]' 'libwhisper.a' -- env LWS_INSTALL_CHECK_DROP=lib/libwhisper.a \
        scripts/install-check.sh || rc=1
    ((rc == 0)) || return 1

    gate_patch_append tests/install/consumer.cpp '#include "shell/streamer_impl.hpp"'
    red_with install '[install]' 'consumer build failed' -- scripts/install-check.sh || rc=1
    gate_restore_now || return 1
    ((rc == 0)) || return 1

    # shellcheck disable=SC2016  # the text is CMake's, matched literally
    gate_patch_replace CMakeLists.txt '$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>' \
        '${PROJECT_SOURCE_DIR}/include'
    red_with install-generate 'prefixed in the source directory' -- cmake --preset release || rc=1
    gate_restore_now || return 1
    step install-reconfigure-1 -- cmake --preset release || return 1
    ((rc == 0)) || return 1

    gate_patch_append cmake/lws-config.cmake.in "# configured from $LWS_GATE_ROOT"
    step install-reconfigure-seed -- cmake --preset release || rc=1
    red_with install '[install]' 'source path in' -- scripts/install-check.sh || rc=1
    gate_restore_now || return 1
    step install-reconfigure-2 -- cmake --preset release || return 1
    ((rc == 0)) || return 1

    gate_expect_green install-restored -- scripts/install-check.sh || return 1
    mark_red install
}

mapfile -t proof_fns < <(sed -n 's/^\(proof_[a-z0-9_]*\)() *{.*/\1/p' "${BASH_SOURCE[0]}")
if ((${#proof_fns[@]} == 0)); then
    echo "release_gates_red: no proof functions found; refusing to pass an empty run" >&2
    exit 3
fi

failed=()
for fn in "${proof_fns[@]}"; do
    name=${fn#proof_}
    name=${name//_/-}
    status=0
    "$fn" || status=$?
    # A failed proof may have left its seed behind; the next proof must start from the clean tree.
    gate_restore_now || status=1
    if ((status != 0)); then
        failed+=("$name")
    fi
done

printf '\n%-24s %-6s %-6s\n' proof green red
for fn in "${proof_fns[@]}"; do
    name=${fn#proof_}
    name=${name//_/-}
    printf '%-24s %-6s %-6s\n' "$name" "${green_result[$name]:-FAIL}" "${red_result[$name]:-FAIL}"
done

if ((${#failed[@]} > 0)); then
    echo "RELEASE-GATES-FAILED n=${#proof_fns[@]} failed=${failed[*]}"
    exit 1
fi
echo "RELEASE-GATES-PROVEN n=${#proof_fns[@]}"
