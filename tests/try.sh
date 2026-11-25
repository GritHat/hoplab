#!/usr/bin/env bash
# Try parameter variants of one scenario over seeds 1..5, one compact line each:
#
#   tests/try.sh 231-rip-count-to-infinity "" split=1 "split=1 poison=1"
#
# P = expectations hold, F = they do not, E = error.
cd "$(dirname "$0")/.." || exit 2
name=$1
shift
src=$(ls scenarios/"$name"*.nl 2>/dev/null | head -1)
[ -n "$src" ] || { echo "no scenario $name" >&2; exit 2; }
[ $# -eq 0 ] && set -- ""
for v in "$@"; do
    row=""
    for seed in ${SEEDS:-1 2 3 4 5}; do
        # shellcheck disable=SC2086
        out=$(./netlab run "$src" $v --quiet --seed "$seed" --lang en 2>&1 | tail -1)
        case "$out" in PASS*) row+=P ;; FAIL*) row+=F ;; *) row+=E ;; esac
    done
    printf '%s  %s\n' "$row" "${v:-(defaults)}"
done
