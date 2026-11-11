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
| S5 | 2024 | A loss blocked in full by a purchase within 30 days: one sale row with `F10` false |
| S6 | 2025 | A split before the sale: the purchase row lists the units after the split with `F4` = basis / units |
| S7 | 2025 | A sale whose loss is blocked in part: two sale rows with the same date and price, `F10` true and false |
| S8 | 2024 | A purchase sold before and after a split: two purchase rows of one purchase, `F8` mixing old and new units |
| P1 | 2025 | Probe: S3 with the left-out unit added back by hand |
| P2 | 2023 | Probe: S4 with the left-out sale added back by hand |

## Findings

- **2026-10-27, workflow code of a self-report.** A self-report (*samoprijava*) is
  `DocumentWorkflowID` `I` with `DocumentWorkflowName` `SamoPrijava`. The `S` and `SelfReport`
  that the legacy app wrote, and that the app wrote until this finding, are wrong. The writer now
  writes `I` and `SamoPrijava`.
- **2026-11-10, no self-report workflow for the income forms.** A draft import of Doh-Div,
  Doh-DHO and Doh-Obr with `I` / `SamoPrijava` and `SelfReport` `true` did not work; the same
  files as originals (`O` / `Original`, `SelfReport` `false`) imported. A Doh-Obr file with `O` in
  the header and `I` in `DocumentWorkflowID` of the body (a hand edit) did not work either. The
  three writers therefore always write an original, also for a run with `--self-report`, which
  changes only Doh-KDVP. The `ib-edavki` project also writes `O` for these forms.
- **2026-11-10, the content errors of a draft import are expected.** eDavki lists a critical
  content error for a row that has some payer facts but not all (name, address, payer country,
  source country), and for a foreign tax without the proof of payment as an attachment. A row
  without a reference row has empty payer facts, and the app already warns about both (a payer
  that is not found and the foreign-tax evidence); the user completes them in eDavki.
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

## Results: income forms

Golden files of `tests/test_data/income/` (synthetic), original workflow `O`, a tax number that
passes eDavki's check. Each file is app output: an integration test compares the writer's
output with it byte for byte.

| Form | Schema | Tax year | Checked on | Import | Content errors | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Doh-Div | `Doh_Div_3.xsd` | 2024 (the golden file has 2025; a 2025 return already existed) | 2026-11-10 | Imported | Row 3 (no payer facts): name, address, payer country, source country; foreign-tax proof for the row with tax 2.40 | Dates, amounts (4.00, 5.00, 15.63 with tax 2.40, 10.02), `BR` and `US` countries, the Brazilian identification number, the treaty text `10/01, 2b odstavek 10. člena` and `false` as expected |
| Doh-DHO | `Doh_DHO_4.xsd` | 2025 | 2026-11-10 | Imported | None | |
| Doh-Obr | `Doh_Obr_2.xsd` | 2025 | 2026-11-10 | Imported | Both coupons (no payer facts): payer country, name, address, source country | `Type` 2, `Value` 16,00 and 15,63, `ForeignTax` 0,00 as expected |
| Doh-Obr (`Doh-Obr_2023_treaty`) | `Doh_Obr_2.xsd` | 2022 (the golden file has 2023) | 2026-11-10 | Imported | Foreign-tax proof of payment required for the rows with a foreign tax (3.00 and 6.00) | Three rows with payer facts: `ReliefStatement` `2. odstavek 11. člena`, `ReliefStatement` `true` (imported before the change to `DA`), and a `Type` `4` cash-interest row without the element |
| Doh-Obr (E4, a coupon of a Slovenian issuer) | `Doh_Obr_2.xsd` | 2025 | 2026-11-11 | Imported | None at import. The check before submitting, for the row written at fb4a079: a Slovenian payer needs `TaxNumber` and must not have `IdentificationNumber`. With the made-up number moved to `TaxNumber`: name and address differ from the tax register. | With a real tax number (Krka `82646716`) and only the number filled in, eDavki fills in the name and address from the register and the check passes. The writer now writes `TaxNumber` and leaves out `IdentificationNumber`, `Name` and `Address` for such a payer |
| Doh-Div (E2, a dividend on a Saturday, rate of the day before) | `Doh_Div_3.xsd` | 2025 | 2026-11-11 | Imported | Only the empty payer facts, as expected | `Value` 10,00, `ForeignTax` 0,00, date 2025-03-15 as expected |
| Doh-Obr (E7, a taxed coupon with payer facts and a treaty text, through the CLI) | `Doh_Obr_2.xsd` | 2025 | 2026-11-11 | Imported | No unexpected error | App commit fb4a079 |
| Doh-KDVP, Doh-Div, Doh-DHO, Doh-Obr (E1, the four files of `scripts/report.sh --synthetic`) | the four schemas | 2024 | 2026-11-11 | Imported | No unexpected error | App commit fb4a079, the taxpayer's own tax numbers |

