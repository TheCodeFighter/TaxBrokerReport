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
| `payers.csv` | `isin`, `name`, `tax_number`, `address`, `country`, `instrument_kind`, `verified_on`, `isin_source`, `source`, `source_revision` | 282 |
| `payers_without_isin.csv` | `ticker`, `ibkr_conid`, `name`, `tax_number`, `address`, `country`, `note`, `source`, `source_revision` | 151 |
| `relief_statements.csv` | `country`, `income`, `statement`, `source`, `source_revision` | 17 |
| `broker_payers.csv` | `broker`, `code`, `name`, `tax_number`, `address`, `country`, `source`, `source_revision` | 6 |

- `payers.csv`: a payer of dividends, found by its ISIN. Every row has an ISIN, so one row serves
  every broker. `isin_source` says where the ISIN came from: `ib-edavki` (upstream gave it,
  68 rows), `wikidata` (found as described in "How the ISINs were found", 214 rows), or, for a
  later `manual` row, `issuer` (the issuer's own page or report), `exchange` (the exchange's page)
  or `gleif` (the GLEIF ISIN-to-LEI data). `source` still names where the other facts came from.
  `tax_number` is empty in 5 rows. The ISIN identifies one instrument, so for an ETF it also
  identifies the share class; there is no separate share class column.
- `instrument_kind` (empty or `share`, `etf`, `fund`, `bond`) and `verified_on` (empty or
  `YYYY-MM-DD`, the day a person checked the row) are filled only by a `manual` row. Every
  imported row leaves them empty. They are shown to the user and never compared with a payment
  date. A row has no validity dates: when a payer changes, the new facts are a new row, and the
  lookup reports the two rows as a conflict until the user decides.
- A Slovenian company is a row of `payers.csv` with `country` `SI`. Its `tax_number` is the eight
  digits only, with a valid check digit (no `SI` prefix); otherwise the app writes no tax number
  and warns. The four rows (Krka, NLB, Petrol, Luka Koper) hold upstream's number without `SI`,
  and each passes the check digit. Upstream's Krka number failed it; the row holds `82646716`
  from the Slovenian tax register instead (checked in eDavki), with `source` `manual`.
  `payers_without_isin.csv` and `payers_by_ticker.csv` still have the `SI` prefix; the app does not
  read them.
- `payers_without_isin.csv`: upstream payers whose ISIN is not certain, kept so that they are not
  lost. `ticker` is upstream's symbol, `ibkr_conid` the Interactive Brokers contract number where
  upstream gave one (a record of the upstream data, not a key). `note` says why the row is here:
  `isin_not_found` (no source gave a confirmed ISIN), `ticker_ambiguous` (the ticker leads to
  several instruments), `instrument_ended` (the ISIN found belongs to a later company, for example
  after a move to another country, not to the company of the row) or `isin_check_digit` (upstream's
  ISIN fails the check digit). The app does not read this file.
- `relief_statements.csv`: the treaty text of a country for the `ReliefStatement` field, one row per
  country and kind of income. `income` is `dividend` (Doh-Div, the 17 upstream rows),
  `cash_interest` (Doh-Obr interest on the cash a broker holds) or `bond_coupon` (Doh-Obr bond
  coupon). The articles of a treaty can differ per kind of income, so a row serves only its own
  kind. The text is Slovenian (a treaty citation). Upstream has no interest text, so the built-in
  file has no `cash_interest` or `bond_coupon` row yet: a row is added with `source` `manual` and a
  source for the text (the treaty or an official FURS page), in a release or in a supplied file.
- `broker_payers.csv`: the broker companies that pay interest on the cash a broker holds, for
  Doh-Obr and Doh-DHO: the five Interactive Brokers companies and Trade Republic Bank GmbH (`TR`,
  `manual`, from its imprint: Brunnenstr. 19-21, 10119 Berlin, VAT ID `DE307510626`).

## What was left out

