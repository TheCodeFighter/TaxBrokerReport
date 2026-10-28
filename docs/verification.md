# eDavki verification

This file records what eDavki did with the XML files the app writes: whether a draft import
worked, and whether the portal calculated the values we expect. It is interoperability evidence
only.

- An import is never submitted. A working draft is not a filed return and not legal
  certification.
- The legal rules are in [`tax_rules.md`](tax_rules.md), the official examples and sources there
  too. The XSD checks and the golden XML tests are described in
  [`architecture.md`](architecture.md). None of these are recorded here.
- Only results seen in eDavki are recorded. A synthetic test, an XSD check or a CLI run that was
  not imported is never called an eDavki check.

## Rules

- Never submit a draft; delete it after the check.
- No personal data: no tax number, name, address, screenshots or values from a real export.
- The scenario files are synthetic and committed in
  [`tests/manual/edavki/`](../tests/manual/edavki/README.md), which has the steps, the inputs, the
  expected rows and the expected values.
- Every entry names the app commit that wrote the file, the schema version, the tax year, the
  workflow (`O` original, `I` self-report), the date of the check and what eDavki showed.
- A hand-edited file (a probe) is recorded as a probe, never as app output.

## Scenarios

| Scenario | Tax year | What it checks |
| --- | --- | --- |
| S1 | 2025 | One sale uses two purchases; an earlier-year sale and unused units are not listed; `F3` and `F8` of only the listed units |
| S2 | 2025 | A sale mixes units past the 15 years with newer units; `F7` (1) is lower than the broker quantity (3) |
| S3 | 2025 | Sale on the 15-year anniversary (listed) and on the day after (left out), in one sale |
| S4 | 2023 | Purchase on 29 February: the 15 years end on 28 February (listed), 1 March is past (left out) |
| P1 | 2025 | Probe: S3 with the left-out unit added back by hand |
| P2 | 2023 | Probe: S4 with the left-out sale added back by hand |

## Findings

- **2026-10-27, workflow code of a self-report.** A self-report (*samoprijava*) is
  `DocumentWorkflowID` `I` with `DocumentWorkflowName` `SamoPrijava`. The `S` and `SelfReport`
  that the legacy app wrote, and that the app wrote until this finding, are wrong. The writer now
  writes `I` and `SamoPrijava`.
- **2026-10-27, no calculation in a draft.** eDavki calculates the tax only after a return is
  submitted, and FURS contacts the taxpayer if something is wrong. A draft import therefore shows
  that eDavki accepts the file and its rows, not that the gain or the holding period is treated as
  expected. The calculation columns record this; the expected values stay in
  `tests/manual/edavki/README.md`.

## Results: Doh-KDVP

Schema `Doh_KDVP_9.xsd` with `EDP-Common-1.xsd` (`resources/xml/schemas/`).

| Scenario | Tax year | Workflow | App commit | Checked on | Import | Calculation against the expected values | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Own export | Not recorded | `S` | PR #304 branch, before its merge `ae87767` | 2026-10-26 | Imported | Not compared in detail | Maintainer's own export, written by `scripts/dump_tr_kdvp.sh` before the CLI existed, with the wrong self-report code `S` (see Findings); no values recorded |
| S1 | 2025 | `I` | TBR-0074 | 2026-10-27 | Imported | Not shown in a draft | Rows as expected in `tests/manual/edavki/README.md` |
| S2 | 2025 | `I` | TBR-0074 | 2026-10-27 | Imported | Not shown in a draft | Rows as expected in `tests/manual/edavki/README.md` |
| S3 | 2025 | `I` | TBR-0074 | 2026-10-27 | Imported | Not shown in a draft | Rows as expected in `tests/manual/edavki/README.md` |
| S4 | 2023 | `I` | TBR-0074 | 2026-10-27 | Imported | Not shown in a draft | Rows as expected in `tests/manual/edavki/README.md` |

### Probes (hand-edited, not app output)

| Probe | Tax year | Workflow | Based on | Checked on | Import | What eDavki showed for the left-out unit | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| P1 | 2025 | `I` | S3 | 2026-10-27 | Imported | Nothing: a draft shows no holding period or tax | The import does not tell whether the row is tax free |
| P2 | 2023 | `I` | S4 | 2026-10-27 | Imported | Nothing: a draft shows no holding period or tax | The import does not tell whether the row is tax free |

## Not checked yet

- **eDavki's calculation** for any scenario, including the holding-period boundaries (S3, S4, P1,
  P2) and `F7` below the broker quantity (S2): a draft does not show it (see Findings).
- **The original workflow `O` from the CLI.** eDavki imports an original return only from
  1 January to 28 February. A working self-report is expected to mean the original works too, but
  that is an expectation, not a check. Next window: 1 January to 28 February 2027, tax year 2026.
- **Doh-Div, Doh-DHO and Doh-Obr.** Their writers do not exist yet.
- **S5, a loss blocked by the 30-day rule (`F10` false)** (`tests/manual/edavki/README.md`, from
  following tasks): not imported yet.
- **Doh-KDVP rows for split-adjusted history and partially eligible losses** (#267), and the
  developer export without `F10` (#188). The CLI blocks these cases today.
- If a result is unclear, the FURS developer contact is `sd.fu@gov.si`.
