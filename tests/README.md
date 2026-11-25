# Tests

| Command | What it does |
|---|---|
| `make test` (`./hoplab check`) | every lab of the catalog, headless, seed 1, with its `expect` lines |
| `make regress` (`tests/regress.sh`, or `tests/regress.sh quick`) | every lab with seeds 1 to 5; every line of `tests/variants/*.txt` (labs run with the parameter values a student would try, which must still behave as their description says); the catalog under AddressSanitizer and UBSan; one lab split over three processes joined by UDP tunnels; a real transfer with each classic demo; translation and guide consistency |
| `tests/try.sh LAB "k=v ..." ...` | quick look at a few variants of one lab: one line of P/F per variant, five seeds |
| `tests/counters.sh LAB "node:counter ..." [k=v ...]` | print the final value of counters for one run, to design `expect` and `check` lines |

A variant file is a list of lines

```
PASS  scenario-name  key=value key=value
```

There is no `FAIL`: a lab must hold for every value a student would give its
parameters, so its expectations describe each configuration (`expect ... if
$param == value`, or the `check` module for numeric conditions). A `FAIL` line
in a variants file is reported as an error. See "Expectations that depend
on parameters" in `docs/scenario-format.md`.