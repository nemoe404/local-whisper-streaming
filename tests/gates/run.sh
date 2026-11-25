#!/usr/bin/env bash
# tests/gates/run.sh: the one entry point of the seeded-defect proofs. No arguments.
#
# It runs every proof script, each to its own log under build/gate-seed/logs/run/, reads each exit status
# directly and prints one line per script: PASS, FAIL, or NOT RUN. Exit 77 means NOT RUN only for the scripts
# named in not_run_reason below: legs_consistency.sh when the local-only AGENTS.md is absent, and
# cuda_host_red.sh when the machine has no CUDA 13 toolkit. A 77 from any other script is a FAIL. A NOT RUN is
# reported and never counted as a pass. The last line is GATES-PROVEN scripts={passed} not-run={n}. Exit 0 only
# when no script failed.
#
# The script is run by hand on this machine. Hosted CI runs scripts/check.sh only: two of these scripts need
# local-only files, and the leg matrix rebuilds every leg once per seed.
set -euo pipefail

if (($# != 0)); then
    echo "run.sh: takes no arguments" >&2
    exit 2
fi

root=$(git rev-parse --show-toplevel)
cd "$root"

gate_scripts=(
tests/gates/fetch_models_test.sh
tests/gates/skip_red.sh
tests/gates/static_gates_red.sh
tests/gates/release_gates_red.sh
tests/gates/cuda_host_red.sh
tests/gates/leg_matrix.sh
tests/gates/legs_consistency.sh
)

# Scripts that may exit 77 ("not run") and why. Nothing else is allowed to.
declare -A not_run_reason=(
    [tests/gates/legs_consistency.sh]="AGENTS.md absent"
    [tests/gates/cuda_host_red.sh]="no CUDA 13 toolkit"
)

mkdir -p build/gate-seed/logs/run

passed=0
not_run=0
failed=0
for script in "${gate_scripts[@]}"; do
    log="build/gate-seed/logs/run/$(basename "$script" .sh).log"
    status=0
    "$script" >"$log" 2>&1 || status=$?
    if ((status == 0)); then
        echo "PASS $script"
        passed=$((passed + 1))
    elif ((status == 77)) && [[ -n ${not_run_reason[$script]:-} ]]; then
        echo "NOT RUN $script (${not_run_reason[$script]})"
        not_run=$((not_run + 1))
    else
        echo "FAIL $script (exit $status, see $log)"
        failed=$((failed + 1))
    fi
done

if ((failed > 0)); then
    echo "GATES-FAILED scripts=$passed not-run=$not_run failed=$failed"
    exit 1
fi
echo "GATES-PROVEN scripts=$passed not-run=$not_run"
