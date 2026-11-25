#!/usr/bin/env bash
# Regression of parameterised runs: every line of tests/variants/*.txt is
#   PASS|FAIL  scenario  key=value ...
# and must give that outcome with each of the seeds (default 1..5).
#   tools/check-variants.sh [FILE ...]        SEEDS="1 2 3" to change the seeds
cd "$(dirname "$0")/.." || exit 2
files=("$@")
[ ${#files[@]} -eq 0 ] && files=(tests/variants/*.txt)
bad=0 total=0
for f in "${files[@]}"; do
    while read -r want scen params; do
        case "$want" in PASS|FAIL) ;; *) continue ;; esac
        for seed in ${SEEDS:-1 2 3 4 5}; do
            total=$((total + 1))
            # shellcheck disable=SC2086
            got=$(./netlab run "$scen" $params --quiet --seed "$seed" 2>&1 | tail -1 | cut -c1-4)
            if [ "$got" != "$want" ]; then
                bad=$((bad + 1))
                echo "MISMATCH  $scen $params  seed $seed: expected $want, got ${got:-nothing}"
            fi
        done
    done < "$f"
done
echo "$total runs, $bad mismatches"
[ "$bad" -eq 0 ]
