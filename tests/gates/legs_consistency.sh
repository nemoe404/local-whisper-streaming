#!/usr/bin/env bash
# tests/gates/legs_consistency.sh: the four places that name the ctest legs must name the same set.
# No arguments.
#
#   scripts/check.sh                the single `legs=(...)` line
#   CMakePresets.json               the non-hidden configure presets not marked hand-run
#                                   (tests/gates/preset_legs.py)
#   AGENTS.md                       the line `ctest legs: a, b, c` (local-only; path from LWS_LEGS_AGENTS_MD,
#                                   default {root}/AGENTS.md; the variable is a test seam for the red proof)
#   measurements/ctest-legs.md      the line `Decision: legs = a, b, c` (local-only)
#
# The LWS_DB_PRESET defaults in scripts/tidy.sh and scripts/cppcheck.sh must also be members of the set: a
# dropped leg must not leave an analyser pointing at a directory no leg configures.
#
# Exit: 0 consistent (LEGS-CONSISTENT legs={list}); 1 the sets differ or a default is not a leg (LEGS-DIFFER,
# every set printed); 2 a source cannot be parsed, or the decision record is absent; 77 AGENTS.md is absent
# (it is local-only, so a fresh clone has none; run.sh reports that as NOT RUN, never as a pass).
set -euo pipefail

if (($# != 0)); then
    echo "legs_consistency: takes no arguments" >&2
    exit 2
fi

root=$(git rev-parse --show-toplevel)
cd "$root"

agents=${LWS_LEGS_AGENTS_MD:-$root/AGENTS.md}
if [[ ! -f $agents ]]; then
    echo "legs_consistency: $agents is absent (local-only file); nothing to compare" >&2
    exit 77
fi

exec python3 -I - "$root" "$agents" <<'PY'
import re
import subprocess
import sys

root, agents = sys.argv[1], sys.argv[2]


def fail(message):
    print(f"legs_consistency: {message}", file=sys.stderr)
    sys.exit(2)


def read(path):
    try:
        with open(path, encoding="utf-8") as handle:
            return handle.read()
    except OSError as error:
        fail(f"cannot read {path}: {error}")


def only(pattern, text, what, flags=re.M):
    found = re.findall(pattern, text, flags)
    if len(found) != 1:
        fail(f"{what}: expected exactly one match, found {len(found)}")
    return found[0]


def names(text):
    items = [item.strip() for item in re.split(r"[,\s]+", text.strip()) if item.strip()]
    if not items or not all(re.fullmatch(r"[a-z0-9][a-z0-9-]*", item) for item in items):
        fail(f"not a list of leg names: {text!r}")
    return frozenset(items)


check_text = read(f"{root}/scripts/check.sh")
check_legs = names(only(r"^legs=\(([^)]*)\)$", check_text, "scripts/check.sh legs=(...) line"))

helper = f"{root}/tests/gates/preset_legs.py"
listed = subprocess.run(
    [sys.executable, "-I", helper, f"{root}/CMakePresets.json"], capture_output=True, text=True, check=False
)
if listed.returncode != 0:
    fail(f"preset_legs.py failed: {listed.stderr.strip()}")
preset_legs = frozenset(listed.stdout.split())
if not preset_legs:
    fail("CMakePresets.json has no configure preset that is a leg")

agent_legs = names(
    only(
        r"ctest legs:\s*([a-z0-9][a-z0-9-]*(?:\s*,\s*[a-z0-9][a-z0-9-]*)*)",
        read(agents),
        "AGENTS.md `ctest legs:` line",
    )
)

decision_legs = names(
    only(r"^Decision: legs = (.+)$", read(f"{root}/measurements/ctest-legs.md"), "decision line")
)

defaults = {}
for script in ("scripts/tidy.sh", "scripts/cppcheck.sh"):
    defaults[script] = only(
        r"^preset=\$\{LWS_DB_PRESET:-([a-z0-9-]+)\}$", read(f"{root}/{script}"), f"{script} LWS_DB_PRESET default"
    )

sets = {
    "scripts/check.sh": check_legs,
    "CMakePresets.json": preset_legs,
    "AGENTS.md": agent_legs,
    "measurements/ctest-legs.md": decision_legs,
}
equal = len(set(sets.values())) == 1
members = all(value in check_legs for value in defaults.values())

if equal and members:
    print("LEGS-CONSISTENT legs=" + ",".join(sorted(check_legs)))
    sys.exit(0)

print("LEGS-DIFFER")
for source, legs in sets.items():
    print(f"  {source}: {','.join(sorted(legs))}")
for script, value in defaults.items():
    verdict = "member" if value in check_legs else "NOT A LEG"
    print(f"  {script} LWS_DB_PRESET default: {value} ({verdict})")
sys.exit(1)
PY
