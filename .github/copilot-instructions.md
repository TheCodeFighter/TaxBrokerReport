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

Follow these branching and commit naming conventions based on the type of work:

| Work Type | Prefix | Branch Name Format | Commit Message Format |
| :--- | :--- | :--- | :--- |
| Common Work | `TBR-` | `TBR-<increment>-<short-desc>` | `TBR-<increment>: <short-desc>` |
| Trade Republic | `TR-` | `TR-<increment>-<short-desc>` | `TR-<increment>: <short-desc>` |
| Interactive Brokers | `IBKR-` | `IBKR-<increment>-<short-desc>` | `IBKR-<increment>: <short-desc>` |
| New Broker | `<initials>-` | `<initials>-<increment>-<short-desc>` | `<initials>-<increment>: <short-desc>` |

**Additional Rules:**
- **Trade Republic (TR)** branch increments: Ensure the increment number follows the sequential numbering scheme used in the legacy codebase and its commits.
- Pay attention in reviews and auto-generations of commits to ensure they adhere to these table structures.
- When generating a commit message, always start the subject with the required prefix and increment from the active branch or work type, for example `TBR-0006: ...`, `TR-0105: ...`, or `IBKR-0001: ...`.
- Do not generate a plain subject line without the prefix; if the prefix is missing, revise the commit message before finalizing it.

# Logging constraints (for contributors and AI tools)

- Do not modify logging macros
- Do not introduce additional logging libraries
- Do not reinitialize logger
- Do not call InitializeLogger() outside main()
- Do not call spdlog::shutdown() outside main()
- Logger configuration must remain environment-driven
- Keep logging lightweight and non-blocking
- Any changes must preserve:
  - async logging behavior
  - thread safety
  - minimal dependencies
