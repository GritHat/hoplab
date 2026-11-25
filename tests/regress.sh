#!/usr/bin/env bash
# Regression suite. Run from the repository root after `make`:
#
#   tests/regress.sh            everything
#   tests/regress.sh quick      catalog with seed 1 and the variants with seed 1
#
#  1. every lab of the catalog, headless, seeds 1..5: its `expect` lines hold
#  2. the parameter variants in tests/variants/*.txt, seeds 1..5. A line is
#         PASS|FAIL  lab-name  key=value ...
#     and states the outcome guaranteed for those parameter values
#  3. the same catalog under AddressSanitizer and UBSan (seed 2)
#  4. the multi-process mode: one lab split over three processes joined by
#     UDP tunnels, with the start-up rendezvous
#  5. the classic demos: a real transfer over UDP for each protocol
#  6. translations: no missing or stale entry in locale/it; every guide
#     topic exists in both languages and names labs that exist
set -u
cd "$(dirname "$0")/.."
BIN=./hoplab
QUICK=${1:-}
SEEDS="1 2 3 4 5"; [ "$QUICK" = quick ] && SEEDS=1
fail=0; total=0
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
note() { printf '%s\n' "$*"; }
bad()  { printf 'FAIL  %s\n' "$*"; fail=$((fail + 1)); }

note "== 1. catalog (seeds: $SEEDS)"
for f in scenarios/*.nl; do
    for s in $SEEDS; do
        total=$((total + 1))
        "$BIN" run "$f" --quiet --seed "$s" >"$TMP/out" 2>&1 || bad "$(basename "$f") seed $s: $(tail -1 "$TMP/out")"
    done
done

note "== 2. parameter variants"
for v in tests/variants/*.txt; do
    [ -e "$v" ] || continue
    while read -r want lab params; do
        case "$want" in PASS|FAIL) ;; *) continue ;; esac
        for s in $SEEDS; do
            total=$((total + 1))
            # shellcheck disable=SC2086
            if "$BIN" run "$lab" $params --quiet --seed "$s" >"$TMP/out" 2>&1; then got=PASS; else got=FAIL; fi
            grep -q "scenario not found\|: unknown\|usage:" "$TMP/out" && got=ERROR
            [ "$got" = "$want" ] || bad "$(basename "$v"): $lab $params seed $s: expected $want, got $got"
        done
    done <"$v"
done

if [ "$QUICK" != quick ]; then
    note "== 3. sanitizers"
    if gcc -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -D_DEFAULT_SOURCE -I. -Isrc \
           src/*.c rdt.c channel.c tui.c i18n.c -lm -o "$TMP/san" 2>"$TMP/cc"; then
        for f in scenarios/*.nl; do
            total=$((total + 1))
            ASAN_OPTIONS=detect_leaks=0 HOPLAB_HOME=$PWD HOPLAB_HOME=$PWD "$TMP/san" run "$f" --quiet --seed 2 >"$TMP/out" 2>&1 ||
                bad "sanitizer run of $(basename "$f")"
            grep -q "runtime error\|AddressSanitizer" "$TMP/out" && bad "sanitizer report in $(basename "$f"): $(grep -m1 'runtime error\|ERROR' "$TMP/out")"
        done
    else
        bad "sanitizer build: $(head -3 "$TMP/cc")"
    fi

    note "== 4. three processes over UDP tunnels"
    port=$((30000 + $$ % 2000 * 10))
    "$BIN" node 101-ping h2 --headless --baseport $port --monitor 127.0.0.1:$((port - 1)) --until 13s --quiet --seed 1 >"$TMP/h2" 2>&1 &
    "$BIN" node 101-ping h1 --headless --baseport $port --monitor 127.0.0.1:$((port - 1)) --until 13s --quiet --seed 1 >"$TMP/h1" 2>&1 &
    sleep 0.5
    "$BIN" node 101-ping r1 --hub --headless --baseport $port --until 13s --quiet --seed 1 >"$TMP/r1" 2>&1
    wait
    total=$((total + 1))
    grep -q '^PASS' "$TMP/h1" || bad "distributed 101-ping: $(tail -1 "$TMP/h1")"

    note "== 5. classic demos over real UDP"
    for p in stop_and_wait go_back_n selective_repeat; do
        total=$((total + 1))
        port=$((port + 7))
        ./${p}_recv -p $port -d 30 -l 0.15 -c 0.1 -S 1 >"$TMP/r" 2>&1 &
        rpid=$!
        sleep 0.3
        timeout 60 ./${p}_send -p $port -d 30 -l 0.15 -c 0.1 -S 2 -m "reliable over unreliable" >"$TMP/s" 2>&1
        sleep 0.5
        kill $rpid 2>/dev/null; wait $rpid 2>/dev/null
        grep -q "reliable over unreliable" "$TMP/r" || bad "classic $p: text not delivered"
    done
fi

note "== 6. translations and guide"
total=$((total + 1))
python3 tools/extract-strings.py --check locale/it >"$TMP/out" 2>&1 || bad "locale/it: $(tail -1 "$TMP/out")"
for f in guide/en/*.md; do
    total=$((total + 1))
    [ -e "guide/it/$(basename "$f")" ] || bad "guide/it/$(basename "$f") is missing"
    for lab in $(grep -m1 'labs:' "$f" | sed 's/.*labs://; s/-->//; s/,/ /g'); do
        [ -e "scenarios/$lab.nl" ] || bad "$(basename "$f") names a lab that does not exist: $lab"
    done
done

note ""
note "$total checks, $fail failed"
[ "$fail" -eq 0 ]
