# Development

How to build, check and debug TaxBrokerReport. The Docker setup itself is described in
[docker/docker_doc.md](../docker/docker_doc.md); how to contribute is in
[CONTRIBUTING.md](../CONTRIBUTING.md). Logging is described in [logging.md](logging.md).

## Workflow

The C++20 build requires Boost headers for checked 256-bit exact arithmetic. The development Docker
image installs `boost-dev`; production needs no additional Boost runtime library.

Run `scripts/format.sh` before opening a pull request. To make Git reject unformatted commits and pushes locally, run `scripts/install_hooks.sh` once; it wires the repo-local hooks in `.githooks/` to `scripts/format.sh --check`.

The `format-check` job of the `CI` workflow runs `scripts/format.sh --check` on every PR to `main`,
so unformatted code will fail CI and should be fixed before merge.

The `CI` GitHub Actions workflow runs on every pull request to `main` and after changes reach
`main`. It builds the development image once and compiles the Debug backend once. The tests,
Valgrind and Cppcheck jobs load that build and compile nothing, so the failing category stays
visible. The jobs and timings are described in [docker/docker_doc.md](../docker/docker_doc.md). The
`main` ruleset **Required CI checks** requires **Dev image**, **Development build**, **Production
image**, **Required tests**, **Valgrind**, **Cppcheck**, **format-check**, **Coverage (current)**,
**Coverage (baseline)** and **coverage**. **Required issue reference** is a separate workflow; it
requires the pull request's `Related issue` section to contain an issue reference or exactly `N/A`.

## Build scripts

Every script that builds or checks the code starts from an empty build folder, so a local pass
means a CI pass. Each script owns one folder below `build/` and deletes only that one: `build/dev`
(`build.sh dev`, `test.sh`, `valgrind.sh`, `run.sh`), `build/cppcheck`, `build/debug-tools` (dump
scripts) and `build/coverage`. `debug_tests.sh` stays incremental in `build/test-debug`.

```sh
scripts/build.sh image          # build only the development image (docker-build also does)
scripts/build.sh dev            # clean Debug build of build/dev
scripts/test.sh --reuse-build   # use the existing build/dev, compile nothing
```

A script that needs the image stops with a hint when it is missing. `test.sh`, `valgrind.sh` and
`cppcheck.sh` accept `--reuse-build`, which uses the existing `build/dev` as it is and fails clearly
when it is missing. CI uses the same flag in the jobs that follow the build job.

Options and paths can come in any order, so `scripts/report.sh runtime/export.csv --self-report`
works. For Tab completion of the script options, years and paths in bash or zsh, add this line to
`~/.bashrc` or `~/.zshrc` (in zsh after `compinit`):

```sh
source /path/to/TaxBrokerReport/scripts/completion.bash
```

## Static analysis

Run Cppcheck against all active project-owned C++ source and header files:

```sh
scripts/cppcheck.sh
```

The analysis uses CMake's compile database to cover `src/`, `tests/`, `tools/`, and all headers they
include. It also checks every file in `include/` directly so currently unreferenced headers are not
missed. Third-party dependencies and the deprecated `legacy-QT-GUI/` tree are intentionally
excluded. Cppcheck findings fail the `Cppcheck` job of the `CI` workflow.

The script configures `build/cppcheck` from scratch to create the compile database. With
`scripts/cppcheck.sh --reuse-build` it uses the compile database of the existing `build/dev`
instead, as the CI job does.

## Memory checks

Run every C++ executable under Valgrind inside the development container:

```sh
scripts/valgrind.sh
```

The script builds `build/dev` from scratch and runs the unit tests, integration tests, backend
server, and the Trade Republic and exact-arithmetic dump tools from that one build. The dump tools
use synthetic test data. Valgrind reports invalid memory access and fails on definite or indirect
memory leaks. Each test executable is started once to avoid Valgrind startup overhead for every
test discovered by CTest. With `scripts/valgrind.sh --reuse-build` it runs the existing `build/dev`
as it is and compiles nothing.

