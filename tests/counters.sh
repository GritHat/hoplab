#!/usr/bin/env bash
# Print the final value of counters for one run, to design expectations:
#
#   tests/counters.sh SCENARIO "node:counter node:counter ..." [key=value ...] [--seed N]
#
# It runs a temporary copy of the scenario with extra `expect` lines and
# shows what each counter was ("got").
cd "$(dirname "$0")/.." || exit 2
name=$1
list=$2
shift 2
src=$(ls scenarios/"$name"*.nl 2>/dev/null | head -1)
[ -f "$name" ] && src=$name
[ -n "$src" ] || { echo "no scenario $name" >&2; exit 2; }
tmp=$(mktemp "${TMPDIR:-/tmp}/nlc.XXXXXX.nl") || exit 2
grep -v '^expect ' "$src" > "$tmp"
for item in $list; do
    echo "expect ${item%%:*} ${item#*:} >= -1e18" >> "$tmp"
done
./hoplab run "$tmp" "$@" --headless --detail 1 --lang en | grep -E '^  (ok|FAIL) ' | sed 's/ >= -1e+18//; s/^  ok  *//; s/(got /= /; s/)$//' | tr '\n' ';'
echo
rm -f "$tmp"
