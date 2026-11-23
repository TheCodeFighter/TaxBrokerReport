# IBKR fixtures

Every file here is **synthetic**: accounts `U0000000`, `U0000001` and `U0000002`, made-up names
and ISINs with valid check digits, no real value. The Interactive Brokers parser reads them, and
its tests take the expected result from the tables below, so a table row is one test. Dates are
weekdays that have an ECB rate; on every trade, `proceeds` = −`quantity` × `tradePrice` ×
`multiplier` (`tests/unit/ibkr_fixture_test.cpp` checks this). The bad rows of
`rejected_rows.xml` break these rules on purpose. `UTC` times are from the US Eastern `dateTime`
(`EST` = UTC−5, `EDT` = UTC−4). A cash row has a date and no time in these files, so it has no
timestamp.

| File | Purpose |
| --- | --- |
| `contract_example.xml` | One row of every kind in one file (overview, later a fuzz seed) |
| `supported.xml` | Only rows that become events |
| `ignored.xml` | Rows and sections that give no event |
| `rejected_rows.xml` | One valid buy and one bad row for each way a row is rejected |
| `corrections_2024.xml`, `corrections_2025.xml` | A dividend, its reversal and correction across two years |
| `conflicts_2024.xml`, `conflicts_2025.xml` | The same trade ID with different quantities in two files |
| `unknown_entity.xml` | An account whose `ibEntity` is not in the payer list |
| `payer_country_2024.xml` | `issuerCountryCode` on dividends, a reversal, taxes and a coupon: valid, lower case, empty, `XS`, three letters, different countries |
| `timestamps.xml` | Daylight-saving edges and an overnight trade |
| `quantities.xml` | Quantities with more than eight decimals |
| `file_level/` | Files that are not read as a Flex Query XML, and edge cases of the file |
| `equivalence/` | The same trades in a Trade Republic CSV and an IBKR XML |

# IBKR contract example