Upstream's `companies.xml` has 438 entries. 132 have neither an ISIN nor a `conid`, only a ticker.
6 more have a ticker (`INRG`, `INTU`, `LOB`, `MA`, `NVDA`, `WD`) in the `conid` field instead of
a contract number. A ticker is not an exact identifier: the same ticker can name different
instruments on different exchanges. One more entry (ticker `NAKA`) has an ISIN that fails the
ISIN check digit and no `conid`; the app checks every ISIN of `payers.csv`, and a wrong ISIN would
never match. These 139 entries, like the upstream payers with only a `conid`, enter `payers.csv`
only with an ISIN found as described in "How the ISINs were found".

Upstream has no dates, no instrument kind, no ETF share class and no record of how a fact was
checked. The imported rows leave `instrument_kind` and `verified_on` empty. They must not be
invented when the files are loaded.

## How the ISINs were found

Upstream gave an ISIN for only 68 of its payers; most of the others had only the Interactive
Brokers contract number (`conid`), which no other broker knows. The ISIN of each such payer was
looked up once, from the payer's upstream ticker and name:

1. **Proposal: Wikidata** (`https://www.wikidata.org`, CC0, no notice needed). An item counts
   when it is listed under the row's ticker on an exchange (or found by the row's name) and its
   name matches the row's name, ignoring legal-form words such as "Inc", "plc" or "AG". Its ISIN
   is the proposal.
2. **Confirmation: a second source.** The proposed ISIN must also be confirmed by GLEIF (the
   global LEI registry, `https://www.gleif.org`; its ISIN-to-LEI data names the legal entity that
   issued the ISIN) with the same legal name, or by OpenFIGI (`https://www.openfigi.com`, which
   lists the tickers and names of an ISIN) with the same name, and with the row's ticker when the
   item was found by name only. Nothing from these two sources is copied into the files; they only
   confirm.
3. **One ISIN.** When several confirmed ISINs remain, the one listed under the row's ticker is
   kept. When that still leaves several, the row is left out (`ticker_ambiguous`).
4. **The company of the row.** The ISIN must belong to the company the row describes. Where the
   only ISIN found belongs to a later company with a new legal form or a new country (for
   example "Ltd" that became "plc" in another country), the row is left out
   (`instrument_ended`): its tax number and address may be those of the earlier company. An
   older ISIN of the same company (for example from before a rename) is kept, because a dividend
   of an earlier year may still be reported with it.
5. A ticker or a name alone never decides, and the ISIN prefix is never a check of `country`.

Result: 219 of the 370 payers without an ISIN got a confirmed one; 151 are in
`payers_without_isin.csv` (146 `isin_not_found`, 3 `instrument_ended`, 1 `ticker_ambiguous`, 1
`isin_check_digit`). 5 of the 219 were already in the file with the same facts (one company listed
under two tickers), so only the first row was kept: 214 rows of `payers.csv` have `isin_source`
`wikidata`. For 2 ISINs upstream has two rows with different facts (the name written another way,
another address or address form, a tax number with and without its country prefix); both rows are
kept, and the lookup reports a Conflict
that the user decides. An income whose payer is not in `payers.csv` is still reported, with empty
payer facts and a warning (rule 4 below).

A later fix is a `manual` row in `payers.csv` with its own `isin_source`, or a row moved out of
`payers_without_isin.csv` when its ISIN is confirmed the same way.

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

1. **Exact match only.** A payer is found by the exact ISIN, for every broker. A ticker, a name,
   an ISIN prefix, an exchange, a currency, a broker or a broker's own number (such as a
   `conid`) never matches.
2. **A suggestion, not a fact.** One row, or several rows with the same facts, prefill the payer
   facts of the income rows without a question. The result lists the row (file, row number and
   source) with the facts, so the user can check them and complete or correct them in eDavki. Only
   rows that disagree are put to the user (rule 4).
3. **Payment evidence wins.** A payment that shows other facts than the row (for example another
   country) overrides it.
4. **A missing or conflicting row** (no row, two rows with different facts for one identifier, or
   a row changed by a new revision after the user picked it) leaves the payer facts empty. The
   income row is still made, and a warning tells the user to complete them in eDavki. With two
   rows that disagree the user picks one of them or skips (a skip, or no answer, is like no row). A row with `country` `SI` makes
   the payment a Slovenian payment, which is not reported (the company deducts the tax).
