# TaxBrokerReport

<p align="center">
	<img src="docs/assets/icon.jpg" alt="TaxBrokerReport Icon" width="200"/>
</p>

> This project is currently under major reconstruction.
>
> The legacy codebase, together with the full legacy documentation, is available here:
> https://github.com/TheCodeFighter/TaxBrokerReport/tree/main-legacy

**TaxBrokerReport** is an open-source tool designed for Slovenian investors to automate the generation of FURS-compatible XML files (Doh-KDVP, Doh-Div, and Doh-DHO) from broker export data.

## Development Workflow

Run `scripts/format.sh` before opening a pull request. To make Git reject unformatted commits and pushes locally, run `scripts/install_hooks.sh` once; it wires the repo-local hooks in `.githooks/` to `scripts/format.sh --check`.

The `Format Check` GitHub Action runs `scripts/format.sh --check` on every PR to `main`, so
unformatted code will fail CI and should be fixed before merge.

Separate GitHub Actions build the development and production images, run tests, run Valgrind, and
run Cppcheck on every pull request to `main` and after changes reach `main`. Keeping them separate
makes the failing category immediately visible. In the GitHub ruleset or branch-protection rule for
`main`, enable **Require status checks to pass before merging** and select **Development build**,
**Production image**, **Required tests**, **Valgrind**, and **Cppcheck**. Also select **Required
issue reference**, which requires the pull request's `Related issue` section to contain an issue
reference or exactly `N/A`.

### Static analysis

Run Cppcheck against all active project-owned C++ source and header files:

```sh
scripts/cppcheck.sh
```

The analysis uses CMake's compile database to cover `src/`, `tests/`, `tools/`, and all headers they
include. It also checks every file in `include/` directly so currently unreferenced headers are not
missed. Third-party dependencies and the deprecated `legacy-QT-GUI/` tree are intentionally
excluded. Cppcheck findings fail its dedicated pull-request and `main` workflow.

### Memory checks

Run every C++ executable under Valgrind inside the development container:

```sh
scripts/valgrind.sh
```

The script refreshes the normal Debug build and runs the unit tests, integration tests, backend
server, and Trade Republic dump tool. The dump tool is built in its isolated debug-tools build and
uses synthetic test data. Valgrind reports invalid memory access and fails on definite or indirect
memory leaks. Each test executable is started once to avoid Valgrind startup overhead for every
test discovered by CTest.

The dedicated `Valgrind` workflow runs the same memory checks for pull requests and after changes
reach `main`.

### Test coverage

Run the test suite with coverage instrumentation inside the development container:

```sh
scripts/coverage.sh
```

The script keeps its instrumented build separate from the normal development build and writes:

- an HTML report to `coverage/html/index.html`;
- the machine-readable LCOV report to `coverage/lcov.info`;
- a short summary and exact line-coverage metrics to `coverage/summary.txt` and
  `coverage/metrics.env`.

The `Test Coverage` GitHub Action compares every pull request with its exact base commit. Its job
summary shows both results and provides the HTML report as an artifact. It creates or updates one
pull-request comment with the current summary. A decrease is highlighted prominently but does not
currently block merging.

## Logging

This project uses spdlog with a lightweight wrapper for structured, thread-safe, and async-capable logging.

Parser warnings and errors intended for the local browser UI are not extracted from log text. Each
parse returns structured diagnostics, and the API layer can serialize them using the versioned JSON
contract documented in [docs/architecture.md](docs/architecture.md). To inspect the current Trade
Republic parser output and diagnostics locally, run:

```sh
scripts/dump_tr_parse.sh
```

Generated debug files are written below `runtime/`, which is ignored by Git.

## Initialization model

### Important

Logger initialization must happen exactly once.

```cpp
int main() {

    taxbroker::InitializeLogger();

    // application code

    spdlog::shutdown();
}
```

### Rules

* `InitializeLogger()` must be called:

  * exactly once
  * at application startup
  * before spawning worker threads

* `spdlog::shutdown()` must be called:

  * exactly once
  * at application shutdown

### Do NOT:

* Do NOT call `InitializeLogger()` in multiple files
* Do NOT call it inside worker threads
* Do NOT reinitialize logger at runtime

---

## Thread safety

* Logger initialization is protected with `std::call_once`
* Logging is thread-safe (spdlog `_mt` sinks)
* Async logging is enabled by default

---

## Async behavior

* Logging is asynchronous by default
* Uses a shared thread pool
* Prevents logging from blocking CSV parsing threads

### Notes

* If the queue is full, logging will block (backpressure)
* This is intentional to avoid log loss
* Overflow policy is `block`; switch to `overrun_oldest` in code for high-throughput production

---

## Flush behavior

* Logs with level `warn` and above are flushed immediately
* Additionally, logs are flushed every 1 second:

```cpp
spdlog::flush_every(std::chrono::seconds(1));
```

### Important

* In async mode, logs can be lost on crash
* Periodic flushing reduces this risk

---

## Output

### File logging (always enabled)

Logs are written to:

```
logs/taxbroker.log
```

---

### Stdout logging (optional)

Controlled by:

```
TBR_LOG_STDOUT=1|0
```

Default:

* enabled in debug builds
* disabled in release builds

### Recommendation

* Enable stdout when running in Docker
* Disable stdout for high-performance production runs

---

## Environment variables

* `TBR_LOG_LEVEL` = trace|debug|info|warn|error|critical
* `TBR_LOG_FILE` = log file path
* `TBR_LOG_FILE_MODE` = append|truncate
* `TBR_LOG_ASYNC` = 1|0
* `TBR_LOG_QUEUE_SIZE` = async queue size
* `TBR_LOG_THREADS` = async worker threads
* `TBR_LOG_STDOUT` = enable stdout logging

---

## Usage

Logging macros:

```cpp
LOG_TRACE("...");
LOG_DEBUG("...");
LOG_INFO("...");
LOG_WARN("...");
LOG_ERROR("...");
LOG_CRITICAL("...");
```

---

## Design notes

* Logger is initialized once and treated as immutable
* Configuration is read from environment variables at startup
* Runtime reconfiguration is NOT supported

---

## Performance considerations

* Avoid excessive logging in tight loops (e.g. per CSV row)
* Prefer aggregated or sampled logs
* Async logging prevents most contention, but queue saturation can still block

---

## Summary

* Initialize once
* Shutdown once
* Do not reconfigure at runtime
* Use async logging for parallel workloads
* Use stdout only when operationally needed
