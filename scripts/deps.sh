#!/usr/bin/env bash
# The one Conan install command. check.sh, the 01-03 measurement script and the 01-04 seed harness
# all call this script, so the command cannot drift between them.
#
# Takes exactly one argument, Debug or Release. Anything else exits 2.
set -euo pipefail

if (($# != 1)) || [[ $1 != Debug && $1 != Release ]]; then
    echo "usage: scripts/deps.sh Debug|Release" >&2
    exit 2
fi

root=$(git rev-parse --show-toplevel)
cd "$root"

id=${LWS_BUILD_ID:-gate}
logs="$root/build/$id/logs"
# Create the directory before anything redirects into it: on a fresh tree neither build/ nor
# build/$id exists, and a redirect into a missing directory fails before conan starts.
mkdir -p "$logs"
log="$logs/conan-$1.log"

status=0
conan install . --output-folder="$root/build/$id/conan" --profile:all=profiles/clang-21 \
    -s "&:build_type=$1" --build=missing >"$log" 2>&1 || status=$?
if ((status != 0)); then
    echo "deps: conan install ($1) failed (exit $status); see $log" >&2
    exit 1
fi
echo "DEPS-OK build_type=$1 id=$id"