`contract_example.xml` is a **synthetic** Activity Flex Query XML file (accounts `U0000000` and
`U0000001`, made-up names and ISINs with valid check digits, no real value). It has one row for
each kind of row in the Interactive Brokers export contract (`docs/architecture.md`, "Interactive
Brokers export contract"), so the parser fixtures and the parser are built from it. On every trade,
`proceeds` = −`quantity` × `tradePrice` × `multiplier`.

The structure and the trade attributes are confirmed on a real export. The cash and corporate-action
attributes and their `type` values are not yet; they are corrected here and in the
contract when a real file shows a difference.

Account `U0000000` (`ibEntity` `IB-IE`):

| Element | `transactionID` | Kind | Expected event |
| --- | --- | --- | --- |
| `Trade` | 1001 | Buy of a share, fee in the trade currency | Buy 10 units at USD 150.25, fee USD 1.00 (fee currency USD), tax date 2024-03-05 |
| `Trade` | 1002 | Fractional sell (`FracShare`), fee in another currency, `notes` `P` | Sell 0.5 units at USD 200, fee EUR 0.35 (fee currency EUR) |
| `Lot` | 1001 | Closed lot of 1002, reuses the ID of buy 1001 | Ignored, one file-level notice; no duplicate-ID conflict with 1001 |
| `Trade` | 1003 | Buy of an ETF, `notes` `IA` | Buy 4 units at EUR 50, fee EUR 1.25, fund |
| `Trade` | 1004 | Option sale (`OPT`, no ISIN, multiplier 100) | Derivative: left out of every form; counted in the one margin and leverage warning |
| `Trade` | 1006 | Currency conversion (`CASH`) | Ignored, counted in one file-level notice |
| `Trade` | 1007 | Cancelled trade (`BUY (Ca.)`, `TradeCancel`) | Error on that row |
| `Trade` | 1008 | Bond buy, 1000 nominal at 99.5 % (multiplier 0.01) | Bond: outside Doh-KDVP, listed in `excludedItems` |
| `Trade` | 1009 | Fund buy (`FUND`), fee 0 | Buy 3 units at EUR 100, fund |
| `Trade` | 1010 | Share buy with a financial transaction tax (`taxes` −0.8) | Buy 10 units at EUR 20, fee EUR 1.25 and a second fee EUR 0.80 |
| `Trade` | 1011 | Corrected trade (`TradeCorrect`) | Error on that row |
| `CashTransaction` | 2001, 2002, 2013 | Dividend (`A9001`), its withholding, a withholding refund booked in 2025 | One dividend, gross USD 12.00, tax USD 1.50 (1.80 − 0.30), 2024-03-15; warning: correction booked in a later year |
| `CashTransaction` | 2003 to 2005 | Dividend, its reversal and the corrected dividend (`A9002`), all booked in 2024 | One dividend, gross USD 4.50 (5.00 − 5.00 + 4.50), no tax, 2024-05-10 |
| `CashTransaction` | 2006 | Payment in lieu of dividends | Dividend USD 3.00 with a warning |
| `CashTransaction` | 2007 | Broker interest received | Interest EUR 3.21, broker cash, payer `IB-IE` |
| `CashTransaction` | 2008 | Broker interest paid (margin) | Left out; counted in the same warning (one warning for 1004 and 2008: 2 rows) |
| `CashTransaction` | 2009, 2010 | Bond coupon, accrued interest paid at the bond buy 1008 | Coupon EUR 45.00 (4.5 % of 1000, Doh-Obr); accrued-interest warning |
| `CashTransaction` | 2011, 2012 | Fee, deposit | Ignored (fee summary only) |
| `CorporateAction` | 3001 | Forward split 4 for 1 (`FS`), same ISIN: 9.5 units → 38 | Unresolved split, ratio asked |
| `CorporateAction` | 3004 | Reverse split 1 for 4 (`RS`), same ISIN: 4 units → 1 | Unresolved split, ratio asked |
| `CorporateAction` | 3002 | Merger (`TC`) | `unsupported_corporate_action` |
| `CorporateAction` | 3003 | Bond maturity (`BM`) | Redemption event, information only |
| `Transfer` | 4001, 4002 | Position transfers in and out | Section not read: no event, no diagnostic |

Account `U0000001` (`ibEntity` `IB-CE`), the same taxpayer:

| Element | `transactionID` | Kind | Expected event |
| --- | --- | --- | --- |
| `Trade` | 5001 | Buy of the same share as 1001 | Buy 2 units at USD 160; the same FIFO pool as account `U0000000` |
| `CorporateAction` | 5002 | The same split as 3001 (`S9001`): 2 units → 8 | One split for the ISIN, asked once |

Not in the example: `CRYPTO` and `CMDTY` trades. IBKR writes them without an ISIN, and the app
never makes one up. A crypto trade without an ISIN is not read and leaves one notice
(`unsupported_row_type.crypto_without_isin`); a `CMDTY` trade without an ISIN is a `missing_field`
error on `isin`. The parser tests write these rows themselves.

# `supported.xml`

Two accounts, one taxpayer. Every row gives the event below. Amounts, fees and tax are stored
positive. Fee 0 means no fee.

Trades:

| `transactionID` | Event | ISIN | Units | Price | Currency | Fee (currency) | Tax date | UTC timestamp |
| --- | --- | --- | ---: | ---: | --- | --- | --- | --- |
| 1001 | Buy, stock | US0000000010 | 10 | 150.25 | USD | 1.00 (USD) | 2024-03-05 | 2024-03-05T20:42:10Z |
| 1002 | Sell, stock, `FracShare`, `notes` `P` | US0000000010 | 0.5 | 200 | USD | 0.35 (EUR) | 2024-06-10 | 2024-06-10T14:15:00Z |
| 1003 | Buy, fund (`STK` `ETF`) | IE0000000020 | 4 | 50 | EUR | 1.25 (EUR) | 2024-04-02 | 2024-04-02T08:00:00Z |
| 1008 | Buy, bond (`BOND`, `multiplier` 0.01) | DE0000000041 | 1000 | 99.5 | EUR | 2.00 (EUR) | 2024-03-20 | 2024-03-20T09:00:00Z |
| 1009 | Buy, fund (`FUND`) | LU0000000066 | 3 | 100 | EUR | 0 (EUR) | 2024-04-15 | 2024-04-15T10:00:00Z |
| 1010 | Buy, stock, `taxes` −0.8 | FR0000000077 | 10 | 20 | EUR | 1.25 (EUR) and a second fee 0.80 (EUR) | 2024-04-16 | 2024-04-16T08:30:00Z |
| 5001 (`U0000001`) | Buy, stock, same ISIN as 1001 | US0000000010 | 2 | 160 | USD | 1.00 (USD) | 2024-07-01 | 2024-07-01T15:30:00Z |

The bond 1008 is class Bond (outside Doh-KDVP); its amount is EUR 995.00 (1000 × 99.5 × 0.01). The
units are the nominal (1000) and the unit price is `tradePrice` × `multiplier` = 0.995, as for a
Trade Republic bond. Trade 5001 is in the
same FIFO pool as 1001 and 1002.

Cash rows (one event per row; `reportDate` is not a tax date):

| `transactionID` | Event | ISIN | Amount | Currency | Tax date | Payment group ID | Cash holder | Payer code |
| --- | --- | --- | ---: | --- | --- | --- | --- | --- |
| 2001 | Dividend, gross | US0000000010 | 12.00 | USD | 2024-03-15 | `A9001` | none | none |
| 2002 | Tax of the dividend 2001 (the row is −1.80) | US0000000010 | 1.80 | USD | 2024-03-15 | `A9001` | none | none |
| 2006 | Dividend (payment in lieu), with a warning | US0000000010 | 3.00 | USD | 2024-06-14 | `A9003` | none | none |
| 2007 | Interest on cash | none | 3.21 | EUR | 2024-04-03 | none | Broker | `IB-IE` |
| 2009 | Bond coupon (4.5 % of 1000) | DE0000000041 | 45.00 | EUR | 2024-06-17 | none | none | none (issuer, by ISIN) |

A missing fact is not a zero. Trades with `taxes="0"` or `ibCommission="0"` have a known tax or
fee of 0. Dividend 2006 has no tax row at all: its tax is blank (no foreign tax paid), kept as
blank, and gives no warning. Dividend 2001 has its tax row 2002 (1.80). Interest 2007 and coupon
2009 have no tax row either (blank).

Row 2010 (accrued interest paid, −12.50 EUR, 2024-03-20, for bond 1008) gives no income event; the
existing accrued-interest warning is its only result.

Corporate actions:

| `transactionID` | Event | ISIN | Tax date | UTC timestamp | Evidence |
| --- | --- | --- | --- | --- | --- |
| 3001 (`U0000000`), 5002 (`U0000001`) | Split, ratio open: asked once for the ISIN (same `actionID` `S9001`) | US0000000010 | 2024-08-01 | 2024-08-02T00:25:00Z | `quantity` 28.5 and 6, `actionDescription` "SPLIT 4 FOR 1": evidence only, never the ratio |
| 3004 | Reverse split, ratio open | IE0000000020 | 2024-10-14 | 2024-10-15T00:25:00Z | `quantity` −3, "SPLIT 1 FOR 4": evidence only |
| 3003 | Bond maturity, information only (units 1000, amount EUR 1000.00) | DE0000000041 | 2024-12-16 | 2024-12-17T01:25:00Z | none |

The UTC date of a corporate action is the day after its tax date: this is normal for IBKR and gives
no `inconsistent_value` warning.

# `ignored.xml`

Result: **0 events**, and these diagnostics only.

| Row | Why | Diagnostic |
| --- | --- | --- |
| `Lot` 1001 (`CLOSED_LOT`), `Order` 1021 (`ORDER`) | Not an execution | One file-level notice (severity `info`), count 2 |
| `Trade` 1006 (`CASH`, `EUR.USD`) | Currency conversion | One file-level notice (severity `info`), count 1 |
| `Trade` 1004 (`OPT`), 1022 (`FUT`), 1023 (`CFD`), `CashTransaction` 2008 (`Broker Interest Paid`) | Margin and leverage | One warning, count 4, never blocking |
| `CashTransaction` 2011 (`Other Fees`), 2012 (`Deposits/Withdrawals`) | Ignored cash rows | None |
| `Transfers`, `OpenPositions`, `CashReport` sections | Sections that are not read | None |

# `rejected_rows.xml`

Each bad row has exactly one error, and a row with several faults gets the first check that
fails, in this order: cancellation, missing attribute, field format, signs. So 8002 (which also
has a wrong sign and a positive fee) gets only the cancelled-row error.

Result: **1 event**, the valid buy 8001 (5 units at USD 20, fee USD 1.00, 2024-07-01, UTC
2024-07-01T18:00:00Z), and one error on each other row, which is not read. The file has no other
diagnostic. A cancelled row is `unsupported_row_type.cancelled_trade`, and its `field` is the
attribute that shows it (`buySell`, `transactionType` or `notes`).

| `transactionID` | What is wrong | Error |
| --- | --- | --- |
| 8002 | `buySell` `BUY (Ca.)`, `transactionType` `TradeCancel`, `notes` `Ca` | `unsupported_row_type`, field `buySell` |
| 8003 | `transactionType` `TradeCorrect` | `unsupported_row_type`, field `transactionType` |
| 8004 | `notes` `Ca;P` (code `Ca` among others) | `unsupported_row_type`, field `notes` |
| 8005 | `buySell` `BUY` with `quantity` −5 and `proceeds` 100 | `inconsistent_value` (sign, as for Trade Republic) |
| 8006 | Stock row without the `isin` attribute | `invalid_header` (a missing attribute) |
| 8007 | `tradeDate` `2024-03-05` | `invalid_value`, field `tradeDate` |
| 8008 | `dateTime` `20240305 154210` (space, no `;`) | `invalid_value`, field `dateTime` |
| 8009 | `currency` `XXX` | `invalid_value`, field `currency` |
| 8010 | `quantity` `1,5` | `invalid_value`, field `quantity` |
| 8011 | `proceeds` empty | `missing_field`, field `proceeds` |

# `corrections_2024.xml` and `corrections_2025.xml`

The two files are read together, as two runs of the same account. The 2025 file starts on
2024-11-01, so the buy 6001 is in both.

| File | Rows |
| --- | --- |
| 2024 | Buy 6001 (3 units of US0000000036 at USD 20, 2024-11-12). Dividend 2101 `A9101` USD 10.00 and its tax 2102 −1.50, both paid and booked 2024-11-15 |
| 2025 | Buy 6001 again. For `A9101`, booked 2025-01-15 with pay date 2024-11-15: reversal 2103 −10.00, corrected dividend 2104 9.00, tax refund 2105 +0.15. Reversal 2106 −4.00 of `A9102` (US0000000010, pay date 2024-12-03, booked 2025-01-10), whose dividend is in no file |

Result after the merger and the netting step:

| Result | Value |
| --- | --- |
| Buy 6001 | Once (the exact duplicate is dropped) |
| Dividend of `A9101` | One, US0000000036, tax date 2024-11-15 (the pay date, in 2024), gross USD 9.00 (10.00 − 10.00 + 9.00), tax USD 1.35 (1.50 − 0.15) |
| Warning | A correction booked in a later year (2025) than its pay date (2024) |
| `A9102` | No dividend; one warning, "reversal without its dividend" (not also "net of zero or less": that rule is for a payment that has a dividend row) |

# `conflicts_2024.xml` and `conflicts_2025.xml`

Two files of the same account. The 2025 file starts on 2024-11-01, so trade 6101 is in both, but
with different facts.

| File | Trade 6101 |
| --- | --- |
| 2024 | Buy of 3 units of US0000000036 at USD 20, `proceeds` −60, 2024-11-12 |
| 2025 | Buy of 4 units of US0000000036 at USD 20, `proceeds` −80, 2024-11-12 |

Result: one `conflicting_duplicate` error for trade 6101 with both sources kept; neither version is
used as an event (the whole group is rejected). This is the same rule as for Trade Republic.

# `unknown_entity.xml`

Account `U0000002` has `ibEntity` `IB-XX`, which is not in `broker_payers.csv`. Its one row is broker
interest received 2301 (EUR 1.75, 2024-05-03, cash holder Broker).

Result: 1 interest event with empty payer facts, and one warning that the entity is unknown (the row is
never rejected and never asks a question).

# `payer_country_2024.xml`

Account `U0000000` (`ibEntity` `IB-IE`), no trades. The country of the payer is the
`issuerCountryCode` of a dividend, reversal or coupon row, in capital letters when it is an ISO 3166-1
country code, otherwise none (no diagnostic). The country of a tax row is read too, but the netting
step ignores it.

| `transactionID` | Row | ISIN | `issuerCountryCode` | Date | Expected |
| --- | --- | --- | --- | --- | --- |
| 2401, 2402 | Dividend USD 20.00 (`A9401`) and its tax USD 3.00 | US0000000010 | `US` | 2024-04-10 | One dividend, country `US` |
| 2403 | Dividend EUR 8.00 (`A9402`) | NL0000000081 | empty | 2024-05-10 | Country none |
| 2404 | Dividend EUR 6.00 (`A9403`) | FR0000000010 | `XS` | 2024-06-10 | Country none (`XS` is no country) |
| 2405, 2406 | Dividend EUR 10.00 and tax EUR 2.64 (`A9404`) | DE0000000033 | `DE` / `CA` | 2024-06-20 | One dividend, country `DE` (the tax row's `CA` is ignored) |
| 2409 | Bond coupon EUR 30.00 | IT0000000007 | `IT` | 2024-07-01 | Coupon, country `IT` |
| 2407, 2408 | Dividend EUR 5.00 and reversal EUR 1.00 (`A9405`) | DE0000000058 | `DE` / `AT` | 2024-07-15 | `income_payment_rows_disagree` error, no dividend |
| 2410 | Dividend EUR 4.00 (`A9406`) | CH0000000015 | `ch` | 2024-08-12 | Country `CH` (upper-cased) |
| 2411 | Dividend EUR 3.00 (`A9407`) | BE0000000001 | `USA` | 2024-09-10 | Country none (three letters) |

# `timestamps.xml`

Six buys of 1 unit at USD 100 of US0000000010, fee USD 1.00. The reasons are the US daylight-saving
rule (second Sunday of March, first Sunday of November).

| `transactionID` | `tradeDate` | `dateTime` | Tax date | UTC timestamp | Why |
| --- | --- | --- | --- | --- | --- |
| 9001 | 20240305 | 20240305;094210 | 2024-03-05 | 2024-03-05T14:42:10Z | EST |
| 9002 | 20240308 | 20240308;155959 | 2024-03-08 | 2024-03-08T20:59:59Z | Last Friday before EDT starts |
| 9003 | 20240311 | 20240311;093000 | 2024-03-11 | 2024-03-11T13:30:00Z | EDT |
| 9004 | 20240306 | 20240305;203000 | 2024-03-06 | 2024-03-06T01:30:00Z | Overnight trade: IBKR gives the next day as `tradeDate`; the tax date is `tradeDate` |
| 9005 | 20241101 | 20241101;160000 | 2024-11-01 | 2024-11-01T20:00:00Z | EDT |
| 9006 | 20241104 | 20241104;093000 | 2024-11-04 | 2024-11-04T14:30:00Z | EST |

The switch instants have no trade here. They are cases of the time converter:

| `dateTime` | UTC |
| --- | --- |
| 20240310;015959 | 2024-03-10T06:59:59Z |
| 20240310;030000 | 2024-03-10T07:00:00Z |
| 20241103;013000 | 2024-11-03T05:30:00Z (the repeated hour is read as EDT) |
| 20241103;020000 | 2024-11-03T07:00:00Z |
| 20240310;023000 | `invalid_value` (this hour does not exist in US Eastern time) |

# `quantities.xml`

Units are stored with 8 decimals (half away from zero). The original quantity text stays as
evidence, with the digits that were discarded (`docs/fifo.md`, "Rounding differences and inventory
balance"). ISIN NL0000000081, EUR.

| `transactionID` | Event | Original text | Units | Discarded digits |
| --- | --- | --- | ---: | ---: |
| 9101, 9102, 9103 | Buy at EUR 30 | `0.333333334` | 0.33333333 | 0.000000004 |
| 9104 | Sell at EUR 35 | `-1` | 1 | none |
| 9105 | Buy at EUR 10 | `0.1234567890123456789012345678901` (31 decimals) | 0.12345679 | not usable: more than 30 decimals |

The three buys sum to 0.99999999 units, and their original texts to 1.000000002, which covers the
sale 9104 (`docs/fifo.md`). Trade 9105 has no usable source decimal: nothing is proved from it.

# `file_level/`

| File | Content | Result |
| --- | --- | --- |
| `flex_query.csv` | The Flex Query as CSV | One file-level `invalid_header` error with the hint to export XML; 0 events |
| `activity_statement.csv` | An Activity Statement (CSV) | Same |
| `wrong_root.xml` | Root `FlexStatementResponse` (the web service reply, not the report) | Same |
| `truncated.xml` | A valid file cut off inside `Trades` (not well-formed XML) | Same |
| `doctype_entity.xml` | A DOCTYPE that declares an external entity and uses it | One file-level `invalid_header` error (`invalid_header.doctype_not_allowed`); the entity is never read (the file it names does not exist) |
| `empty_statements.xml` | `FlexStatements count="0"` | 0 events, no diagnostic |
| `bom.xml` | A valid file with a UTF-8 byte order mark | Read as normal: 1 buy (10 units of US0000000010 at USD 150.25, fee USD 1.00) |

# `equivalence/`

`tr.csv` (Trade Republic) and `ibkr.xml` (IBKR) hold the same trades. After the parser, each pair
of events is equal apart from broker, transaction ID and source (file, row). Names are not
compared. The interest payer differs too: it comes from the broker (Trade Republic) or from the
account's `ibEntity` (IBKR, `IB-IE`). So does the payer country of a dividend: Trade Republic has
none, IBKR gives `FR` for the euro dividend and `US` for the dollar dividend (`issuerCountryCode`).

| Event | Trade Republic row | IBKR row | Equal fields |
| --- | --- | --- | --- |
| Buy | `equivalence-buy-001` | 7001 | FR0000000077, 5 units at EUR 20, fee EUR 1.00, 2024-03-05, 2024-03-05T14:42:10Z |
| Sell | `equivalence-sell-001` | 7002 | FR0000000077, 2 units at EUR 25, fee EUR 1.00, 2024-06-10, 2024-06-10T14:15:00Z |
| Dividend with tax | `equivalence-dividend-eur-001` | 7003 and 7004 | FR0000000077, gross EUR 10.00, tax EUR 1.50, 2024-04-15 (after netting; Trade Republic has a timestamp, IBKR has none) |
| Dividend in USD | `equivalence-dividend-usd-001` | 7005 | US0000000010, gross USD 10.00, no tax, 2024-05-16 (Trade Republic also keeps its EUR rate 0.92 and has a timestamp; IBKR has neither) |
| Interest | `equivalence-interest-001` | 7006 | EUR 2.50, 2023-11-15, cash holder Broker (before 2023-12-06 Trade Republic interest is also on broker cash; Trade Republic has a timestamp, IBKR has none) |

A Trade Republic dividend has the tax in EUR, so a USD dividend with a tax would not be equal; the
USD dividend therefore has none.
