#!/usr/bin/env bash
# tests/gates/cuda_host_red.sh: the configure-time proof of the CUDA host gate. No arguments.
#
# The root enables CUDA itself and runs the toolchain verdict on the CUDA compiler and its host before ggml
# configures. This script proves that verdict, and the toolchain gate's configured layer that reads what a
# CUDA leg recorded, from both sides in fresh binary directories:
#   cuda-host           `cmake --preset cuda` configures with nvcc 13 and the clang 21 host and prints the CUDA
#                       verdict line; a seeded preset that inherits `cuda` but forces the CUDA host to g++ stops
#                       at configure with the verdict's own `[toolchain][cuda-host]` refusal naming GNU.
#   cuda-leg-identity   the configured toolchain gate is green on the clang 21 CUDA leg; the leg left by the
#                       refused g++ configure (CMake recorded the CUDA identity before the verdict stopped it)
#                       is refused by the same gate with the same `[toolchain][cuda-host]` token.
#   cuda-nvcc-host      a copy of the green CUDA leg whose compile database names g++ as every nvcc host
#                       is refused for the `nvcc host` of its entries.
#
# It needs a CUDA 13 toolkit: nvcc at ${LWS_CUDA_NVCC:-/usr/local/cuda/bin/nvcc}. The variable is a test seam;
# a path that does not exist simulates a machine without the toolkit.
#
# Exit: 0 proven (CUDA-HOST-PROVEN proofs=3); 1 a proof failed; 3 harness error (leftovers, tree not
# restored); 77 no CUDA 13 toolkit (run.sh reports that as NOT RUN, never as a pass).
set -euo pipefail

