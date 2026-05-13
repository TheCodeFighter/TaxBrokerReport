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