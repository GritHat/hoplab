#!/usr/bin/env bash
# Run scenarios and variants under AddressSanitizer / UBSan.
#
#   tests/run-sanitized.sh            every lab with defaults, and every variant line (seed 1)
#
# Builds ./netlab-asan if it is missing. Prints only the runs that report a
# memory or undefined-behaviour error; exit status 1 if there is any.
cd "$(dirname "$0")/.." || exit 2
if [ ! -x ./netlab-asan ]; then
    gcc -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -D_DEFAULT_SOURCE \
        -I. -Isrc src/*.c rdt.c channel.c tui.c i18n.c -lm -o netlab-asan || exit 2
fi
export ASAN_OPTIONS=detect_leaks=0
bad=0
total=0
run() {
    total=$((total + 1))
    # shellcheck disable=SC2086
    out=$(./netlab-asan run "$@" --quiet --seed 1 --lang en 2>&1)
    if printf '%s' "$out" | grep -q -E 'ERROR: AddressSanitizer|runtime error'; then
        bad=$((bad + 1))
        echo "SANITIZER: $*"
        printf '%s\n' "$out" | grep -E 'ERROR: AddressSanitizer|runtime error|SUMMARY' | sort | uniq -c | head -5
    fi
}
for f in scenarios/*.nl; do
    run "$f"
done
for f in tests/variants/*.txt; do
    while read -r want name args; do
        case "$want" in PASS|FAIL) ;; *) continue ;; esac
        [ -n "$args" ] || continue
        # shellcheck disable=SC2086
        run "$name" $args
    done < "$f"
done
echo "$total runs, $bad with sanitizer errors"
[ "$bad" -eq 0 ]