The `Valgrind` job of the `CI` workflow runs the same memory checks for pull requests and after
changes reach `main`, on the build that the `Development build` job made.

## Test suites

Unit and integration tests live in `tests/unit/` and `tests/integration/`, respectively, and use
separate executables. Every discovered CTest test has its corresponding `unit` or `integration`
label. Run both suites, one suite, or a filtered selection:

```sh
scripts/test.sh
scripts/test.sh --suite unit
scripts/test.sh --suite integration
scripts/test.sh --suite integration StatementMergerIntegrationTest
```

`scripts/test.sh` builds `build/dev` from scratch first. Add `--reuse-build` (for example
`scripts/test.sh --reuse-build --suite unit`) to run the existing `build/dev` as it is without
compiling.

Inside the development container, `ctest --test-dir /workspace/build/dev -L unit` and
`ctest --test-dir /workspace/build/dev -L integration` select the same suites. CI runs them in
separate steps on the shared build, including integration tests after a unit-test failure when the
build was loaded. The **Required tests** check still requires both suites to pass.

## Debug tests in VS Code

Open this repository locally in VS Code, install Microsoft's **C/C++** extension, and use the
**Docker: debug unit tests** or **Docker: debug integration tests** profile. The build, GDB, and test
process all run inside the existing development Docker image. See the
[debugging guide](debugging.md) for breakpoints, stepping, and inspecting variables.

## Test coverage

Run the test suite with coverage instrumentation inside the development container:

```sh
scripts/coverage.sh
```

Each run deletes `build/coverage` and builds it from scratch, so removed or stashed sources cannot
leak into the reports. The script needs the development image and never builds it.

The script keeps its instrumented build separate from the normal development build and writes:

- an HTML report to `coverage/html/index.html`;
- the machine-readable LCOV report to `coverage/lcov.info`;
- a short summary and exact line-coverage metrics to `coverage/summary.txt` and
  `coverage/metrics.env`.

The same reports are also written to `coverage/unit/` and `coverage/integration/` for each suite.
Counters are reset between suites, and all three reports use the same production-code denominator
(`include/` and `src/`, excluding tests and dependencies). Combined coverage is the union of covered
lines, not an average or sum of the two suite percentages.

The `Coverage (current)` and `Coverage (baseline)` jobs of the `CI` workflow measure the pull-request
commit and its exact base commit in parallel, and the `coverage` job compares them. Its job
summary and pull-request comment show unit, integration, and combined coverage against the base.
All three HTML reports are available in one artifact. Only a decrease in combined coverage triggers
the existing warning; it does not currently block merging.

## Debug dumps

Parser warnings and errors intended for the local browser UI are not extracted from log text. Each
parse returns structured diagnostics, and the API layer can serialize them using the versioned JSON
contract documented in [architecture.md](architecture.md). To inspect the current Trade
Republic parser output and diagnostics locally, run:

```sh
scripts/dump_tr_parse.sh
```

Generated debug files are written below `runtime/`, which is ignored by Git.

To inspect splits from a local Trade Republic export:

```sh
scripts/dump_exact_arithmetic.sh
# Or supply another repository-relative CSV:
scripts/dump_exact_arithmetic.sh tmp/another-export.csv
# After checking a split's CSV row and verified new/old share counts:
scripts/dump_exact_arithmetic.sh --split-row ROW NEW_SHARES OLD_SHARES
```

The default input is `tmp/TransactionExport.csv`; output stays in the ignored
`runtime/debug/exact_arithmetic.txt` and is also printed in the terminal. Only scripts, helpers and
synthetic fixtures are committed.
The dump shows only instruments with split rows: split dates and source rows, factor comparisons,
units before/after the split, and each open purchase lot's exact cost and readable price per unit.
Trades are read internally to reconstruct the position before each split; purchase and sale
transactions and unrelated instruments are not printed. TR's exported split quantity field is shown as evidence, not interpreted
as a verified ratio. The tool shows separate what-if previews for two readings of that field
(added units or total units after the action), plus an imaginary 1 new / 2 old comparison. These
previews do not change the inspected position or establish the real factor. Supply `--split-row` for a local preview; without it, the position after that
action remains unresolved. Non-EUR trades also remain unresolved until verified conversion inputs
are supported. This tool previews arithmetic; full action validation and tax reporting remain later
processor work. Use `--output runtime/path.txt` to choose another local output file.

