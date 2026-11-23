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

# preset_info {name|-}: prints "{preset} {build type}". `-` selects the first non-hidden configure preset
# of CMakePresets.json; the build type is resolved through `inherits`.
preset_info() {
    python3 -I - "$LWS_GATE_ROOT/CMakePresets.json" "$1" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1], encoding="utf-8"))
presets = {p["name"]: p for p in doc["configurePresets"]}
want = sys.argv[2]
if want == "-":
    visible = [p["name"] for p in doc["configurePresets"] if not p.get("hidden", False)]
    if not visible:
        sys.exit(2)
    want = visible[0]

def build_type(name):
    preset = presets[name]
    value = preset.get("cacheVariables", {}).get("CMAKE_BUILD_TYPE")
    if value:
        return value
    parents = preset.get("inherits", [])
    for parent in [parents] if isinstance(parents, str) else parents:
        found = build_type(parent)
        if found:
            return found
    return None

kind = build_type(want)
if kind not in ("Debug", "Release"):
    sys.exit(2)
print(want, kind)
PY
}

# db_preset_default {script}: the LWS_DB_PRESET default written in scripts/tidy.sh or scripts/cppcheck.sh.
db_preset_default() {
    # shellcheck disable=SC2016 # the pattern matches the literal ${...} written in the scripts
    sed -n 's/^preset=\${LWS_DB_PRESET:-\([a-z0-9-]*\)}$/\1/p' "$1"
}