if (($# != 0)); then
    echo "cuda_host_red: takes no arguments" >&2
    exit 2
fi

nvcc=${LWS_CUDA_NVCC:-/usr/local/cuda/bin/nvcc}
if [[ ! -x $nvcc ]]; then
    echo "cuda_host_red: $nvcc is not executable; no CUDA toolkit to prove against" >&2
    exit 77
fi
nvcc_status=0
nvcc_version=$("$nvcc" --version 2>&1) || nvcc_status=$?
if ((nvcc_status != 0)) || ! grep -qF 'release 13.' <<<"$nvcc_version"; then
    echo "cuda_host_red: $nvcc is not a CUDA 13 toolkit; nothing to prove against" >&2
    exit 77
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

# red_with_tokens {name} {token}... -- {cmd...}: gate_expect_red for a gate that must show more than one token.
# The first token goes through gate_expect_red; every further token must also be in the red run's log.
red_with_tokens() {
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

# The pinned clang 21 host configures and prints the verdict line; a seeded g++ host is refused.
proof_cuda_host() {
    local rc=0
    step deps-release -- scripts/deps.sh Release || return 1
    rm -rf -- "$LWS_GATE_ROOT/build/gate-seed/cuda" "$LWS_GATE_ROOT/build/gate-seed/seed-cuda-gcc"
    gate_expect_green cuda-configure -- cmake --preset cuda || return 1
    if ! grep -qF 'lws: [toolchain][cuda] holds' "$GATE_LOGS/gates/cuda-configure.log"; then
        echo "GREEN-FAIL cuda-configure: the log lacks the CUDA verdict line (see $GATE_LOGS/gates/cuda-configure.log)"
        return 1
    fi
    mark_green cuda-host
    gate_write CMakeUserPresets.json '{
  "version": 6,
  "configurePresets": [
    {
      "name": "seed-cuda-gcc",
      "inherits": "cuda",
      "cacheVariables": { "CMAKE_CUDA_HOST_COMPILER": "g++" }
    }
  ]
}'
    gate_expect_red cuda-host-gcc '[toolchain][cuda-host]' -- cmake --preset seed-cuda-gcc || rc=1
    if ((rc == 0)) && ! grep -qF 'GNU' "$GATE_LOGS/gates/cuda-host-gcc.log"; then
        echo "RED-FAIL cuda-host-gcc: the refusal does not name GNU (see $GATE_LOGS/gates/cuda-host-gcc.log)"
        rc=1
    fi
    gate_restore_now || rc=1
    ((rc == 0)) || return 1
    mark_red cuda-host
}

leg_gate() {
    cmake "-DLWS_ROOT=$LWS_GATE_ROOT" "-DLWS_BUILD_ID=$LWS_BUILD_ID" "-DLWS_LEGS=$2" \
        -P "$LWS_GATE_ROOT/scripts/$1"
}

# The configured layer reads the CUDA identity CMake recorded: a leg configured with a g++ host is refused
# although the static layer (user preset restored) is green.
proof_cuda_leg_identity() {
    local cache="$LWS_GATE_ROOT/build/gate-seed/seed-cuda-gcc/CMakeCache.txt"
    if [[ ! -f "$LWS_GATE_ROOT/build/gate-seed/cuda/compile_commands.json" ]]; then
        step cuda-configure-identity -- cmake --preset cuda || return 1
    fi
    gate_expect_green cuda-leg-green -- leg_gate toolchain-gate.cmake cuda || return 1
    mark_green cuda-leg-identity
    rm -rf -- "$LWS_GATE_ROOT/build/gate-seed/seed-cuda-gcc"
    gate_write CMakeUserPresets.json '{
  "version": 6,
  "configurePresets": [
    {
      "name": "seed-cuda-gcc",
      "inherits": "cuda",
      "cacheVariables": { "CMAKE_CUDA_HOST_COMPILER": "g++" }
    }
  ]
}'
    # The configure stops at the root verdict; its exit status is the other proof's business.
    cmake --preset seed-cuda-gcc >"$GATE_LOGS/gates/setup-seed-cuda-gcc.log" 2>&1 || true
    gate_restore_now || return 1
    # Recorded for the reader: what the stopped configure left in the cache for GGML_CUDA.
    grep -E '^GGML_CUDA:' "$cache" >"$GATE_LOGS/gates/seed-cuda-gcc-ggml-cuda.txt" || true
    red_with_tokens cuda-leg-gcc '[toolchain][leg seed-cuda-gcc]' '[toolchain][cuda-host]' -- \
        leg_gate toolchain-gate.cmake seed-cuda-gcc || return 1
    mark_red cuda-leg-identity
}

# Every nvcc compile-database entry names its host; a database whose hosts were rewritten to g++ is refused
# for the entries, with the recorded identity left clang 21 so only the entry sweep can refuse.
proof_cuda_nvcc_host() {
    local green="$LWS_GATE_ROOT/build/gate-seed/cuda" copy="$LWS_GATE_ROOT/build/gate-seed/seed-nvcc-host"
    local rewritten
    if [[ ! -f $green/compile_commands.json ]]; then
        step cuda-configure-nvcc -- cmake --preset cuda || return 1
    fi
    rm -rf -- "$copy"
    mkdir -p -- "$copy/CMakeFiles"
    cp -- "$green/CMakeCache.txt" "$green/compile_commands.json" "$copy/"
    cp -r -- "$green"/CMakeFiles/[0-9]*.[0-9]* "$copy/CMakeFiles/"
    gate_expect_green cuda-nvcc-copy -- leg_gate toolchain-gate.cmake seed-nvcc-host || {
        rm -rf -- "$copy"
        return 1
    }
    mark_green cuda-nvcc-host
    rewritten=$(python3 -I - "$copy/compile_commands.json" <<'PY'
import json, re, sys
path = sys.argv[1]
with open(path, encoding="utf-8") as f:
    entries = json.load(f)
host = re.compile(r"(-ccbin|--compiler-bindir)([= ])\S+")
count = 0
for entry in entries:
    if "command" in entry:
        if entry["command"].split()[0].endswith("nvcc"):
            entry["command"] = host.sub(r"\1\2/usr/bin/g++", entry["command"])
            count += 1
    else:
        args = entry["arguments"]
        if args[0].endswith("nvcc"):
            for i, a in enumerate(args):
                if a in ("-ccbin", "--compiler-bindir") and i + 1 < len(args):
                    args[i + 1] = "/usr/bin/g++"
                elif a.startswith(("-ccbin=", "--compiler-bindir=")):
                    args[i] = a.split("=", 1)[0] + "=/usr/bin/g++"
            count += 1
with open(path, "w", encoding="utf-8") as f:
    json.dump(entries, f)
print(count)
PY
    ) || {
        rm -rf -- "$copy"
        return 1
    }
    if ((rewritten == 0)); then
        echo "SETUP-FAIL cuda-nvcc-host: no nvcc entry found to rewrite"
        rm -rf -- "$copy"
        return 1
    fi
    local rc=0
    red_with_tokens cuda-nvcc-host-gcc '[toolchain][leg seed-nvcc-host]' 'nvcc host' -- \
        leg_gate toolchain-gate.cmake seed-nvcc-host || rc=1
    rm -rf -- "$copy"
    ((rc == 0)) || return 1
    mark_red cuda-nvcc-host
}

mapfile -t proof_fns < <(sed -n 's/^\(proof_[a-z0-9_]*\)() *{.*/\1/p' "${BASH_SOURCE[0]}")
if ((${#proof_fns[@]} == 0)); then
    echo "cuda_host_red: no proof functions found; refusing to pass an empty run" >&2
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
    echo "CUDA-HOST-FAILED proofs=${#proof_fns[@]} failed=${failed[*]}"
    exit 1
fi
echo "CUDA-HOST-PROVEN proofs=${#proof_fns[@]}"
