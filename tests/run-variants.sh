#!/usr/bin/env bash
# Regression over parameter variants: every line of tests/variants/*.txt is
#     PASS  scenario-name  key=value ...
# (or FAIL when a failing expectation is the documented outcome). Each line is
# run headless with seeds 1..5 and must give that result every time.
#
#   tests/run-variants.sh [file.txt ...]     exit status 1 if any line disagrees
#   SEEDS="1 2" tests/run-variants.sh        fewer seeds
cd "$(dirname "$0")/.." || exit 2
files=("$@")
[ ${#files[@]} -eq 0 ] && files=(tests/variants/*.txt)
bad=0
total=0
for f in "${files[@]}"; do
    while read -r want name args; do
        case "$want" in PASS|FAIL) ;; *) continue ;; esac
        total=$((total + 1))
        for seed in ${SEEDS:-1 2 3 4 5}; do
            # shellcheck disable=SC2086
            out=$(./netlab run "$name" $args --quiet --seed "$seed" --lang en 2>&1 | tail -1)
            got=${out%% *}
            if [ "$got" != "$want" ]; then
                echo "MISMATCH ($f) want $want, seed $seed: $name $args  ->  $out"
                bad=$((bad + 1))
                break
            fi
        done
    done < "$f"
done
echo "$total variants, $bad mismatching"
[ "$bad" -eq 0 ]
