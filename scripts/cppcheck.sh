#!/usr/bin/env bash
# cppcheck gate over the repo-owned translation units of the asan-ubsan compile database.
#
# Owned means what scripts/tidy.sh means: not under third_party/ or build/, and inside the repository.
# The owned entries are written to build/$id/logs/cppcheck-db.json and that file is the --project, so
# the units cppcheck checks and the units this gate expects are the same set.
#
# Exit: 0 clean, 1 findings, 2 usage, no compile database, or an empty sweep, 3 not every owned unit
# was analysed (an internal error, or a unit missing from cppcheck's "Checking" output).
set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

id=${LWS_BUILD_ID:-gate}
preset=${LWS_DB_PRESET:-asan-ubsan}
db="build/$id/$preset/compile_commands.json"
logs="build/$id/logs"
[[ $# -eq 0 ]] || { echo "usage: scripts/cppcheck.sh" >&2; exit 2; }
[[ -f $db ]] || {
    echo "cppcheck: no $db; configure and build the $preset preset first (LWS_BUILD_ID=$id)" >&2
    exit 2
}

mkdir -p "$logs"
log="$logs/cppcheck.log"
filtered="$logs/cppcheck-db.json"

# Writes the owned entries to $filtered; exits 2 on an empty sweep.
python3 - "$db" "$root" "$filtered" <<'PY'
import json, os, sys
db, root, out = sys.argv[1], sys.argv[2], sys.argv[3]
excluded = {"third_party", "build", ".."}
entries = []
for entry in json.load(open(db, encoding="utf-8")):
    rel = os.path.relpath(entry["file"], root)
    if rel.split(os.sep, 1)[0] in excluded:
        continue
    entries.append(entry)
if not entries:
    print("cppcheck: no owned translation unit in the database: the sweep would be empty", file=sys.stderr)
    sys.exit(2)
with open(out, "w", encoding="utf-8") as handle:
    json.dump(entries, handle, indent=1)
PY

# Findings and internal errors are read from the log by the python pass below, so cppcheck's own
# exit status is not the decision here.
cppcheck --project="$filtered" --std=c++23 \
    --enable=warning,style,performance,portability \
    --inline-suppr --library=googletest --suppress=missingIncludeSystem \
    --template='{file}:{line}: {severity}: {message} [{id}]' >"$log" 2>&1 || true

python3 - "$filtered" "$root" "$log" <<'PY'
import json, os, re, sys
filtered, root, log_path = sys.argv[1], sys.argv[2], sys.argv[3]
expected = {os.path.relpath(e["file"], root) for e in json.load(open(filtered, encoding="utf-8"))}
lines = open(log_path, encoding="utf-8", errors="replace").read().splitlines()

checked = set()
for line in lines:
    if line.startswith("Checking "):
        name = re.split(r"(?: \.\.\.|: |\s)", line[len("Checking "):], maxsplit=1)[0]
        checked.add(os.path.relpath(name, root))

broken = [l for l in lines if re.search(r"syntaxError|internalAstError|cppcheckError", l)]
missing = sorted(expected - checked)
if broken or missing:
    for l in broken[:20]:
        print(l, file=sys.stderr)
    if missing:
        print("cppcheck: not analysed: " + ", ".join(missing[:20]), file=sys.stderr)
    print(f"cppcheck: {len(broken)} internal error(s), {len(missing)} unit(s) not analysed", file=sys.stderr)
    sys.exit(3)

findings = [l for l in lines if re.search(r"\[[A-Za-z]+\]$", l)]
for l in findings:
    print(l)
if findings:
    print(f"cppcheck: {len(findings)} finding(s) in {len(expected)} unit(s)", file=sys.stderr)
    sys.exit(1)
print(f"CPPCHECK-OK units={len(expected)} checked={len(checked & expected)} findings=0")
PY
