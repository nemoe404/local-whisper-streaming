#!/usr/bin/env bash
# tests/gates/leg_matrix.sh: the ctest leg matrix (plan 01-04). No arguments.
#
# Runs a control row (no seed) and five seeded defects against every candidate leg (the non-hidden configure
# presets of CMakePresets.json, in file order) and keeps a leg only if some seed turns it red while every
# other candidate stays green. Each seed in tests/gates/seeds/{name}.cpp is copied to
# tests/smoke/zz_seed_{name}_test.cpp for its row and removed after it. A ctest skip is a red cell
# (red:skip), so a skipped test can never make a cell green.
#
# Ordering guard: the rule and the prediction must be frozen before the matrix runs. The record
# measurements/ctest-legs.md is local-only and is never committed, so the freeze is a commit whose message
# carries the sha256 of the record's frozen part (everything before its first '## Setup' line). The matrix
# refuses (exit 3) unless the oldest such commit exists, is an ancestor of HEAD, and the record's frozen part
# still hashes to it. It writes the HEAD it ran at to build/gate-seed/logs/matrix/matrix.head.
#
# Exit: 0 every candidate leg is kept; 1 the matrix is invalid (control red, or a seed reddens nothing);
# 3 harness error (ordering guard, leftovers, unreadable ctest log, tree not restored);
# 4 some candidate has no unique catch (a decision is pending).
set -euo pipefail

if (($# != 0)); then
    echo "leg_matrix: takes no arguments" >&2
    exit 2
fi

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=tests/gates/lib.sh
source "$here/lib.sh"

record="measurements/ctest-legs.md"
freeze_subject="docs: freeze ctest leg decision rule and prediction"
seeds=(tsan_race asan_oob_write ubsan_signed_overflow release_ndebug_break debug_assert)

refuse() {
    echo "matrix refused: rule and prediction not committed ($record): $1" >&2
    exit 3
}

# Ordering guard, before gate_begin. Every value is read from a file or a command substitution, never
# through a quiet match on a pipe.
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
git log --format='%H%x09%s' >"$tmp/log.tsv"
freeze_sha=$(awk -F'\t' -v subject="$freeze_subject" '$2 == subject { sha = $1 } END { print sha }' "$tmp/log.tsv")
[[ -n $freeze_sha ]] || refuse "no freeze commit"
git show -s --format=%B "$freeze_sha" >"$tmp/freeze.msg"
want=$(sed -n 's/^ctest-legs-frozen-sha256: \([0-9a-f]\{64\}\)$/\1/p' "$tmp/freeze.msg")
[[ -n $want ]] || refuse "the freeze commit carries no sha256"
[[ -f $record ]] || refuse "the record is missing"
awk '/^## Setup/ { exit } { print }' "$record" >"$tmp/frozen.md"
have=$(sha256sum "$tmp/frozen.md" | cut -d' ' -f1)
[[ $have == "$want" ]] || refuse "the frozen part of the record differs from the freeze commit"
grep -qxF '## Rule (frozen before the run)' "$tmp/frozen.md" || refuse "no rule heading"
grep -qxF '## Prediction' "$tmp/frozen.md" || refuse "no prediction heading"
git merge-base --is-ancestor "$freeze_sha" HEAD || refuse "the freeze commit is not an ancestor of HEAD"
trap - EXIT
rm -rf "$tmp"

gate_begin
matrix_dir="$GATE_LOGS/matrix"
git rev-parse HEAD >"$matrix_dir/matrix.head"

mapfile -t legs < <(python3 -c '
import json
for preset in json.load(open("CMakePresets.json"))["configurePresets"]:
    if not preset.get("hidden"):
        print(preset["name"])
')
if ((${#legs[@]} == 0)); then
    echo "GATE-HARNESS: no candidate leg in CMakePresets.json" >&2
    exit 3
fi

declare -A cell
rows=(control "${seeds[@]}")
for row in "${rows[@]}"; do
    if [[ $row != control ]]; then
        gate_insert "tests/gates/seeds/$row.cpp" "tests/smoke/zz_seed_${row}_test.cpp"
    fi
    for leg in "${legs[@]}"; do
        echo "matrix: row $row leg $leg" >&2
        cell["$row|$leg"]=$(gate_leg_run "$row" "$leg") || exit 3
        echo "matrix: row $row leg $leg: ${cell["$row|$leg"]}" >&2
    done
    gate_restore_now || exit 3
done

tsv="$matrix_dir/matrix.tsv"
: >"$tsv"
for row in "${rows[@]}"; do
    for leg in "${legs[@]}"; do
        printf '%s\t%s\t%s\n' "$row" "$leg" "${cell["$row|$leg"]}" >>"$tsv"
    done
done

# The table.
printf '\n%-24s' "row"
for leg in "${legs[@]}"; do printf '%-16s' "$leg"; done
printf '\n'
for row in "${rows[@]}"; do
    printf '%-24s' "$row"
    for leg in "${legs[@]}"; do printf '%-16s' "${cell["$row|$leg"]}"; done
    printf '\n'
done
printf '\n'

# Validity.
for leg in "${legs[@]}"; do
    if [[ ${cell["control|$leg"]} != green ]]; then
        echo "matrix invalid: control red on $leg (${cell["control|$leg"]})"
        exit 1
    fi
done
for row in "${seeds[@]}"; do
    reds=0
    for leg in "${legs[@]}"; do
        [[ ${cell["$row|$leg"]} == green ]] || reds=$((reds + 1))
    done
    if ((reds == 0)); then
        echo "seed $row reddens nothing"
        exit 1
    fi
done

# A leg is kept when some seed is red on it and green on every other candidate.
keep=()
drop=()
for leg in "${legs[@]}"; do
    kept=0
    for row in "${seeds[@]}"; do
        [[ ${cell["$row|$leg"]} == green ]] && continue
        alone=1
        for other in "${legs[@]}"; do
            [[ $other == "$leg" ]] && continue
            [[ ${cell["$row|$other"]} == green ]] || alone=0
        done
        if ((alone == 1)); then
            kept=1
            break
        fi
    done
    if ((kept == 1)); then keep+=("$leg"); else drop+=("$leg"); fi
done
echo "LEGS-KEEP ${keep[*]}"
echo "LEGS-DROP ${drop[*]}"
if ((${#drop[@]} == 0)); then
    exit 0
fi
exit 4