## Not checked yet

- **eDavki's calculation** for any scenario, including the holding-period boundaries (S3, S4, P1,
  P2) and `F7` below the broker quantity (S2): a draft does not show it (see Findings).
- **The original workflow `O` for Doh-KDVP.** Doh-Div, Doh-DHO and Doh-Obr as originals were
  imported as drafts on 2026-11-10 (see "Results: income forms"), outside 1 January to
  28 February, so the note in `tests/manual/edavki/README.md` that an original imports only in
  that window does not hold for these forms. Doh-KDVP as an original was not imported yet.
- **Doh-Div as written by the app.** A draft import accepted `ReliefStatement` `false` on the rows
  without a claim and the treaty text on the claimed row, `Type` `1`, `ForeignTax` `0.00` and
  `SelfReport` `false`. Still open: that eDavki applies the relief from the text (a draft shows no
  calculation), `Type` for a foreign fund distribution (it may need `4`; FURS did not say), and a
  row without payer facts only warns: eDavki lists critical content errors for it, which the user
  completes in eDavki before filing.
- **Doh-DHO as written by the app.** A draft import accepted the file (taxpayer block without
  `ResidentCountry` and `WfTypeU`, no country names, the totals, `SelfReport` `false`) with no
  errors. Still open: a row without payer facts and a row with a foreign tax.
- **Doh-Obr as written by the app.** Draft imports accepted two coupons of `Type` `2` without
  payer facts (the only errors were the empty payer facts), and a file with payer facts, a treaty
  text for interest (`2. odstavek 11. člena`), `ReliefStatement` `true` (the writer now writes `DA`, not imported yet), and broker cash interest
  of `Type` `4` (see "Results: income forms"). Still open: whether eDavki shows a `true` row as a
  claim the user completes with the paragraph and article (not recorded), and whether an interest text needs the
  *Uradni list MP* number (the built-in file has no interest text yet); the interest-type code
  for broker cash interest (FURS names none, `4` imports); the payer fields of a coupon (the
  issuer's facts; unconfirmed); whether a draft shows the interest as expected (`tax_rules.md`, "Doh-Obr form
  contract"). A coupon of a Slovenian issuer: eDavki's check before submitting wants `TaxNumber`
  (eight digits) and no `IdentificationNumber`, and fills in the name and address from the tax
  register (E4, 2026-11-11); a draft import accepts it. Still open: the same rule for a Slovenian
  payer in Doh-DHO and Doh-Div (not checked). `Doh-Obr_2023.golden.xml` (a self-report run written
  as an original) is not imported; its rows have the shapes already imported (E1 and E7 cover
  them).
- **S5, a loss blocked by the 30-day rule (`F10` false)** (`tests/manual/edavki/README.md`, from
  following tasks): not imported (see "Not imported on purpose").
- **S6 to S8, the rows for split lots and partly blocked losses** (#267): not imported yet. The
  files and expected rows are in `tests/manual/edavki/README.md`.
- **The developer export without `F10`**. The CLI blocks that case today.
- **Trade Republic bond maturity rows**: `FINAL_MATURITY` (action and cash rows),
  `FINAL_MATURITY_CANCELLED` and the foreign payout are checked against one real export (shapes
  only). Still guesses: `REDEMPTION`, `shares` filled on a cash row, the sign of `tax`
  on a maturity, `TILG`, `SEC_ACCOUNT` and `EARLY_REDEMPTION` rows (`architecture.md`, "Trade
  Republic bond rows").
- If a result is unclear, the FURS developer contact is `sd.fu@gov.si`.
