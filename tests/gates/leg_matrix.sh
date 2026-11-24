#!/usr/bin/env bash
# tests/gates/leg_matrix.sh: the ctest leg matrix. No arguments.
#
# Runs a control row (no seed) and five seeded defects against every candidate leg (the non-hidden configure
# presets of CMakePresets.json not marked hand-run, in file order; tests/gates/preset_legs.py lists them)
# and keeps a leg only if some seed turns it red while every other candidate stays green.
# Each seed in tests/gates/seeds/{name}.cpp is copied to tests/smoke/zz_seed_{name}_test.cpp for its row and
# removed after it. A ctest skip is a red cell (red:skip), so a skipped test can never make a cell green.
#
# Ordering guard: the rule and the prediction must be frozen before the matrix runs. The record
# measurements/ctest-legs.md is local-only and is never committed, so git history cannot witness the freeze.
# Its frozen part (everything before the first '## Setup' line) must hash to the value recorded at freeze
# time in measurements/ctest-legs.frozen.sha256. The matrix refuses (exit 3) otherwise. This detects later
# edits to the rule; it does not stop a deliberate rewrite of both files on this machine. The HEAD it ran at
# is written to build/gate-seed/logs/matrix/matrix.head.
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
freeze_hash_file="measurements/ctest-legs.frozen.sha256"
seeds=(tsan_race asan_oob_write ubsan_signed_overflow release_ndebug_break debug_assert)

refuse() {
    echo "matrix refused: rule and prediction not frozen ($record): $1" >&2
    exit 3
}

# Ordering guard, before gate_begin. Every value is read from a file or a command substitution, never
# through a quiet match on a pipe.
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
[[ -f $record ]] || refuse "the record is missing"
[[ -f $freeze_hash_file ]] || refuse "no freeze hash file ($freeze_hash_file)"
want=$(cat "$freeze_hash_file")
[[ $want =~ ^[0-9a-f]{64}$ ]] || refuse "the freeze hash file does not hold a sha256"
awk '/^## Setup/ { exit } { print }' "$record" >"$tmp/frozen.md"
have=$(sha256sum "$tmp/frozen.md" | cut -d' ' -f1)
[[ $have == "$want" ]] || refuse "the frozen part of the record differs from $freeze_hash_file"
grep -qxF '## Rule (frozen before the run)' "$tmp/frozen.md" || refuse "no rule heading"
grep -qxF '## Prediction' "$tmp/frozen.md" || refuse "no prediction heading"
trap - EXIT
rm -rf "$tmp"

gate_begin
matrix_dir="$GATE_LOGS/matrix"
git rev-parse HEAD >"$matrix_dir/matrix.head"

legs_text=$(python3 -I tests/gates/preset_legs.py CMakePresets.json) || {
    echo "GATE-HARNESS: cannot list the candidate legs" >&2
    exit 3
}
mapfile -t legs <<<"$legs_text"
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
