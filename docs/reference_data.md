# Reference data: payers, relief statements and broker payers

## What this document explains

Dividend and interest forms need facts about the payer that a broker export does not contain: the
legal name, the tax number, the address and the country. The app ships small reference files for
this. This document names their source, licence and format, says what was left out, and says how
the app may use them. The tax rules that decide what the payer facts mean are in
[`tax_rules.md`](tax_rules.md), "Payer lists, settled Slovenian tax and foreign-tax relief".

## Source

| Item | Value |
| --- | --- |
| Project | `ib-edavki` (`https://github.com/ib-edavki/ib-edavki`), a tool that turns Interactive Brokers reports into eDavki XML |
| Licence | MIT, copyright 2020 Primož Sečnik Kolman |
| Revision | `22220f546378b638c129ffe4bd6f2c9a3139809c` (2026-03-29) |
| Files used | `companies.xml`, `relief-statements.xml`, `ib-affiliates.xml` |

The MIT licence allows copying with the copyright and permission notice. The notice is shipped
unchanged as `resources/reference/LICENSE.ib-edavki`. Every converted row records the source and
the revision, so a later upstream update is a separate change that records its own revision and
the changes in counts.

The app never downloads these files and works offline. Upstream's own tool downloads them at run
time; this app does not. A newer copy is supplied by the user with `--reference-files`, see "How the app loads the files".

## Files

The files are in `resources/reference/`. They are UTF-8 text, comma-separated, with one header row
and `\n` line endings. A field with a comma or a quote is quoted. After the conversion, these
files are the source: a company that is added later is a new row with `source` set to `manual`.

| File | Columns | Rows |
| --- | --- | ---: |
| `payers.csv` | `isin`, `ibkr_conid`, `name`, `tax_number`, `address`, `country`, `instrument_kind`, `verified_on`, `source`, `source_revision` | 299 |
| `payers_by_ticker.csv` | `ticker`, `isin`, `name`, `tax_number`, `address`, `country`, `instrument_kind`, `verified_on`, `source`, `source_revision` | 139 |
| `relief_statements.csv` | `country`, `statement`, `source`, `source_revision` | 17 |
| `broker_payers.csv` | `broker`, `code`, `name`, `tax_number`, `address`, `country`, `source`, `source_revision` | 5 |

- `payers.csv`: a payer of dividends. At least one of `isin` and `ibkr_conid` is set (`ibkr_conid`
  is the Interactive Brokers contract number, an exact identifier of one instrument at that
  broker). 68 rows have an ISIN, 278 have a `conid` (digits only), 47 have both. `tax_number`
  is empty in 10 rows. The ISIN identifies one instrument, so for an ETF it also identifies the
  share class; there is no separate share class column.
- `instrument_kind` (empty or `share`, `etf`, `fund`, `bond`) and `verified_on` (empty or
  `YYYY-MM-DD`, the day a person checked the row) are filled only by a `manual` row. Every
  imported row leaves them empty. They are shown to the user and never compared with a payment
  date. A row has no validity dates: when a payer changes, the new facts are a new row, and the
  lookup reports the two rows as a conflict until the user decides.
- A Slovenian company is a row of `payers.csv` with `country` `SI` and `source` `manual`. There
  is no separate file; no such row exists yet.
- `payers_by_ticker.csv`: upstream payers that have no usable identifier: only a ticker, or an ISIN
  that fails the check digit. `isin` is empty in every row and is to be filled later by looking
  the ticker up. The app does not read this file until then.
- `relief_statements.csv`: the treaty text that upstream writes into the `ReliefStatement` field of
  Doh-Div, one per country. The text is Slovenian (a treaty citation).
- `broker_payers.csv`: the Interactive Brokers companies that pay interest, for Doh-Obr.

## What was left out

Upstream's `companies.xml` has 438 entries. 132 have neither an ISIN nor a `conid`, only a ticker.
6 more have a ticker (`INRG`, `INTU`, `LOB`, `MA`, `NVDA`, `WD`) in the `conid` field instead of
a contract number. These 138 entries are not in `payers.csv`, because a ticker is not an exact
identifier: the same ticker can name different instruments on different exchanges. One more entry
(ticker `NAKA`) has an ISIN that fails the ISIN check digit and no `conid`, so it is left out too:
the app checks every ISIN of `payers.csv`, and a wrong ISIN would never match. These 139 entries
are kept in `payers_by_ticker.csv` so that they are not lost; each needs its ISIN found before it
can move into `payers.csv`. Upstream's ticker column is copied only there.

Upstream has no dates, no instrument kind, no ETF share class and no record of how a fact was
checked. The imported rows leave `instrument_kind` and `verified_on` empty. They must not be
invented when the files are loaded.

## Missing ISINs (to do)

The 139 payers in `payers_by_ticker.csv` have no usable ISIN, so the app cannot use them yet. Their ISINs
still have to be added:

1. Find the ISIN of each ticker from a source with a known licence. A ticker or a name alone is
   not enough, because one ticker can name different instruments on different exchanges.
2. Where the ISIN is certain, move the row into `payers.csv` with that ISIN. Record the lookup
   source in `source`.
3. A ticker that names several instruments, or whose ISIN cannot be confirmed, stays in
   `payers_by_ticker.csv` with a note. Manual income details still cover it.
4. Update the counts in this document after each move.

Until then, the app does not read `payers_by_ticker.csv`, and a payer found only here is treated
as a missing row (manual income details).

