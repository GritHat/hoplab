# Tests

| Command | What it does |
|---|---|
| `make test` (`./netlab check`) | every lab of the catalog, headless, seed 1, with its `expect` lines |
| `tests/run-variants.sh [FILE...]` | every line of `tests/variants/*.txt`, seeds 1 to 5: labs run with the parameter values a student would try, and must still behave as their description says |
| `tests/run-sanitized.sh` | every lab and every variant under AddressSanitizer and UBSan (builds `./netlab-asan`) |
| `tests/try.sh LAB "k=v ..." ...` | quick look at a few variants of one lab: one line of P/F per variant, five seeds |
| `tests/counters.sh LAB "node:counter ..." [k=v ...]` | print the final value of counters for one run, to design `expect` and `check` lines |

A variant file is a list of lines

```
PASS  scenario-name  key=value key=value
```

`FAIL` is accepted for the rare case in which a failing expectation is the
documented outcome; there is none at present. See "Expectations that depend
on parameters" in `docs/scenario-format.md` for how labs state what they
should show for any value of their parameters.
