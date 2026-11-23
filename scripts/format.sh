#!/usr/bin/env bash
# clang-format gate over every C++ source the tree owns: tracked files plus new untracked files
# that git does not ignore. third_party/ is not ours and is never checked.
#
# Exit: 0 clean (prints FORMAT-OK files=N); 1 unformatted files (prints one `not clang-formatted`
# line per file and the fix line); 2 misuse or an empty or broken sweep (refused, never a pass).
set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

clang_format=${CLANG_FORMAT:-clang-format-21}

mapfile -t listed < <(git ls-files --cached --others --exclude-standard -- '*.cpp' '*.hpp' '*.h')
sources=()
for path in "${listed[@]}"; do
    case "$path" in
        third_party/*) continue ;;
    esac
    sources+=("$path")
done

if ((${#sources[@]} == 0)); then
    echo "format: no C++ sources listed; refusing to pass an empty sweep" >&2
    exit 2
fi

unformatted=()
for path in "${sources[@]}"; do
    if [[ ! -f $path ]]; then
        echo "format: listed path does not exist (not a skip): $path" >&2
        exit 2
    fi
    status=0
    "$clang_format" --dry-run -Werror "$path" >/dev/null 2>&1 || status=$?
    case "$status" in
        0) ;;
        1) unformatted+=("$path") ;;
        *)
            echo "format: $clang_format failed on $path (exit $status); not a formatting verdict" >&2
            exit 2
            ;;
    esac
done

if ((${#unformatted[@]} > 0)); then
    for path in "${unformatted[@]}"; do
        echo "format: not clang-formatted: $path"
    done
    echo "$clang_format -i ${unformatted[*]}"
    exit 1
fi

echo "FORMAT-OK files=${#sources[@]}"
