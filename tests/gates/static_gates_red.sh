#!/usr/bin/env bash
# tests/gates/static_gates_red.sh: the static gates shown red on a seeded defect and green on the same
# command without it (D-04, GATE-01). No arguments.
#
# Every proof is a function named proof_{name}, the name's dashes written as underscores, defined at column 0.
# The runner derives the list of proofs from those definitions, in file order, so the functions are the one
# enumeration: a proof added here is run, counted and summarised without a second edit. A proof does three
# things: a green control (the gate's command on the clean tree), the seeded red (the same command shape with
# one defect inserted into a test-only file or a temporary file, never src/) and a byte-for-byte restore.
# gate_expect_red asserts the gate's own token and not merely a non-zero exit, so a command that dies for
# another reason (a wrong path, a missing tool) cannot pass as red.
#
# Everything runs under LWS_BUILD_ID=gate-seed (set by lib.sh), so build/gate/, check.sh's tree, is never
# touched. Exit 0 prints STATIC-GATES-PROVEN n={count}; exit 1 names the failed proofs; exit 3 is a harness
# error (leftovers, tree not restored).
set -euo pipefail

if (($# != 0)); then
    echo "static_gates_red: takes no arguments" >&2
    exit 2
fi

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=tests/gates/lib.sh
source "$here/lib.sh"

gate_begin

declare -A green_result=()
declare -A red_result=()
mark_green() { green_result[$1]=ok; }
mark_red() { red_result[$1]=ok; }

# step {label} -- {cmd...}: a setup command whose failure fails the proof (it is not itself a gate verdict).
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

proof_format() {
    local misformatted
    misformatted=$(
        cat <<'EOF'
#include <gtest/gtest.h>
TEST(SeedFormat,   Misformatted) {
      int   value=1;
  EXPECT_EQ(value, 1);   // this comment deliberately runs far past the one hundred column limit that .clang-format sets
}
EOF
    )
    gate_expect_green format -- scripts/format.sh || return 1
    mark_green format
    gate_write tests/smoke/zz_seed_format_test.cpp "$misformatted"
    gate_expect_red format 'not clang-formatted' -- scripts/format.sh || return 1
    mark_red format
}

mapfile -t proof_fns < <(sed -n 's/^\(proof_[a-z0-9_]*\)() *{.*/\1/p' "${BASH_SOURCE[0]}")
if ((${#proof_fns[@]} == 0)); then
    echo "static_gates_red: no proof functions found; refusing to pass an empty run" >&2
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
    echo "STATIC-GATES-FAILED n=${#proof_fns[@]} failed=${failed[*]}"
    exit 1
fi
echo "STATIC-GATES-PROVEN n=${#proof_fns[@]}"
