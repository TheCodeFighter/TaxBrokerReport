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
time; this app does not.

## Files

The files are in `resources/reference/`. They are UTF-8 text, comma-separated, with one header row
and `\n` line endings. A field with a comma or a quote is quoted. After the conversion, these
files are the source: a company that is added later is a new row with `source` set to `manual`.

| File | Columns | Rows |
| --- | --- | ---: |
| `payers.csv` | `isin`, `ibkr_conid`, `name`, `tax_number`, `address`, `country`, `source`, `source_revision` | 300 |
| `payers_by_ticker.csv` | `ticker`, `isin`, `name`, `tax_number`, `address`, `country`, `source`, `source_revision` | 138 |
| `relief_statements.csv` | `country`, `statement`, `source`, `source_revision` | 17 |
| `broker_payers.csv` | `broker`, `code`, `name`, `tax_number`, `address`, `country`, `source`, `source_revision` | 5 |

- `payers.csv`: a payer of dividends. At least one of `isin` and `ibkr_conid` is set (`ibkr_conid`
  is the Interactive Brokers contract number, an exact identifier of one instrument at that
  broker). 69 rows have an ISIN, 278 have a `conid` (digits only), 47 have both. `tax_number`
  is empty in 11 rows.
- `payers_by_ticker.csv`: upstream payers that have only a ticker. `isin` is empty in every row and
  is to be filled later by looking the ticker up. The app does not read this file until then.
- `relief_statements.csv`: the treaty text that upstream writes into the `ReliefStatement` field of
  Doh-Div, one per country. The text is Slovenian (a treaty citation).
- `broker_payers.csv`: the Interactive Brokers companies that pay interest, for Doh-Obr.

## What was left out

Upstream's `companies.xml` has 438 entries. 132 have neither an ISIN nor a `conid`, only a ticker.
6 more have a ticker (`INRG`, `INTU`, `LOB`, `MA`, `NVDA`, `WD`) in the `conid` field instead of
a contract number. These 138 entries are not in `payers.csv`, because a ticker is not an exact
identifier: the same ticker can name different instruments on different exchanges. They are kept
in `payers_by_ticker.csv` so that they are not lost; each needs its ISIN found before it can move
into `payers.csv`. Upstream's ticker column is copied only there.

Upstream has no dates, no instrument kind, no ETF share class and no record of how a fact was
checked. These fields do not exist in the files. They must not be invented when the files are
loaded.

## Missing ISINs (to do)

The 138 payers in `payers_by_ticker.csv` have no ISIN, so the app cannot use them yet. Their ISINs
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
  ticker (6 of them in the `conid` field) went to `payers_by_ticker.csv` instead.
- Upstream's `-` (meaning unknown) as the tax number of 3 payers became an empty field.
- One value was corrected: the country `IRL` of one payer (Johnson Controls International Plc)
  became `IE`, the 2-letter code every other row uses.
- Every row has `source` set to `ib-edavki` and `source_revision` set to the revision above.

`payers_by_ticker.csv` holds the 138 entries in upstream order, with `symbol` as `ticker` and an
empty `isin`. 300 + 138 = 438, the upstream count.

The check after the conversion: every row has as many fields as the header; `payers.csv` has no
duplicate ISIN, no duplicate `conid`, no row that has neither, only digits in `ibkr_conid`, and
a 2-letter `country` in every row.
