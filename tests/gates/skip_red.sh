#!/usr/bin/env bash
# tests/gates/skip_red.sh: proof that scripts/check.sh fails a leg whose ctest run skipped a test
# (GATE-01: a skip is never a pass). No arguments.
#
# GTEST_SKIP exits 0, and ctest exits 0 on a skipped test, so the rule lives in check.sh's count of
# `***Skipped` lines and not in ctest's exit status. This proof therefore drives check.sh itself, the whole
# path: gtest skip marker, SKIP_REGULAR_EXPRESSION, ctest's `***Skipped`, check.sh's count, check.sh's exit.
#   1. green control: scripts/check.sh with no seed exits 0 and ends with a CHECK-OK line.
#   2. red: tests/gates/seeds/skip_seed.cpp is inserted as tests/smoke/zz_seed_skip_test.cpp and the same
#      command must exit non-zero with check.sh's own phrase `a skip is never a pass`.
#   3. the seed's skipped line must be in some leg's ctest log. That separates a real skip from check.sh's
#      unreadable-log failure, which carries the same phrase (`a skip is never a pass: log unreadable`).
#      The presence check is scripts/lib/skips.sh's lws_has_skip_marker (a presence check, not a count);
#      this script reads the skip marker nowhere else.
# check.sh stops at the first leg with the skip, so the red run is short. Everything runs under
# LWS_BUILD_ID=gate-seed (set by lib.sh), so build/gate/ is never touched. Exit 0 prints SKIP-PROVEN.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=tests/gates/lib.sh
source "$here/lib.sh"

seed_test="SeedSkip.AlwaysSkips"
gate_begin

gate_expect_green skip-control -- scripts/check.sh || exit 1
last=$(tail -n 1 "$GATE_LOGS/gates/skip-control.log")
if [[ $last != CHECK-OK* ]]; then
    echo "GREEN-FAIL skip-control: the log does not end with a CHECK-OK line (last line: $last)"
    exit 1
fi

gate_insert tests/gates/seeds/skip_seed.cpp tests/smoke/zz_seed_skip_test.cpp
gate_expect_red skip 'a skip is never a pass' -- scripts/check.sh || exit 1

found_leg=""
for log in "$GATE_LOGS"/*.ctest.log; do
    [[ -e $log ]] || continue
    status=0
    lws_has_skip_marker "$log" "$seed_test" || status=$?
    case "$status" in
        0)
            found_leg=$(basename "$log" .ctest.log)
            break
            ;;
        1) ;;
        *)
            echo "RED-FAIL skip: ctest log unreadable ($log)"
            exit 1
            ;;
    esac
done
if [[ -z $found_leg ]]; then
    echo "RED-FAIL skip: no leg's ctest log shows $seed_test as skipped; the red is for another reason"
    exit 1
fi

gate_restore_now || exit 1
echo "SKIP-PROVEN leg=$found_leg"