# prepare {preset}: install the Conan dependencies for the preset's build type, then configure it.
prepare() {
    local preset=$1 info ptype
    info=$(preset_info "$preset") || {
        echo "SETUP-FAIL preset $preset: not a configure preset with a Debug or Release build type"
        return 1
    }
    ptype=${info#* }
    step "deps-$preset" -- scripts/deps.sh "$ptype" || return 1
    step "cfg-$preset" -- cmake --preset "$preset" || return 1
}

# The gates as commands. Static toolchain gate: presets only. The configured gates read build/gate-seed/{leg}.
toolchain_static() { cmake "-DLWS_ROOT=$LWS_GATE_ROOT" -P "$LWS_GATE_ROOT/scripts/toolchain-gate.cmake"; }
leg_gate() {
    cmake "-DLWS_ROOT=$LWS_GATE_ROOT" "-DLWS_BUILD_ID=$LWS_BUILD_ID" "-DLWS_LEGS=$2" \
        -P "$LWS_GATE_ROOT/scripts/$1"
}

# A user preset that names one compiler pair and inherits everything else from `base`.
seed_user_preset() {
    printf '{"version": 6, "configurePresets": [{"name": "%s", "inherits": "base", "cacheVariables": {"CMAKE_C_COMPILER": "%s", "CMAKE_CXX_COMPILER": "%s"}}]}' "$1" "$2" "$3"
}
# The bare variant: no inherits and no toolchain file, so configure reaches the compiler verdict with gcc.
# shellcheck disable=SC2016 # ${sourceDir} is a CMake preset macro, not a shell expansion
seed_bare_preset='{"version": 6, "configurePresets": [{"name": "seed-gcc-bare", "generator": "Ninja", "binaryDir": "${sourceDir}/build/gate-seed/seed-gcc-bare", "cacheVariables": {"CMAKE_C_COMPILER": "gcc", "CMAKE_CXX_COMPILER": "g++", "LWS_CPU_BASELINE": "x86-64-v3"}}]}'

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

proof_toolchain_seed_gcc() {
    gate_expect_green toolchain-seed-gcc -- toolchain_static || return 1
    mark_green toolchain-seed-gcc
    gate_write CMakeUserPresets.json "$(seed_user_preset seed-gcc gcc g++)"
    red_with toolchain-seed-gcc '[toolchain][preset seed-gcc]' -- toolchain_static || return 1
    mark_red toolchain-seed-gcc
}

proof_toolchain_seed_clang20() {
    gate_expect_green toolchain-seed-clang20 -- toolchain_static || return 1
    mark_green toolchain-seed-clang20
    gate_write CMakeUserPresets.json "$(seed_user_preset seed-clang20 clang-20 clang++-20)"
    red_with toolchain-seed-clang20 '[toolchain][preset seed-clang20]' -- toolchain_static || return 1
    mark_red toolchain-seed-clang20
}

proof_toolchain_configure() {
    prepare asan-ubsan || return 1
    gate_expect_green toolchain-configure -- cmake --preset asan-ubsan || return 1
    mark_green toolchain-configure
    rm -rf -- "$LWS_GATE_ROOT/build/gate-seed/seed-gcc-bare"
    gate_write CMakeUserPresets.json "$seed_bare_preset"
    red_with toolchain-configure '[toolchain]' -- cmake --preset seed-gcc-bare || return 1
    mark_red toolchain-configure
}

proof_toolchain_leg() {
    prepare asan-ubsan || return 1
    gate_expect_green toolchain-leg -- leg_gate toolchain-gate.cmake asan-ubsan || return 1
    mark_green toolchain-leg
    # Configure the gcc leg (it stops at the compiler verdict, after CMake wrote its compiler-ID files),
    # then remove the user preset so the static layer is green and only the configured layer can refuse.
    rm -rf -- "$LWS_GATE_ROOT/build/gate-seed/seed-gcc-bare"
    gate_write CMakeUserPresets.json "$seed_bare_preset"
    cmake --preset seed-gcc-bare >"$GATE_LOGS/gates/setup-seed-gcc-bare.log" 2>&1 || true
    gate_restore_now || return 1
    red_with toolchain-leg '[toolchain][leg seed-gcc-bare]' -- leg_gate toolchain-gate.cmake seed-gcc-bare ||
        return 1
    mark_red toolchain-leg
}

proof_isa() {
    local info preset ptype rc=0
    info=$(preset_info -) || return 1
    preset=${info% *}
    ptype=${info#* }
    step deps-isa -- scripts/deps.sh "$ptype" || return 1
    step cfg-isa -- cmake --preset "$preset" || return 1
    gate_expect_green isa -- leg_gate isa-gate.cmake "$preset" || return 1
    mark_green isa
    gate_patch_append tests/CMakeLists.txt 'target_compile_options(smoke_link_test PRIVATE -march=native)'
    step cfg-isa-seeded -- cmake --preset "$preset" || return 1
    red_with isa '[isa]' 'link_test' -- leg_gate isa-gate.cmake "$preset" || rc=1
    gate_restore_now || rc=1
    step cfg-isa-restored -- cmake --preset "$preset" || rc=1
    ((rc == 0)) || return 1
    mark_red isa
}

proof_sanitizer_gate() {
    local rc=0
    prepare asan-ubsan || return 1
    gate_expect_green sanitizer-gate -- leg_gate sanitizer-gate.cmake asan-ubsan || return 1
    mark_green sanitizer-gate
    step cfg-scope-lws-only -- cmake --preset asan-ubsan -DLWS_SANITIZER_SCOPE=lws-only || return 1
    red_with sanitizer-gate '[sanitizer]' -- leg_gate sanitizer-gate.cmake asan-ubsan || rc=1
    step cfg-scope-all -- cmake --preset asan-ubsan -DLWS_SANITIZER_SCOPE=all || rc=1
    ((rc == 0)) || return 1
    mark_red sanitizer-gate
}

proof_sanitizer_typo() {
    local rc=0
    prepare asan-ubsan || return 1
    gate_expect_green sanitizer-typo -- cmake --preset asan-ubsan || return 1
    mark_green sanitizer-typo
    red_with sanitizer-typo 'unknown LWS_SANITIZER' -- cmake --preset asan-ubsan -DLWS_SANITIZER=nonsense ||
        rc=1
    step cfg-sanitizer-restored -- cmake --preset asan-ubsan -DLWS_SANITIZER=asan-ubsan || rc=1
    ((rc == 0)) || return 1
    mark_red sanitizer-typo
}

proof_cppcheck() {
    local db rc=0
    db=$(db_preset_default scripts/cppcheck.sh)
    [[ -n $db && $db == "$(db_preset_default scripts/tidy.sh)" ]] || {
        echo "SETUP-FAIL cppcheck: no single LWS_DB_PRESET default in scripts/cppcheck.sh and scripts/tidy.sh"
        return 1
    }
    prepare "$db" || return 1
    gate_expect_green cppcheck -- scripts/cppcheck.sh || return 1
    mark_green cppcheck
    gate_insert tests/gates/seeds/cppcheck_defect.cpp tests/smoke/zz_seed_cppcheck_test.cpp
    step cfg-cppcheck-seeded -- cmake --preset "$db" || return 1
    gate_expect_red cppcheck 'zz_seed_cppcheck_test.cpp' -- scripts/cppcheck.sh || rc=1
    gate_restore_now || rc=1
    step cfg-cppcheck-restored -- cmake --preset "$db" || rc=1
    ((rc == 0)) || return 1
    mark_red cppcheck
}

proof_tidy() {
    local db rc=0
    db=$(db_preset_default scripts/tidy.sh)
    [[ -n $db && $db == "$(db_preset_default scripts/cppcheck.sh)" ]] || {
        echo "SETUP-FAIL tidy: no single LWS_DB_PRESET default in scripts/cppcheck.sh and scripts/tidy.sh"
        return 1
    }
    prepare "$db" || return 1
    gate_expect_green tidy -- scripts/tidy.sh --all || return 1
    mark_green tidy
    gate_insert tests/gates/seeds/tidy_defect.cpp tests/smoke/zz_seed_tidy_test.cpp
    step cfg-tidy-seeded -- cmake --preset "$db" || return 1
    gate_expect_red tidy 'zz_seed_tidy_test.cpp' -- scripts/tidy.sh --all || rc=1
    gate_restore_now || rc=1
    step cfg-tidy-restored -- cmake --preset "$db" || rc=1
    ((rc == 0)) || return 1
    mark_red tidy
}

proof_legs_consistency() {
    local source=${LWS_LEGS_AGENTS_MD:-$LWS_GATE_ROOT/AGENTS.md} dir legs rc=0
    dir=$(mktemp -d)
    if [[ ! -f $source ]]; then
        # AGENTS.md is local-only, so a fresh clone has none. The proof then runs on a fixture built from
        # the decision line, outside the repository; legs_consistency.sh itself still reports 77 on its own.
        legs=$(sed -n 's/^Decision: legs = //p' measurements/ctest-legs.md)
        printf 'ctest legs: %s\n' "$legs" >"$dir/fixture.md"
        source=$dir/fixture.md
        echo "NOTE legs-consistency: AGENTS.md is absent; proving on a fixture built from the decision line"
    fi
    gate_expect_green legs-consistency -- env "LWS_LEGS_AGENTS_MD=$source" tests/gates/legs_consistency.sh ||
        rc=1
    if ((rc == 0)); then
        mark_green legs-consistency
        # The differing copy lives outside the repository: the last leg is removed from its `ctest legs:` line.
        python3 -I - "$source" "$dir/differs.md" <<'PY' || rc=1
import re, sys

text = open(sys.argv[1], encoding="utf-8").read()


def drop_last(match):
    items = [item.strip() for item in match.group(1).split(",")]
    return "ctest legs: " + ", ".join(items[:-1])


out, count = re.subn(
    r"ctest legs:\s*([a-z0-9][a-z0-9-]*(?:\s*,\s*[a-z0-9][a-z0-9-]*)*)", drop_last, text, count=1
)
if count != 1:
    sys.exit(2)
open(sys.argv[2], "w", encoding="utf-8").write(out)
PY
    fi
    if ((rc == 0)); then
        gate_expect_red legs-consistency LEGS-DIFFER -- env "LWS_LEGS_AGENTS_MD=$dir/differs.md" \
            tests/gates/legs_consistency.sh || rc=1
    fi
    rm -rf -- "$dir"
    ((rc == 0)) || return 1
    mark_red legs-consistency
}

# GATE-02: the tuning-values gate. The seed names are camel case on purpose: they are the case the
# gate's lowercase step exists to catch. The gate is a text scan, so a header is never compiled.
proof_tuning_values() {
    local rc=0
    gate_expect_green tuning -- cmake "-DLWS_ROOT=$LWS_GATE_ROOT" -P "$LWS_GATE_ROOT/scripts/tuning-gate.cmake" ||
        return 1
    mark_green tuning-values
    gate_write include/lws/zz_seed_tuning.hpp 'inline constexpr float kSeededThreshold = 0.5F;'
    red_with tuning '[tuning]' 'kSeededThreshold' -- cmake "-DLWS_ROOT=$LWS_GATE_ROOT" \
        -P "$LWS_GATE_ROOT/scripts/tuning-gate.cmake" || rc=1
    gate_restore_now || return 1
    ((rc == 0)) || return 1
    gate_patch_append include/lws/options.hpp 'inline constexpr std::uint32_t kSeededOverlap = 250;'
    red_with tuning '[tuning]' 'kSeededOverlap' -- cmake "-DLWS_ROOT=$LWS_GATE_ROOT" \
        -P "$LWS_GATE_ROOT/scripts/tuning-gate.cmake" || rc=1
    gate_restore_now || return 1
    ((rc == 0)) || return 1
    mark_red tuning-values
}

# GATE-02 companion: clang-tidy reaches the public headers through the .cpp that includes them. The seed is
# readability-container-size-empty written as an inline function (the gtest body of seeds/tidy_defect.cpp
# cannot sit in a header). A compile error in place of a finding would not satisfy the path token below.
proof_tidy_public_header() {
    local db rc=0
    db=$(db_preset_default scripts/tidy.sh)
    prepare "$db" || return 1
    gate_expect_green tidy-public-header -- scripts/tidy.sh --all || return 1
    mark_green tidy-public-header
    gate_patch_append include/lws/options.hpp '#include <vector>'
    gate_patch_append include/lws/options.hpp \
        'inline bool lws_seed_empty(const std::vector<std::int32_t>& values) { return values.size() == 0; }'
    gate_write src/core/zz_seed_tidy_public.cpp '#include "lws/options.hpp"'
    step cfg-tidy-public-seeded -- cmake --preset "$db" || rc=1
    gate_expect_red tidy-public-header 'include/lws/options.hpp' -- scripts/tidy.sh --all || rc=1
    gate_restore_now || rc=1
    step cfg-tidy-public-restored -- cmake --preset "$db" || rc=1
    ((rc == 0)) || return 1
    mark_red tidy-public-header
}

# SC5, T-02-10: the include scan. The seed is a core source that includes <thread>, named so that the
# refusal line carries it. The scan is a text scan, so nothing is compiled.
proof_core_isolation() {
    local rc=0
    gate_expect_green isolation -- cmake "-DLWS_ROOT=$LWS_GATE_ROOT" \
        -P "$LWS_GATE_ROOT/scripts/core-isolation-gate.cmake" || return 1
    mark_green core-isolation
    gate_write src/core/zz_seed_isolation.cpp '#include <thread>'
    red_with isolation '[isolation]' 'zz_seed_isolation' -- cmake "-DLWS_ROOT=$LWS_GATE_ROOT" \
        -P "$LWS_GATE_ROOT/scripts/core-isolation-gate.cmake" || rc=1
    gate_restore_now || return 1
    ((rc == 0)) || return 1
    mark_red core-isolation
}

# SC5, T-02-10: the link closure. The seed links pthread into lws_core from tests/CMakeLists.txt. The
# closure walk is deferred, so it must see the seed after the reconfigure; the red run also needs the name
# pthread in its log, which fails the proof if the walk ran before the seed.
proof_core_closure() {
    local rc=0
    prepare release || return 1
    gate_expect_green closure -- ctest --preset release -R '^core_link_closure$' || return 1
    mark_green core-closure
    gate_patch_append tests/CMakeLists.txt 'target_link_libraries(lws_core PRIVATE pthread)'
    step cfg-closure-seeded -- cmake --preset release || rc=1
    if ((rc == 0)); then
        red_with closure '[isolation]' 'pthread' -- ctest --preset release -R '^core_link_closure$' \
            --output-on-failure || rc=1
    fi
    gate_restore_now || rc=1
    step cfg-closure-restored -- cmake --preset release || rc=1
    ((rc == 0)) || return 1
    mark_red core-closure
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
