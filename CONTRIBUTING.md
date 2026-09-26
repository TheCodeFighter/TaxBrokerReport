# Contributing to TaxBrokerReport

Thank you for helping improve TaxBrokerReport. By participating, you agree to follow the
[Code of Conduct](CODE_OF_CONDUCT.md).

## Before starting

- Search existing issues before opening a new one.
- Use the epic template for a milestone or large body of work.
- Use the implementation issue template for focused work and link its parent epic.
- Discuss large architectural changes before implementation.

## Development workflow

Create a focused branch and keep each pull request limited to one coherent change. Follow the
broker-neutral architecture described in [docs/architecture.md](docs/architecture.md) and the
current priorities in [docs/plan.md](docs/plan.md).

Run the relevant checks before opening a pull request:

```sh
scripts/build.sh dev
scripts/test.sh
scripts/format.sh --check
scripts/cppcheck.sh
```

For broader or higher-risk changes, also run `scripts/valgrind.sh`.

## Test data and privacy

Do not commit real broker exports, credentials, account numbers, transaction IDs, or personal or
financial information. Tests and examples must use synthetic data. Data based on a real export must
be fully anonymized and reduced to the smallest useful fixture before it is shared.

## Pull requests

- Link the related issue or write `N/A` when the change is genuinely standalone.
- Add or update tests for behavioral changes.
- Update documentation when behavior, interfaces, or workflows change.
- Keep comments compact and use them only where the intent is not clear from the code.

Security vulnerabilities must follow the private process in [SECURITY.md](SECURITY.md).
