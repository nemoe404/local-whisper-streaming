# scripts/lib/skips.sh: the one home of the skip helpers. Sourced, never executed. It sets no shell
# options of its own, so it runs under the caller's `set -euo pipefail`.
#
# ctest exits 0 when a test is skipped, so the count of `***Skipped` lines in a ctest log is the
# only signal that a skip happened. check.sh and 01-04's tests/gates/lib.sh both source this file,
# so the two can never count differently. 01-04's tests/gates/skip_red.sh uses lws_has_skip_marker
# for its per-test presence check, so no gate script greps the skip marker itself.

# lws_count_skips {ctest-log}
# Prints the number of `***Skipped` lines and returns 0. grep's exit 1 (no match) prints 0.
# Any other grep status (missing or unreadable log) prints nothing on stdout and returns 2:
# a failed read is never reported as zero skips.
lws_count_skips() {
    if (($# != 1)); then
        echo "lws_count_skips: usage: lws_count_skips {ctest-log}" >&2
        return 2
    fi
    local count status=0
    count=$(grep -c -F '***Skipped' -- "$1") || status=$?
    case "$status" in
        0) printf '%s\n' "$count" ;;
        1) printf '0\n' ;;
        *)
            echo "a skip is never a pass: log unreadable ($1, grep exit $status)" >&2
            return 2
            ;;
    esac
}

# lws_has_skip_marker {ctest-log} {test-name}
# A presence check, not a count. Returns 0 when one line of the log holds both the test name and
# `***Skipped`, 1 when no line does, and 2 when the log is not a readable regular file.
# Fixed-string matching (awk index(), the name passed through ENVIRON so no escape processing),
# so a test name with dots needs no regex escaping.
lws_has_skip_marker() {
    if (($# != 2)) || [[ -z "$2" ]]; then
        echo "lws_has_skip_marker: usage: lws_has_skip_marker {ctest-log} {non-empty test-name}" >&2
        return 2
    fi
    local log=$1 status=0
    if [[ ! -f "$log" || ! -r "$log" ]]; then
        echo "a skip is never a pass: log unreadable ($log)" >&2
        return 2
    fi
    LWS_SKIP_NAME=$2 awk '
        index($0, ENVIRON["LWS_SKIP_NAME"]) && index($0, "***Skipped") { found = 1; exit }
        END { exit(found ? 0 : 1) }
    ' "$log" || status=$?
    case "$status" in
        0 | 1) return "$status" ;;
        *)
            echo "a skip is never a pass: log unreadable ($log, awk exit $status)" >&2
            return 2
            ;;
    esac
}
