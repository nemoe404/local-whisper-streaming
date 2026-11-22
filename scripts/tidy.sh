#!/usr/bin/env bash
# clang-tidy gate for the repo-owned translation units of the asan-ubsan compile database.
#
#   scripts/tidy.sh <file>...        check the given files (cheap, run after every edit)
#   scripts/tidy.sh --all            check every owned translation unit (the gate)
#   scripts/tidy.sh --fix <file>...  apply clang-tidy's automatic fixes first (serial)
#
# Owned means: not under third_party/ or build/, and inside the repository. Those units (whisper.cpp
# and ggml) share our compile database and are not ours to gate. The filter is a denylist, so a new
# owned top-level directory is swept without an edit.
#
# Exit: 0 clean, 1 findings, 2 usage, no compile database, or an empty sweep, 3 file not an owned
# translation unit, 4 clang-tidy did not analyse a file. Exit 4 is not a pass: it means nothing was checked.
set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

id=${LWS_BUILD_ID:-gate}
preset=${LWS_DB_PRESET:-asan-ubsan}
build_dir="build/$id/$preset"
db="$build_dir/compile_commands.json"
tidy=${CLANG_TIDY:-clang-tidy-21}
logs="build/$id/logs"

usage() {
    echo "usage: scripts/tidy.sh [--fix] (--all | <file>...)" >&2
    exit 2
}

fix=()
if [[ ${1:-} == --fix ]]; then
    fix=(--fix)
    shift
fi
[[ $# -ge 1 ]] || usage

[[ -f $db ]] || {
    echo "tidy: no $db; configure and build the $preset preset first (LWS_BUILD_ID=$id)" >&2
    exit 2
}

# The owned translation units, as paths relative to the repository root.
owned_files() {
    python3 - "$db" "$root" <<'PY'
import json, os, sys
db, root = sys.argv[1], sys.argv[2]
excluded = {"third_party", "build", ".."}
seen = set()
for entry in json.load(open(db, encoding="utf-8")):
    rel = os.path.relpath(entry["file"], root)
    if rel.split(os.sep, 1)[0] in excluded:
        continue
    if rel not in seen:
        seen.add(rel)
        print(rel)
PY
}

mapfile -t known < <(owned_files)
if ((${#known[@]} == 0)); then
    echo "tidy: no owned translation unit in $db: the sweep would be empty (exit 2, not a pass)" >&2
    exit 2
fi

targets=()
if [[ $1 == --all ]]; then
    targets=("${known[@]}")
else
    for arg in "$@"; do
        norm=$(realpath --relative-to="$root" "$arg")
        found=0
        for k in "${known[@]}"; do
            if [[ $k == "$norm" ]]; then
                found=1
                break
            fi
        done
        if ((found == 0)); then
            echo "tidy: $arg is not an owned translation unit in $db; it would not be analysed" >&2
            exit 3
        fi
        targets+=("$norm")
    done
fi

mkdir -p "$logs"
findings=0
not_analysed=0
for file in "${targets[@]}"; do
    log="$logs/tidy-$(tr / _ <<<"$file").log"
    status=0
    "$tidy" -p "$build_dir" --quiet "${fix[@]}" "$file" >"$log" 2>&1 || status=$?
    if grep -qE 'Error while processing|Error reading configuration|unable to handle compilation' "$log"; then
        echo "tidy: NOT ANALYSED $file" >&2
        # awk, not grep | head: under pipefail a head that closes early would abort this script.
        awk '/Error/ && n++ < 3' "$log" >&2
        not_analysed=$((not_analysed + 1))
    elif ((status != 0)); then
        awk '/^[^ ].*:[0-9]+:[0-9]+: (warning|error): / && n++ < 30' "$log"
        findings=$((findings + 1))
    fi
done

if ((not_analysed > 0)); then
    echo "tidy: $not_analysed of ${#targets[@]} file(s) not analysed" >&2
    exit 4
fi
if ((findings > 0)); then
    echo "tidy: $findings of ${#targets[@]} file(s) with findings" >&2
    exit 1
fi
echo "TIDY-OK files=${#targets[@]} findings=0"
