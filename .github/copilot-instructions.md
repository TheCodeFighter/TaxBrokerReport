---
description: Workspace naming and style conventions for TaxBrokerReport.
applyTo: "**"
---

# TaxBrokerReport Conventions

- Use PascalCase for types, structs, classes, enums, and aliases.
- Prefix data members with `m` and use PascalCase after the prefix, for example `mName` and `mTaxNumber`.
- Prefix function parameters and local arguments with `a` and use PascalCase after the prefix, for example `aName` and `aTaxNumber`.
- Prefer explicit, descriptive names over abbreviated snake_case identifiers.
- Keep naming consistent with the legacy GUI codebase when adding new public types or data-transfer objects.

# Commits and branches
- All branches should be named:
  - TBR-<increment_number>-<short-description>, for common work (TBR is short for TaxBrokerReport)
  - TR-<increment_number>-<short-description>, for work related to Trade Republic (TR is short for Trade Republic), pay attention to increment properly from legacy codebase and it's commits
  - IBKR-<increment_number>-<short-description>, for work related to Interactive Brokers (IBKR is short for Interactive Brokers)
  - <new-broker-initials>-<increment_number>-<short-description>, for work related to a new broker (use the broker's initials as the prefix)

- All commits should be named:
  - TBR-<increment_number>: <short-description>, for common work
  - TR-<increment_number>: <short-description>, for work related to Trade Republic
  - IBKR-<increment_number>: <short-description>, for work related to Interactive Brokers
  - <new-broker-initials>-<increment_number>: <short-description>, for work related to a new broker (use the broker's initials as the prefix)

- Pay attention in reviews and auto generations of commits