The Trade Republic parse/merge dumps also retain each quantity source text and discarded digits
in their local text output.

To split a local Trade Republic export into three inputs and inspect the merged result, run:

```sh
scripts/dump_tr_merge.sh
```

The script writes the split CSVs below `runtime/debug/tr_merge_inputs/` and the human-readable
merged result to `runtime/debug/tr_merged_debug.txt`. The tracked script accepts alternate relative
paths; generated financial data must remain below the ignored `runtime/` directory.

For a separate text file containing only parser and merger diagnostics, run:

```sh
scripts/dump_tr_diagnostics.sh
```

The default output is `runtime/diagnostics/tr_merge_diagnostics.txt`, also ignored by Git.
To inspect the same three files used by the merge dump, supply them in request order:

```sh
scripts/dump_tr_diagnostics.sh runtime/debug/tr_merge_inputs/tr_merge_part_1.csv \
    runtime/debug/tr_merge_inputs/tr_merge_part_2.csv \
    runtime/debug/tr_merge_inputs/tr_merge_part_3.csv
```

Use `--output runtime/path.txt` before the CSV paths to choose another local output file.

To inspect FIFO matching for one year, run:

```sh
scripts/dump_tr_fifo.sh --year 2024
```

The default year is the previous calendar year, the default input is `tmp/TransactionExport.csv`
and the default output is `runtime/debug/tr_fifo_<year>.txt`. Several CSVs can be supplied in
request order, as for the diagnostics dump, and `--output runtime/path.txt` chooses another file.
The output starts with counts per instrument status and the number of loss-making matches, then
lists each instrument's status, the event it stopped at, its in-year matches (units, basis, sale
value, gain) and its open lots at 31 December. Amounts are exact values shown rounded to eight
decimals, without fees. After the instruments, the file shows the Doh-KDVP filing rows (`F1` to
`F10`, with `F8` the running stock), each blocked ISIN with its reason and the sales it names, and
the units left out because they are past the holding period. While any ISIN is blocked there is no
report, so the listed rows leave out every blocked ISIN as a preview of the tool, not a choice the
user made. Before the rows, a "CORPORATE ACTIONS" part lists each corporate action of an ISIN
whose pool stopped at one: its date, rows and state (for example `WaitingForRatio`, or
`WaitingForEarlierAction` for a later action). The tool has no answers, so a ratio question stays
open. Then a "LOSS RULE (30 days)" part lists each sale of the year with a loss:
its window, loss units, units blocked by known purchases, open units, state and the purchases it
takes (units and dates only). The tool does not ask, so it treats the open units as blocked
(`assumed_blocked_units`, state `Decided`) and gives a warning per ISIN; the CLI asks instead. The file
contains private financial data and must remain local.

To write the Doh-KDVP XML for one year, run:

```sh
scripts/dump_tr_kdvp.sh --year 2025
scripts/dump_tr_kdvp.sh --synthetic --year 2025
```

The defaults are those of the FIFO dump; the output is `runtime/debug/tr_kdvp_<year>.xml`. With
`--synthetic` the input is the synthetic `tests/test_data/csv/kdvp_dump_fixture.csv` (trades in
2025) and the output `runtime/debug/tr_kdvp_synthetic_<year>.xml`. `--tax-number NNNNNNNN` sets
the taxpayer's tax number (default `12345678`, made up; use your own for an eDavki draft import),
and `--self-report` writes the self-report workflow code `I` (`SamoPrijava`). Blocked ISINs are
left out, so with a blocked ISIN the file is not a complete return, and the tool prints a warning;
the FIFO dump shows the reasons. The file from the real export contains private financial data
and must remain local.

The diagnostics text retains structured source locations without dumping event payloads. It may still contain
private identifiers and must remain local; it is not the application's JSON contract.