5. **The relief statement is the treaty text of the claim.** A row in `relief_statements.csv` is
   the treaty text that a Doh-Div row with a foreign tax and the row's country claims; the writer
   writes it as `ReliefStatement`. The row never shows that the investor is entitled to relief,
   and a row without foreign tax makes no claim (`false`). A country without a usable row is asked
   for a text. A row is used only for its own `income`: a Doh-Div row uses a `dividend` text, a
   Doh-Obr bond coupon a `bond_coupon` text and Doh-Obr cash interest a `cash_interest` text; a
   text of another kind is never used. A Doh-Obr row of a country without a text of its kind is
   asked about with a yes or no (`ReliefStatement` `DA`, the paragraph and article are filled in
   eDavki), once for the country's rows without a text.
6. **Broker payers** are used only with the broker they name. The payer of a broker's cash
   interest is its only row; a broker with no row or several (such as Interactive Brokers, whose
   export does not say which company holds the cash) has no known payer, and the payer facts stay
   empty with a warning.
7. **No coverage claim.** The files do not cover every issuer or ETF.

## How the files were made

The three upstream XML files were read once with an XML parser and written as the CSV files above:

- Text values were trimmed and kept otherwise unchanged; XML character references (for example a
  typographic apostrophe) became the character itself.
- Upstream order was kept.
- For `broker_payers.csv`, `broker` is `IBKR` and `code` is upstream's `code`.
- For `payers.csv`, the ticker (`symbol`) column was not copied, and the 138 entries with only a
  ticker (6 of them in the `conid` field) were kept apart. So was the entry with the ISIN that
  fails the check digit (its ISIN is not copied).
- The columns `instrument_kind` and `verified_on` were added, empty in every row.
- Upstream's `-` (meaning unknown) as the tax number of 3 payers became an empty field.
- One value was corrected: the country `IRL` of one payer (Johnson Controls International Plc)
  became `IE`, the 2-letter code every other row uses.
- Every row has `source` set to `ib-edavki` and `source_revision` set to the revision above, except
  the Krka row (see "Slovenian company" above): its tax number was corrected by hand, so its
  `source` is `manual` and its `source_revision` is empty.

After the ISIN lookup ("How the ISINs were found"), the `ibkr_conid` column was removed from
`payers.csv` and the column `isin_source` added. The rows keep upstream order: first the 299
payers that upstream gave an ISIN or a `conid`, then the 139 kept apart, each either in
`payers.csv` or in `payers_without_isin.csv`. 282 + 151 + 5 (rows with the same ISIN and facts,
dropped) = 438, the upstream count.

The check after the lookup: every row has as many fields as the header; every row of
`payers.csv` has an ISIN with a correct check digit, an `isin_source` and a 2-letter `country`.

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

- `payers.csv`: an ISIN of 2 letters, 9 letters or digits and a correct check digit in every row;
  an `isin_source` of `ib-edavki`, `wikidata`, `issuer`, `exchange` or `gleif`; a non-empty
  `name` and `address`; a `country` of 2
  uppercase letters; an `instrument_kind` that is empty or one of the four kinds; a `verified_on`
  that is empty or a calendar date; `source` `ib-edavki` or `manual`, and a `source_revision` for
  `ib-edavki`.
- `relief_statements.csv`: a 2-letter `country`, an `income` of `dividend`, `cash_interest` or
  `bond_coupon`, a non-empty `statement`, a source.
- `broker_payers.csv`: a non-empty `broker`, `code`, `name` and `address`, a 2-letter `country`, a
  source, and no broker and code twice.

A payer lookup takes an ISIN and returns one of three results:

| Result | When |
| --- | --- |
| Found | One row has the ISIN, or several rows do and agree on `name`, `tax_number`, `address`, `country` and `instrument_kind`. The result is the first such row in file order. |
| Conflict | Rows with the ISIN disagree on one of those facts. All rows are returned, in file order. |
| NotFound | No row has the ISIN. |

A Conflict is a skippable question where the user picks one of the rows, and NotFound gives an
income row without payer facts (rule 4). The lookup never matches a
ticker, a name or a country.
