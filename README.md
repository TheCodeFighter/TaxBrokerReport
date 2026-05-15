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

This project uses the C++ logging library:
:contentReference[oaicite:0]{index=0}

It is currently integrated in a minimal but production-oriented setup suitable for development and early-stage backend work.

---

### Current Setup

### Initialization

Logging must be explicitly initialized at application startup:

```cpp
#include "utils/logger.hpp"

int main()
{
    taxbroker::InitializeLogger();

    spdlog::info("Server started");
}
