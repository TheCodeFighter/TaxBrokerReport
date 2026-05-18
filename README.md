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

The `Format Check` GitHub Action also runs `scripts/format.sh --check` on every PR to `main`, so unformatted code will fail CI and should be fixed before merge.

## Logger (spdlog integration)

This project uses [spdlog](https://github.com/gabime/spdlog) for file-backed logging.

`taxbroker::InitializeLogger()` configures a named file logger, sets it as the default logger, and is safe to call more than once. By default it appends to `logs/taxbroker.log`, uses `debug` as the fallback level, and flushes warnings and errors eagerly.

The project keeps spdlog log calls compiled in at `trace` level, so `TBR_LOG_LEVEL` can actually enable or suppress debug output at runtime.

Use `TBR_LOG_FILE_MODE=truncate` only when you explicitly want to clear the log at startup. The other supported environment variables are:

- `TBR_LOG_LEVEL`: `trace`, `debug`, `info`, `warn`, `error`, `critical`, or `off`
- `TBR_LOG_FILE`: custom log file path
- `TBR_LOG_FILE_MODE`: `append` or `truncate`

Use the `LOG_*` macros from `utils/logger.hpp` when you want the pattern to include file and line information:

```cpp
#include "utils/logger.hpp"

int main()
{
    taxbroker::InitializeLogger();

    LOG_TRACE("Trace message");
    LOG_DEBUG("Debug message");
    LOG_INFO("Info message");
    LOG_WARNING("Warning message");
    LOG_WARN("Warning message");
    LOG_ERROR("Error message");
    LOG_CRITICAL("Critical message");

    LOG_INFO("Server started");
}