## Quality of the upstream data

These counts are for the 438 upstream entries. They explain why every fact is only a suggestion.

| Observation | Count |
| --- | ---: |
| Tax numbers shared by several entries | 21 numbers, 65 entries, up to 15 entries on one number |
| Tax number kinds | 57 look like a VAT number, 3 like a LEI, 369 are other registry numbers |
| ISIN prefix differs from `country` | 14 |
| Tickers that appear in more than one entry | 5 |
| One payer with different number forms: BlackRock Asset Management Ireland as VAT ID `IE8227552C`, VAT number `8227552` and company register number `227552`, all valid | 1 payer, 12 rows |
| `tax_number` is an ISIN, or an ISIN without its country prefix, not a registry number | 4 |

A tax number used by 15 entries is a fund family or a custodian, not the issuer of each share. An
ISIN prefix does not prove where the payer lives.

## How the app may use the files

1. **Exact match only.** A payer is found by the exact ISIN, or by the exact `conid` for an
   Interactive Brokers event. A ticker, a name, an ISIN prefix, an exchange, a currency or a
   broker never matches.
2. **A suggestion, not a fact.** A row prefills the income details. The user confirms them once;
   the confirmation is stored with the decision. Nothing from a reference file is written into an
   XML file without that confirmation.
3. **Payment evidence wins.** A payment that shows other facts than the row (for example another
   country) overrides it.
4. **A missing or conflicting row** (no row, two rows with different facts for one identifier, or
   a row changed by a new revision after the user confirmed it) goes to the income details flow
   (`income_details_required`). Manual entry always works.
5. **The relief statement is not a claim.** A row in `relief_statements.csv` never means that the
   user is entitled to relief. How its text may prefill the field is decided with the foreign-tax
   relief rules; until then it is not used.
6. **Broker payers** are used only with the broker they name.
7. **No coverage claim.** The files do not cover every issuer or ETF.

## How the files were made

The three upstream XML files were read once with an XML parser and written as the CSV files above:

- Text values were trimmed and kept otherwise unchanged; XML character references (for example a
  typographic apostrophe) became the character itself.
- Upstream order was kept.
- For `broker_payers.csv`, `broker` is `IBKR` and `code` is upstream's `code`.
- For `payers.csv`, the ticker (`symbol`) column was not copied, and the 138 entries with only a
  ticker (6 of them in the `conid` field) went to `payers_by_ticker.csv` instead. So did the entry
  with the ISIN that fails the check digit (its ISIN is not copied; its `isin` is empty).
- The columns `instrument_kind` and `verified_on` were added, empty in every row.
- Upstream's `-` (meaning unknown) as the tax number of 3 payers became an empty field.
- One value was corrected: the country `IRL` of one payer (Johnson Controls International Plc)
  became `IE`, the 2-letter code every other row uses.
- Every row has `source` set to `ib-edavki` and `source_revision` set to the revision above.

`payers_by_ticker.csv` holds the 139 entries in upstream order, with `symbol` as `ticker` and an
empty `isin`. 299 + 139 = 438, the upstream count.

The check after the conversion: every row has as many fields as the header; `payers.csv` has no
duplicate ISIN, no duplicate `conid`, no row that has neither, only digits in `ibkr_conid`, an ISIN
with a correct check digit in every row that has one, and a 2-letter `country` in every row.

## How the app loads the files

The three files that the app reads (`payers.csv`, `relief_statements.csv`, `broker_payers.csv`) are
built into the program as text. The app never downloads them. A newer copy comes from the user:
`--reference-files FILE...` names files, and each file whose name is one of the three replaces the
built-in file of that name whole. The other names keep the built-in file. The supplied file is the
source of truth: where it disagrees with the built-in data, it wins, with no warning and no
comparison of versions, because a mistake in a built-in file is fixed by publishing a corrected
file. A file with another name, or the same name twice, is an error before any file is read. A
supplied file goes through the same checks as a built-in one. The summary of a run shows which
files were supplied. The shared CSV reader reads each file and the loader checks every row; a fault
stops the load with the file name and the logical row (header = 1), never with a value of the
file, and carries a message key (`reference_file.*`) with named parameters, so the screen can
translate it:

- `payers.csv`: an ISIN or a `conid`; an ISIN of 2 letters, 9 letters or digits and a correct
  check digit; a `conid` of digits only; a non-empty `name` and `address`; a `country` of 2
  uppercase letters; an `instrument_kind` that is empty or one of the four kinds; a `verified_on`
  that is empty or a calendar date; `source` `ib-edavki` or `manual`, and a `source_revision` for
  `ib-edavki`.
- `relief_statements.csv`: a 2-letter `country`, a non-empty `statement`, a source.
- `broker_payers.csv`: a non-empty `broker`, `code`, `name` and `address`, a 2-letter `country`, a
  source, and no broker and code twice.

A payer lookup takes an ISIN, a `conid` or both, and returns one of three results:

| Result | When |
| --- | --- |
| Found | One row has an identifier of the query, or several rows do and agree on `name`, `tax_number`, `address`, `country` and `instrument_kind`. A row found by both the ISIN and the `conid` counts once. The result is the first such row in file order. |
| Conflict | Rows with an identifier of the query disagree on one of those facts. All rows are returned. |
| NotFound | No row has an identifier of the query. |

Both Conflict and NotFound lead to the income details flow (rule 4). The lookup never matches a
ticker, a name or a country.
