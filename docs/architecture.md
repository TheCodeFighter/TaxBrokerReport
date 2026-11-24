# Architecture

TaxBrokerReport is intended to run entirely on the user's machine. The C++20 backend owns broker
parsing, validation, tax processing, and XML generation. A browser frontend (Preact, see
"Frontend" below) calls the local backend over HTTP on `127.0.0.1`. Domain results stay
independent of JSON and HTTP: only `taxbroker_api` and the server know the wire format.

## Public API and internal libraries

The application service is the only public API of the backend. The command line, the local
server and, later, a Python binding reach the backend through it and nothing else. The folders
show the rule:

- `include/taxbroker/` holds the public headers: the service (`application_run.hpp`), its request,
  result and decision session, the diagnostics and messages, the input file type, the report
  models, the shared types, the JSON of the result (`api/`), and two small helpers:
  `text_format.hpp` (date and fixed-point text and the strict number import, shared with the
  frontend later, so a date or an amount is read and shown the same way everywhere) and
  `logging.hpp` (the logger and the `LOG_*` macros). A public header names no stage, parser or
  writer.
- `src/parsers/`, `src/processors/`, `src/xml/` and `src/utils/` hold the headers of the
  stages, beside their sources. A type that the result shows stays in a public header; the stage
  part moved out of a mixed header: the statement merger (`processors/statement_merger.hpp`), the
  reference file loader (`parsers/reference_catalogue.hpp`) and the ECB rate file loader
  (`parsers/ecb_rate_file.hpp`).
- `src/cli/` and `src/server/` hold the headers of the command line and the server (the request
  guard and the launch file), beside their sources.

`src/` is a private include folder of the backend libraries (`taxbroker_core`, `taxbroker_xml`,
`taxbroker_service`, `taxbroker_api`) and of the tests and debug tools, which may use internals.
The command line gets only `include/` (through the service) and its own `src/cli/`; the server
gets only `include/`. The service links `taxbroker_core` privately.

Two checks keep the rule, both in the normal build and test run:

- `taxbroker_public_header_check` compiles every public header alone with only `include/`, so a
  public header cannot need an internal one.
- `PublicApiBoundary.CliCannotIncludeInternal` and `PublicApiBoundary.ServerCannotIncludeInternal`
  build a file that includes an internal header, with the include folders and libraries read from
  the real `taxbroker_cli` and `taxbroker_server` targets. They pass only when the compiler
  reports that header as missing. They have the CTest label `boundary`, and CI runs them in
  the build job after the build tree is uploaded, where the tree is fresh (a restored tree would
  be rebuilt by them).

## Imported event metadata

Every imported trade, corporate action, dividend, interest payment, benefit, private-market event
and bond maturity owns the same `EventMetadata`:

- `mTaxDate` is the broker-provided calendar date used for tax reporting. It is not derived from
  the source timestamp.
- `mSourceTimestamp` is an optional UTC instant with millisecond precision. Fractions finer than
  milliseconds are truncated.
- `mOrderingTimestamp` optionally supplies an explicitly assumed ordering instant. When absent,
  ordering uses `mSourceTimestamp`. Keep the original source instant for overlap/conflict checks.
- `mSources` contains every contributing `SourceReference`, each with the broker, source filename,
  source row, optional transaction ID, and stable input sequence. A parsed event starts with one
  source. An exact duplicate adds its source to the retained event instead of replacing provenance.

These types depend only on the C++ standard library. `SourceFilename::fromPath` removes both POSIX
and Windows directory components and rejects empty, `.` and `..` basenames, so event metadata
cannot retain an absolute host path. Source rows are one-based logical rows; for CSV input, the
header is row 1 and the first data row is row 2. A logical row is one CSV record. A quoted field
that spans several text lines is still one record, so a row number can differ from the line number
shown in a text editor.

A CSV record whose field count differs from the header is rejected with a `field_count_mismatch`
error on its own row, before any field is read. Later rows keep their correct numbers. A file with
an unclosed quote is rejected as a whole with a file-level `parse_error`, because the reader would
otherwise merge the following records into one field without any error. The check counts quote
characters, so two unclosed quotes in one file cancel out. The reader then merges rows into one
record. Such a record has the wrong field count and a line break inside a field, so the whole file
is rejected with a file-level `parse_error`. A quoted line break inside a record with the right
field count is valid and stays one row.

The header is checked before any row is read. The Trade Republic parser needs these 18 columns:
`datetime`, `date`, `category`, `type`, `asset_class`, `name`, `symbol`, `shares`, `price`,
`amount`, `fee`, `tax`, `currency`, `original_amount`, `original_currency`, `fx_rate`,
`description` and `transaction_id`. The other export columns are not used and may be missing, and
extra columns are allowed. A header that misses a required column, for example because the file
was saved with semicolons, is rejected with one file-level `invalid_header` error and no rows are
read. A file of 0 bytes stays a `parse_error`.

**Input from memory.** The parser core takes the CSV text, a display file name and the source index
(`CsvParser::parse(text, displayFilename, sourceIndex)`), so an upload never has to be written to a
disk file. `CsvParser::parseFile(path, sourceIndex)` is the wrapper for the CLI and the tools: it
reads the file and calls `parse`. Nothing in the parser throws for bad input. A display name that
is empty, `.`, `..` or ends in a separator gives one file-level `parse_error`
(`parse_error.invalid_filename`, no file name in the diagnostic); a name with a folder part keeps
its last part. `parseFile` reads regular files only, so a pipe is an unreadable file. The ECB rate files and
the reference files have both forms too (`loadEcbRateFiles`, `loadOfficialRates` and
`loadReferenceData` take a list of `InputFile` (display name and bytes) as well as a list of
paths). A rate file without a valid name gives the same `parse_error.invalid_filename`; the
reference loader reports a name that is not one of its three files as `ReferenceFileUnknownName`. The taxpayer file already has both forms
(`api::parseTaxpayerJson` and `cli::readTaxpayerFile`).

**Size limit and memory.** A user file is at most 100 MB (`kMaxInputFileBytes`,
`include/taxbroker/input_file.hpp`); a bigger file is a wrong file. The path wrappers check the size
on disk before they read, the memory forms check the size of the text, and a file over the limit
is not read: it gives the file-level error `input_file_too_large` (`input_file.too_large`,
parameter `limitMb`). The rest of the processing needs all events in memory, so a stream would
not save memory. If the memory for one file still runs out (`std::bad_alloc`), that file gives
`input_file_out_of_memory` (`input_file.out_of_memory`), never a crash. Each of the two writes one
error log line with the input kind, the source index and the code (no path, file name or size).

A row with a rejected field is skipped, and every invalid field of that row gets its own
diagnostic, in the column order of the export. A sign check (shares or amount against the trade
side) runs after the field checks and only on valid fields. A missing `datetime` is reported
first, and the other fields of that row are still checked.

For a valid trade with an `amount`, the parser also compares `|amount|` with
`|price × shares|` (for a trade with `original_currency`, `original_amount` in that currency). A
difference above EUR 0.01 adds a `trade_amount_mismatch` warning and keeps the row; `amount` stays
informational. See `diagnostics.md`.

Broker numbers must be plain dot decimals (see `calculations.md`). A comma or parentheses makes the
field `invalid_value` and skips the row. From 3 such rows in one file, the parser adds one
file-level `localized_number_format` warning that suggests downloading the export again. A row
counts once, even if several of its fields have a comma.

An empty line after the header is never a record and gets no row number, however many follow each
other. A line with only spaces or commas is a record and goes through the normal row checks.
Trade Republic exports contain neither.

Different selected files may have the same sanitized filename. The application therefore derives
a run-local source ID from the file's request position: `source-0001`, `source-0002`, and so on.
The frontend keeps the selected-file list in that same order. When filenames collide, it shows a
label such as `taxreport.csv (file 2)` instead of exposing either file's directory. The source ID
identifies an upload only within that report run and does not change event ordering or duplicate
detection.

When duplicate names would still be confusing, the frontend may let the user add a run-local
display label such as `2022` or `pension account`. The frontend keeps that label locally with the
source ID and may show `taxreport.csv (2022, file 1)`. It does not send the label as broker data,
use it for processing, or replace the source ID with it.

Trade Republic requires a non-empty `datetime` for imported events. A missing value produces a
`MissingField` error on `datetime` and excludes that row before instrument or event creation.
The diagnostic retains the source file, row and transaction ID for the user to review. Other
valid rows remain available. Ignored cash movements do not require a timestamp.

An invalid non-empty TR timestamp retains the otherwise valid event, leaves `mSourceTimestamp`
empty, and reports an `InvalidValue` warning that exact transaction order is unknown. The tax
date remains unchanged.

When a valid TR timestamp's original calendar date differs from `date`, report an
`InconsistentValue` warning on `datetime`. Retain `date` for reporting and set
`mOrderingTimestamp` to that date at 09:00:00 UTC. The hour is defined by
`kTrDateMismatchDefaultHour` in `traderepublic_parser.hpp`. The warning explicitly states
that the default hour is assumed and FIFO/corporate-action order may be incorrect. Preserve the
original `mSourceTimestamp`, so equal fallback times cannot hide conflicting exports.
Compare before UTC normalization: crossing midnight while normalizing an offset is not itself
a mismatch. Do not silently discard purchases or replace the broker tax date.

### Trade Republic income, trades and optional broker FX

Dividends, broker cash interest and bond interest share income validation. When
`original_currency` is supplied, preserve `original_amount` in that currency; otherwise use
`amount` and `currency`. Do not replace an invalid original amount with the local amount.
Preserve the tax date, source timestamp and complete source reference alongside the income.
Withholding remains separate in `currency`. A blank tax stays blank in the parsed event (absent) and
counts as no tax paid when the income form is built; an explicit zero is a zero.

An interest row that is neither cash interest (blank class, symbol and name) nor a `BOND` coupon is
kept as an interest payment of unknown type (`InterestType::UnknownInterest`, the ISIN only when
the `symbol` is filled, the class label as evidence, no cash holder) and keeps the error
`unsupported_asset_class.interest_mapping`. The income classification stage asks what it is and
moves it to the form of the answer (`tax_rules.md`).

An empty `fx_rate` never rejects otherwise valid foreign income. Store no broker rate, including
when the foreign payment is supplied directly in `amount`/`currency`. Do not invent a unit rate
or infer a rate from the local and original amounts. Direct EUR income without broker FX keeps
its existing identity conversion; it does not establish a foreign-currency fallback.

A supplied `fx_rate` is validated even without `original_currency`. Malformed, whitespace-only,
zero, negative, out-of-range or nonzero values rounded to zero produce an `InvalidValue` error
on `fx_rate` and skip that row. The diagnostic retains filename, row and transaction ID without
repeating the value. Other valid rows and their diagnostics remain available through merging.
Valid source rates retain the established eight-decimal import rounding; a positive rate that
rounds to one rate tick is representable. A stored broker rate also carries its direction and how
that direction is known (`BrokerExchangeRate`). Trade Republic `fx_rate` is EUR per 1 foreign
unit. The direction is verified only for a row with a foreign `original_currency` settled in EUR
(export samples for income, the maintainer's decision for trades) and for the identity rate of a
plain EUR row. Any other supplied rate, such as a foreign amount directly in `amount`, is kept as
unverified and cannot convert an amount.

Required income facts remain required even when broker FX is absent. Empty fields produce
`MissingField`; invalid supplied fields produce `InvalidValue`. Trade Republic's required
`datetime` policy above applies to income as well.

A trade follows the same broker-rate rule. A trade gets a broker rate only from `fx_rate`, except
the identity rate 1.0 for a plain EUR trade without `fx_rate`; any other trade without `fx_rate`
has no broker rate. When `original_currency` is supplied, the unit price and `original_amount` are
in that currency and the fee stays in `currency`; the trade keeps the fee currency separately.
Trade Republic trades in EUR today, so this shape is assumed from foreign income, not verified
from a foreign trade sample. With `original_currency`, `original_amount` is required: an empty
value is `MissingField` and skips the row. An invalid supplied `fx_rate`, `original_currency` or
`original_amount` is an `InvalidValue` error on that field and skips the row. Blocking foreign
trades from a report is a later processor decision.

For a trade with `original_currency`, a supplied `amount` must still be a valid number; it is not
stored. An invalid value is an `InvalidValue` error on `amount` and skips the row. An empty
`amount` stays allowed. Foreign income stays lenient: it ignores `amount` when `original_currency`
is supplied.

### Trade Republic bond rows

A bond leaves several kinds of rows in a Trade Republic export. The shapes marked **verified** were
checked against a real export of one USD bond (outside user source); only the shapes were kept, never
its values. The rest are **guesses**, marked `GUESS` in the parser and its tests, and must be
checked against a real export before the beta.
`tests/test_data/csv/README.md` explains the synthetic fixture that copies the verified shapes.

| Row | Status | What the parser does |
| --- | --- | --- |
| `TRADING` `BUY`, asset class `BOND` | Verified. `shares` is the nominal (may be a fraction), `price` is EUR per 1 nominal of the bond's currency (below 1 because of the exchange rate, not because the bond is under par); `amount` is higher than `shares` × `price` because it also has the accrued interest; one purchase can be two rows a few milliseconds apart | A normal trade of a bond, with a `trade_amount_mismatch` warning on each row (the row is kept); sales are blocked by the class error. |
| `CASH` `INTEREST_PAYMENT`, `BOND` | Verified: `shares`, `price`, `fee` and `tax` blank; `amount` in EUR; for a foreign bond `original_amount`, `original_currency` and `fx_rate` are filled | A bond coupon (Doh-Obr later). The last coupon is its own row on the maturity day. |
| `CASH` `INTEREST_PAYMENT` without an asset class, name and symbol | Verified: `shares`, `price` and `fee` blank; `amount` in EUR. A negative `amount` (debit interest) is unconfirmed: no real row seen yet | Interest on cash. A negative `amount` is read with its sign (no `invalid_value`); the interest stage leaves such a payment out with a warning. A negative dividend, coupon or unknown-type interest amount stays `invalid_value`. |
| `CORPORATE_ACTION` `FINAL_MATURITY`, `BOND` | Verified: `shares` negative (the whole nominal); `price`, `amount` and `currency` blank | One `BondRedemptionEvent`, kind `Maturity`, with the units. |
| `CASH` `FINAL_MATURITY`, `BOND`, positive `amount` | Verified: `shares` **blank**; `amount` in EUR; a foreign payout has `original_amount`, `original_currency` and `fx_rate` | Kind `Payment`: the amount (in the original currency when there is one), the broker rate, fee and tax. |
| `CORPORATE_ACTION` `FINAL_MATURITY_CANCELLED`, `BOND` | Verified: `shares` positive, the size of the maturity; `date` is the maturity day, `datetime` the correction day | Kind `MaturityCancelled`, with the units. |
| `CASH` `FINAL_MATURITY`, `BOND`, negative `amount` | Verified: the cancelled payout; only the sign of `amount` and the description say so | Kind `PaymentReversal`, the amount without its sign. |
| `CORPORATE_ACTION` `REDEMPTION`, `BOND` | **Guess**: the same as `FINAL_MATURITY` (seen in other tools) | Kind `Maturity`. |
| `CASH` `FINAL_MATURITY` with filled `shares` | **Guess**: positive units on a payment | Kind `Payment` with the units; negative units are `inconsistent_value`. |
| `CASH` `SEC_ACCOUNT`, `BOND` | **Guess**: no amount, only `tax` (negative withheld, positive refunded) | Not read yet: `unknown_row_type`. Planned with the bond treatment. |
| `CASH` `TILG` (*Tilgung*, repayment) | Unknown shape | `unsupported_row_type`, not read as a maturity. |

Rules:

- The action rows carry the units and the cash rows carry the amount. Every event has positive
  numbers; the kind carries the direction. A real maturity can be cancelled and booked again days
  later, after a payout in the wrong currency: units out, units back, units out again, and a
  payout, the payout taken back, a new payout. The parser keeps all rows as events and adds nothing
  up (the bond list, per ISIN and by sum: the correction rows keep the maturity
  `date` but have another `datetime`, so they sort at the default hour of that day, before the
  maturity they cancel).
- `amount` is the whole payout. For a discount or zero-coupon bond it contains interest; the
  event does not split it, and the tax treatment stays `unsupported_tax_treatment` until the bond
  treatment is built.
- A cash row with `original_currency` stores `original_amount` in that currency and the broker
  rate (EUR per 1 foreign unit, verified by the source samples), as the coupon rows do. The EUR
  `amount` must be a valid number (blank is `missing_field`, a bad value `invalid_value`) but is
  not stored. A payout and its reversal can have
  different currencies.
- Only the class `BOND` gives an event. A maturity row of another class (a certificate, a
  derivative) is rejected with `unsupported_asset_class.redemption_not_bond` and keeps the capital
  scope, because it can be a taxable sale.
- An action row needs non-zero units (sign by kind, `inconsistent_value` otherwise). A cash row
  may leave `shares` blank; a zero amount is invalid; a reversal has no units.
- A filled `price` on an action row is stored as the redemption price (a positive number, else
  `invalid_value`); this is a guess, because the real action rows leave it blank. A value in
  another column the row does not use (on an action row: `amount`, `fee`, `tax`, `currency`,
  `original_amount`, `original_currency`, `fx_rate`; on a cash row: `price`) is not stored and gets
  an `inconsistent_value.redemption_unused_field` warning, so it is never dropped silently. A
  row skipped for another error gets no such warning.
- The payout of a corrected maturity is not the nominal exactly (a real correction paid one cent
  less), so the bond list must never assume payout = nominal.

The events are information only. They are not a sale, they never reach Doh-KDVP, and the FIFO, the
loss rule and the corporate-action stage do not read them. The classification stage adds them up
per bond into the excluded items (see "Classification" below); the reporting of bond interest only
(Doh-Obr) builds on them later.

### Interactive Brokers export contract

Status: **contract, partly verified on a real export.** The file structure, the account and trade
attributes, the date format, the time zone, the signs and the closed-lot rows were checked on a
real export (names, counts and shapes only, never values). That export holds only buys and sells of
exchange-traded funds in EUR, so the cash-transaction and corporate-action attributes and
their `type` values come from the column names IBKR writes for the same query and from public
descriptions; they are confirmed on the first real file that has such rows, and a name that differs
is corrected here. The parser (`src/parsers/ibkr_parser.cpp`, its own target `taxbroker_ibkr`) reads
every row of the fixtures in `tests/test_data/ibkr/` and gives the events and diagnostics below. The
steps after it (adding up the rows of a payment, the one margin and leverage warning, the payer by
entity code) are not written yet, and no file kind selects this parser in the CLI or the browser.

**The export.** One **Activity Flex Query** in **XML** format, one file per run, one run per
calendar year (overlapping files are fine; the longest period of one run is unconfirmed, and a
leap year has 366 days). The query that Slovenian users of other eDavki converters already have
works as it is. Not read: the Flex Query as CSV (no section codes in the usual setting, so a
section is known only by the shape of its header row) and the **Activity Statement** (a human
report: trade rows without ISIN, subtotal rows, a layout that changes). A file that is not
well-formed XML, or whose root element is not `FlexQueryResponse`, is rejected with one file-level
`invalid_header` error (`invalid_header.flex_query_xml_required`) and a hint to export the Flex Query
as XML. The step-by-step guide for the user is written with the user documentation.

**File shape.** The parser reads the XML once into a tree (no DTD, no external entities; a file
with a `DOCTYPE` is rejected with a file-level `invalid_header` error
(`invalid_header.doctype_not_allowed`) and is never expanded; a UTF-8 byte order mark
at the start is accepted; the file size limit of the other input files applies):

```xml
<FlexQueryResponse queryName="..." type="AF">
  <FlexStatements count="1">
    <FlexStatement accountId="U0000000" fromDate="20240101" toDate="20241231" period="" whenGenerated="20250105;101500">
      <AccountInformation accountId="U0000000" ibEntity="IB-IE"/>
      <Trades> <Trade .../> <Lot .../> </Trades>
      <CorporateActions> <CorporateAction .../> </CorporateActions>
      <CashTransactions> <CashTransaction .../> </CashTransactions>
    </FlexStatement>
  </FlexStatements>
</FlexQueryResponse>
```

There is one `FlexStatement` per account. A section element may be empty or missing; a missing
section is read as empty. Any other section element (transfers, open positions, cash report, NAV,
performance summaries, financial instrument information, accruals, borrow and lending) is not
read, as a whole and without a diagnostic: its rows are never events. Each row element is
checked for the attributes it needs before it is read; a missing one rejects that row with
`invalid_header` (`invalid_header.missing_attribute`, naming the attribute). An attribute that is
there but empty is a missing field instead. A derivative or a currency conversion needs only its
class and its ID, and an ignored cash type needs nothing more. Extra attributes are allowed and
ignored.

| Element | Attributes read | Used for |
| --- | --- | --- |
| `AccountInformation` | `accountId`, `ibEntity` | The Doh-Obr payer of broker interest: `ibEntity` is the `code` of `broker_payers.csv` (`IB-IE`, `IB-CE`, `IB-UK`, `IB-LUX`, `IBLLC-US`). An unknown entity gives empty payer facts and a warning, never a block. |
| `Trade` | `accountId`, `transactionID`, `tradeID`, `levelOfDetail`, `transactionType`, `assetCategory`, `subCategory`, `symbol`, `description`, `isin`, `currency`, `tradeDate`, `dateTime`, `buySell`, `quantity`, `tradePrice`, `proceeds`, `ibCommission`, `ibCommissionCurrency`, `taxes`, `openCloseIndicator`, `notes`, `multiplier` | Buys and sells, fees, asset class, derivatives, cancellations. |
| `Lot` | none | A closed lot: IBKR's own matching of a sale to its buys. Ignored. |
| `CashTransaction` | `accountId`, `transactionID`, `actionID`, `levelOfDetail`, `type`, `assetCategory`, `symbol`, `description`, `isin`, `currency`, `dateTime`, `settleDate`, `reportDate`, `amount`, `issuerCountryCode` | Dividends, withholding tax, payment in lieu, broker interest, bond coupons and accrued interest, fees, deposits. `issuerCountryCode` is read (upper case, only an ISO 3166-1 country) into the payer country of a dividend, a dividend payment row and a bond coupon; it is optional, and empty on rows that are not an income payment. |
| `CorporateAction` | `accountId`, `transactionID`, `actionID`, `type`, `assetCategory`, `symbol`, `description`, `actionDescription`, `isin`, `currency`, `dateTime`, `reportDate`, `quantity`, `proceeds`, `value` | Splits, reverse splits, mergers and the rest, bond maturity. |

Rules (each one is a row of the parser fixtures):

1. **Dates and times.** A date is `yyyyMMdd` (`20240305`); a date and time is `yyyyMMdd;HHmmss`
   (`20240305;154210`). Any other format is `invalid_value` on that attribute. `tradeDate`
   (trades) and the date part of `dateTime` (cash rows, corporate actions) are the tax dates,
   exactly as IBKR gives them; the parser never moves a date. `dateTime` is only the ordering
   timestamp and is in US Eastern time (confirmed: trades on European exchanges show 03:00 to
   11:30). The two dates differ only for a trade between 18:00 and 24:00 Eastern time (US
   after-hours and the IBKR overnight session); IBKR already gives an overnight trade the next
   trading day as `tradeDate`. Whether the Slovenian return uses the exchange's trade date or the
   Slovenian calendar date is unconfirmed (a question for FURS); until it is answered the
   exchange's trade date is used. A time in the hour that does not exist in US Eastern time
   (02:00 to 02:59 on the second Sunday of March) is `invalid_value`: IBKR never writes it, so it
   means a broken file. A time before 2007 is `invalid_value` too: the daylight-saving rule of that
   time is not written, and no value is made up. Trades and corporate actions need the time
   (`yyyyMMdd;HHmmss`); a cash row may have only the date (`yyyyMMdd`), and then it has no
   timestamp. Both forms are read on a cash row, so the first real cash row settles which one IBKR
   writes. The source row of an event is the line of its element's start tag (IBKR writes one row
   element per line), so the user finds it in any text editor.
2. **Signs.** Buy: `quantity` > 0 and `proceeds` < 0. Sell: `quantity` < 0 and `proceeds` > 0.
   `ibCommission` ≤ 0 and `taxes` ≤ 0 (a cost; 0 occurs, a blank one is 0). A sign that disagrees
   with `buySell` rejects the row, as for Trade Republic. A rejected row gets one error, the first
   check that fails, in this order: cancellation (rule 7), a missing attribute, the format of each
   field (an empty value is a missing field, any other is an invalid value), then the signs. A row
   with several faults is therefore reported one at a time.
3. **Numbers.** Plain dot decimals without grouping. Quantities can be fractional
   (`transactionType` `FracShare`); the unit rounding and evidence rules of Trade Republic apply.
   The unit price is `tradePrice` × `multiplier`, rounded once to 8 decimals. Both must be greater
   than zero, and each is checked on its own: a bad `multiplier` is an error on `multiplier`, not
   on `tradePrice`.
4. **Identity.** Broker + `transactionID`, unique among `Trade` elements. A `Lot` reuses the
   `transactionID` of the buy it closes (also in a later year's file), so it is never read as an
   event. Overlapping files give the same IDs, so the merger drops exact duplicates. A row that
   becomes an event and has an empty `transactionID` is rejected as a missing field, because its
   repeat in an overlapping file could not be dropped. A row that is not read (an ignored cash
   type, a cash dividend action, a conversion) needs no ID, and a left-out row (a derivative,
   margin interest) belongs to no form, so it is kept without one; the later warning may then
   count a repeated row twice. `actionID` groups the rows of one corporate action or one dividend
   payment. Several accounts (one `FlexStatement` each) are one taxpayer: one FIFO pool per ISIN,
   nothing is keyed on the account.
5. **Level of detail.** Only `Trade` elements with `levelOfDetail` `EXECUTION` are read. `Lot`
   elements and any other child of `Trades` (orders, wash sales) are ignored with one file-level
   notice with the count (`unsupported_row_type.not_execution`; IBKR's own FIFO and wash-sale
   results are never used). A `levelOfDetail` that is empty is a missing field, not a notice,
   so that a lost trade is never hidden.
6. **Asset class.**

   | `assetCategory` | `subCategory` | Model class | Result |
   | --- | --- | --- | --- |
   | `STK` | `COMMON`, `PREFERRED`, `ADR`, `REIT`, … | Stock | Doh-KDVP |
   | `STK` | `ETF` | Fund | Doh-KDVP |
   | `FUND` | any | Fund | Doh-KDVP |
   | `BOND`, `BILL` | any | Bond | Outside Doh-KDVP, as Trade Republic bonds |
   | `OPT`, `FUT`, `FOP`, `FSFOP`, `FSOPT`, `CFD`, `FXCFD`, `WAR`, `IOPT` | any | Derivative | Left out of every form with one warning (rule 13) |
   | `CRYPTO` | any | Crypto | Left out of Doh-KDVP (the existing crypto rule). IBKR writes it without an ISIN and none is made up, so the row is not read and leaves one notice (`unsupported_row_type.crypto_without_isin`); a crypto row with an ISIN is a trade of class Crypto |
   | `CASH` | any | none | A currency conversion (`EUR.USD`): not read, counted in one file-level notice (`unsupported_row_type.currency_conversion`, `count`), no form |
   | `CMDTY` and unknown | any | Unknown | A trade with an ISIN is kept with the unknown class and the existing classification request; without an ISIN it is a `missing_field` error on `isin` |

   A notice is an `info` diagnostic that names no form. The notices of this contract (the ignored
   rows of rule 5, the cash totals of rule 9, the conversion and the crypto row) block nothing.
   The `subCategory` of an ETF is on trade rows only, so only a trade names the class of an
   instrument: a corporate action row gives no class evidence and never changes it.
   In the merger, an action row that names no class (no evidence, and an unknown class in its own
   file) does not vote on the class of the instrument, so a split row in one file never
   conflicts with the trades of another.

7. **Cancellations and corrections.** A row with `buySell` `BUY (Ca.)` or `SELL (Ca.)`,
   `transactionType` `TradeCancel` or `TradeCorrect`, or the code `Ca` in `notes` is rejected with
   an error on that row (`unsupported_row_type.cancelled_trade`, naming the attribute that shows
   it). It is never netted silently. Other `notes` codes (for example `IA`, `P`,
   `ST`, several joined by `;`) are normal and ignored.
8. **Fees.** `ibCommission` is booked in `ibCommissionCurrency`, which can differ from `currency`:
   the fee currency is `ibCommissionCurrency`, never the trade currency or the amount's currency.
   `taxes` (stamp duty, financial transaction tax) is a second fee in `currency`. Fees stay
   informational. `fxRateToBase` is not read: the official ECB rate is used, as for Trade
   Republic.
9. **Cash transactions** (`type`):

   | `type` | Maps to |
   | --- | --- |
   | `Dividends` | A dividend row, gross = `amount`. |
   | `Withholding Tax` | A tax row of the payment with the same `isin` and `actionID`. A positive row (refund) lowers the tax. |
   | `Payment In Lieu Of Dividends` | A dividend row like `Dividends` (Doh-Div, payer by ISIN), with a warning "substitute payment, check". Unconfirmed, a question for FURS. |
   | `Broker Interest Received` | Interest, `CashHolder::Broker` (Doh-Obr), payer = the `ibEntity` of the account. |
   | `Broker Interest Paid` | Margin interest: left out with one warning (rule 13). |
   | `Bond Interest Received` | A bond coupon (Doh-Obr, payer = issuer by ISIN). |
   | `Bond Interest Paid` | Accrued interest paid at a purchase: the existing accrued-interest warning. |
   | `Other Fees`, `Commission Adjustments`, `Advisor Fees` | Ignored, fee summary only. |
   | `Deposits/Withdrawals` | An ignored cash movement. |
   | any other `type` | An `unknown_row_type` error on the row, with no form named (it may belong to any form). |

   A dividend, a tax and a payment in lieu need `isin` and `actionID` (an empty one is a missing
   field, because the rows of a payment are added up by them) and `reportDate`; a coupon needs
   `isin`. A coupon cannot be negative. Interest on cash is read with its sign (debit interest is
   negative), as for Trade Republic, and has no instrument. A tax row on interest has no ISIN and
   cannot be added to a payment, so it is reported as a missing field until a real file shows how
   IBKR books it. The tax may be on a dividend or on interest, so the error names the tax row
   (`row_kind.income_tax`) and blocks Doh-Div and the interest form of the broker, not capital
   gains. The name of an instrument is not on a cash row (`description` is the text of the
   payment), so a payment row has none.

   **Level of detail.** Only a `CashTransaction` with `levelOfDetail` `DETAIL` (one booking) is
   read, and the attribute is needed (a missing one is `invalid_header`, an empty one a missing
   field). Any other value (a total row, which would count a booking twice, and may have no ID or
   ISIN) is not read and is counted, with any other child of the `CashTransactions` or
   `CorporateActions` section, in one file-level notice (`unsupported_row_type.not_detail`). Which
   values IBKR writes is confirmed on the first real file with cash rows. A cash row whose `type`
   is missing is named a cash row in its error.

10. **Corporate actions** (`type` code):

    | Code | Result |
    | --- | --- |
    | `FS` forward split, `RS` reverse split, same ISIN before and after | `UnresolvedSplit`: the ratio is asked, as for Trade Republic. The text in `actionDescription` (for example "SPLIT 4 FOR 1") is evidence only, never a verified ratio. |
    | `FS` or `RS` where the ISIN changes, `TC` merger, `IC` issue change | `unsupported_corporate_action` until mergers are processed. |
    | `SO` spin-off, `SD` stock dividend, `RI`, `DI`, `ED`, `SR` rights, `TO`, `TI` tender, `DW` delisted worthless, `CH`, `HD`, `HI` choice dividend, `BC`, `CI`, any other | `unsupported_corporate_action`. |
    | `BM` bond maturity, `TM` T-bill maturity | `BondRedemptionEvent`, information only. |
    | `CD` cash dividend, `CP` coupon | Ignored here; the cash row is the payment. |

    A split whose ISIN changes is found by its `actionID`: rows of one split action with two ISINs
    in one statement. A unit or `quantity` that is not a number rejects the row. `BM` and `TM`
    count as a maturity only on a bond or a bill; the same code on another class is
    unsupported. A positive `quantity` on a maturity is a cancelled maturity. Every other action
    keeps its quantity as evidence and is stored with the merger type, which the corporate-action
    stage blocks.
11. **Transfers.** The `Transfers` section is not read. One FIFO pool per ISIN across brokers
    makes a position transfer a no-op, and units that came in without their buys in the files are
    caught where they matter: a later sale gives `incomplete_history`. A warning per transfer would
    only repeat that.
12. **Facts the export does not give** stay shared requests, never an IBKR guess downstream: the
    legal class beyond `assetCategory` (a non-UCITS fund, an ETC: the existing classification
    request), the dividend payer's address (reference files by ISIN, else empty facts and a
    warning), the bond type question and the split ratio.
13. **Margin and leverage are not supported, for any broker.** Margin borrowing, CFDs, options,
    futures, leveraged and knock-out products (*finančni vzvodi*, *izvedeni finančni
    instrumenti*) are filed by the user in Doh-IFI or not at all. When such rows occur the user gets
    **one warning per broker and run** that the files hold one or more margin or leverage rows that
    are not supported (with the number of rows), and that is all: the rows are left out of every
    form, never block a form and are never asked about. A derivative often has no ISIN; none is
    made up. The IBKR rows are the derivative asset classes of rule 6 and
    `Broker Interest Paid` (rule 9).

**Normalization to the common model.** After the parser an IBKR event looks exactly like a Trade
Republic event of the same kind: the merger, FIFO and the forms never see an IBKR format, time
zone, sign or attribute name.

- **Tax date:** a calendar date from `tradeDate` or the date part of `dateTime`, as given (rule 1).
- **Timestamps:** `mSourceTimestamp` is UTC in milliseconds, as for Trade Republic. The parser
  converts IBKR's US Eastern `dateTime` with the US daylight-saving rule in force since 2007
  (second Sunday of March to first Sunday of November, at 02:00 local time; EST is UTC−5, EDT is
  UTC−4), written in the parser, with no time-zone database at run time, so the static and the
  Windows builds behave the same. A time in the repeated hour of November is read as EDT. A
  converted UTC date that differs from the tax date is normal for IBKR (no `inconsistent_value`
  warning as for Trade Republic), because the tax date is the exchange's date.
- **Signs and amounts:** units, prices, amounts, fees and tax are stored positive, and the side or
  the kind carries the direction. IBKR's negative `quantity`, `proceeds`, `ibCommission` and
  withholding amounts are turned around in the parser.
- **One event per cash row, netted after the merger.** The rows of one payment can be in two files
  (a dividend in one year's file, its reversal and correction in the next year's), so the parser
  does not net them. It makes one income event per cash row (a dividend, a tax or a reversal row)
  with its own identity and a broker-neutral payment group ID from `actionID`. After the merger has
  removed duplicate rows, one broker-neutral step adds up the rows of each payment (same broker,
  payment group ID and ISIN) into one dividend (gross, tax, pay date), dated on the pay date in the
  pay date's year. A net of zero or less gives no dividend and a warning. A reversal without its
  dividend in the files gets that one warning instead (the net-of-zero warning is for a payment
  that has a dividend row). Rows that cannot be added up (two currencies, two pay dates, a refund
  above the tax) give one error and no dividend. The step is `netIncomePayments`
  (`processors/income_payment_netting.hpp`); the merger calls it after the duplicates are gone and
  before the presentation lists are built, and the netted dividend adds no name of its own when its
  rows name no instrument. A correction booked (`reportDate`) in a later year than its
  pay date gets a warning that the return of that year may need a correction. A broker without
  such rows (Trade Republic) gives one row per payment, which the step passes through unchanged.
- **Currencies:** IBKR codes map to the shared currency type; an unknown code rejects the row.
  The fee currency is `ibCommissionCurrency` (rule 8).
- **Identity:** broker + `transactionID` for every event; a netted dividend keeps every source row
  in its provenance.

Diagnostics of the parser (`diagnostics.md`, "Interactive Brokers parser diagnostics"): a file that
is not a Flex Query XML or has a `DOCTYPE`; a missing attribute; a cancelled or corrected trade
row; the ignored closed lots and other trade rows, the currency conversion and the crypto row
without an ISIN (notices). The steps after the parser give: the derivative and margin rows left
out (one warning per broker and run); a payment in lieu (warning); a dividend reversal without its
dividend and a correction in a later year (warnings); an unknown `ibEntity` (warning).

The parser stores what the later steps need in neutral lists of the statement, which TR never
fills: an income payment row per cash row of a payment (kind, positive amount, pay date, booked
date, payment group ID, substitute flag), a left-out row per derivative or margin row, a second
fee per trade (`taxes`) and the payer code (`ibEntity`) of interest on cash. The merger removes
duplicate payment rows like duplicate events (broker and transaction ID, all facts equal, otherwise
`conflicting_duplicate`) and nets them; it removes duplicate left-out rows by broker and
transaction ID without comparing facts (a row without an ID is never merged) and keeps them in
the merged statement (`MergedStatement::mLeftOutRows`), where the processing stage counts them for
the selected year. The second fee is one more amount in the fee total of the trade's currency, and
the payer code picks the broker payer row of the Doh-Obr row, so two accounts of one taxpayer can
have two payers. Trade Republic leaves out a trading row of class `DERIVATIVE` the same way.

`tests/test_data/ibkr/` holds the synthetic fixtures (made-up accounts and ISINs with valid check
digits, no real value). `contract_example.xml` has one row for each kind above; the other files
each cover one part (supported rows, ignored rows, rejected rows, corrections across two years,
time zone edges, quantity digits, a conflicting duplicate, an unknown `ibEntity`, file-level
errors, and the same trades as a Trade Republic CSV).
`tests/test_data/ibkr/README.md` has the expected result of every row; the parser tests take their
literals from it.

### Stable input sequence

The stable input sequence is scoped to the complete input request. `mSourceIndex` is the zero-based
position of the source file in the request, and `mEventIndex` is the zero-based source-order
position assigned by that source's parser. The current CSV parsers use the data-row position as the
event index. Parser completion order is never used, so files can be parsed concurrently without
changing event order.

The pair must uniquely identify each imported event in a request. It is the final ordering key and
does not represent broker time. Application JSON may expose only the derived source ID, never the
raw source index.

### Equality and duplicate identity

`EventMetadata` equality compares every metadata field, including the complete source collection.
This exact value equality is separate from duplicate detection. Source references are presented in
stable input-sequence, filename and row order; the earliest source is the event's primary ordering
source.

A transaction identity consists of the broker and transaction ID. The same ID from different
brokers therefore identifies different transactions. Matching identities mark duplicate
candidates even when their filename, row, or stable sequence differs. A candidate is an exact
duplicate only when its event kind, tax date, timestamp, instrument, and event-specific values also
match; otherwise it is a conflict. Events without transaction IDs are not automatically
deduplicated.

Trade and corporate-action quantities retain source-linked `UnitSourceEvidence`: original decimal
text, discarded fractional digits, exact canonical source value and rounding direction. Evidence
from every exact duplicate is kept in stable source order. Canonical source quantities compare
numerically, so extra trailing zeros do not create a conflict. Different exact source quantities
that round to the same stored units do create a conflict. An input without evidence can still
match stored facts, but supplies no rounding proof; known evidence across the complete identity
group must agree even when its first event has no evidence.

### Deterministic ordering

Events are normally ordered by tax date, effective timestamp presence, effective timestamp value,
and stable input sequence, in that order. The effective timestamp uses `mOrderingTimestamp` when
present, otherwise `mSourceTimestamp`. A default ordering time is not evidence of execution time.
On the same tax date, timestamped events precede events without timestamps. Broker, filename, row,
and transaction ID are not ordering fallbacks. The placement of
untimestamped events is a deterministic policy and does not claim that they occurred after every
timestamped event.

Tax processing has one event-kind priority: a supported split or reverse split is applied before
every purchase or sale for the same ISIN on its effective tax date. Trades on that date use the
adjusted position. The effective date is the one the user confirmed with the ratio; the action's
row keeps its tax date in the ledger, and the FIFO matcher applies the action by the confirmed
date instead of at the row. Several actions for the same ISIN and date use reliable source
timestamps. If their order cannot be established, stable input sequence keeps the diagnostics
deterministic but must not be used to guess the result. The processor reports the ambiguity and
leaves that ISIN unprocessed.

## Ownership of pipeline data

Each pipeline stage either passes data on or computes new data, and its signature shows which:

- A stage that passes the same data on takes it by `&&` or by value and moves it. Nothing is
  copied on the way.
- A stage that computes new data from inputs that are still needed later reads them by `const&`
  and returns a new object.

| Stage | Takes | Why |
| --- | --- | --- |
| Parser | returns `ParseResult` by value | the result is new data |
| `StatementMerger::merge` | `std::vector<StatementMergeInput>&&` | events move into the merged statement |
| `TaxProcessor::prepareReportingYear` | `TaxProcessingRequest` by value | moves the merged statement into the result; the ledger holds only references into it |
| `FifoMatcher::match` | `const&` statement and ledger | computes matches; both are still needed |
| `CapitalGainsReportBuilder::build` | `const&` statement, ledger and FIFO result | computes report rows; all are still needed |
| `KdvpXmlWriter::write` | `const&` metadata and report | computes the XML text; the report is still needed |
| `writeReportXml` | `ApplicationResultInput&`, `const&` metadata and models | calls each writer under its own guard and stores the XML and the writer's errors in the input |
| `ApplicationResultBuilder::build` | `ApplicationResultInput&&` | moves each report's XML into the result; the diagnostics become new application diagnostics |

A copy is made only where a caller still needs the data afterwards, and it is written out at the
call site.

## Statement-merger contract

`StatementMerger` is a transport-independent operation over parsed domain results. Each
`StatementMergeInput` carries the source file's request index separately from the collection's
arrival order. Request indices must be unique and must agree with the stable source indices on the
input's events. The merger may therefore receive parse results in task-completion order while still
producing the same result. A repeated or inconsistent source index is an input error and must be
reported; it is never repaired from vector position, filename or broker.

The merger takes the inputs by move (`std::vector<StatementMergeInput>&&`). Their events and parse
diagnostics are moved into the result, so each event exists once. A caller that still needs a
`ParseResult` after the merge, such as a debug tool that prints the parser diagnostics, passes an
explicit copy.

The operation is single-threaded. Its result owns one `BrokerStatement` presentation structure and
a separate chronological sequence of `StatementEventReference` values into that structure. Event
payloads and provenance are stored only in the presentation structure. A reference contains the
event kind, event index and an instrument index when that collection is instrument-based. Benefit
and private-market references have no instrument index.

Presentation order is independent of economic processing order:

1. collections appear as trades, dividends, interest, benefits, private-market events and bond
   maturities;
2. instrument collections use their broker-neutral identity, with deterministic identity and name
   ordering; and
3. events within an instrument use the normal event chronology.

Trade and dividend instruments use ISIN as their identity. Interest instruments use interest type
plus ISIN when one exists; without an ISIN, interest type plus payer or instrument name is the
available broker-neutral identity. Benefits and private-market records remain standalone events.
A Trade Republic benefit (`BENEFITS_SAVEBACK`, `STOCKPERK`, `BONUS`, `REFERRAL`) is information
only: it has no units, lot, classification request or form, and it never changes a purchase value.
Names never replace an ISIN. A valid trade, action, dividend or bond coupon with a known ISIN and
blank display name is retained with a `MissingField` warning on `name`. Presentation uses the
earliest non-empty supplied name for the same ISIN under `StableSourceOrder`. If every supplied name
is blank.
identity, a warning retains every name variant and source, including original blanks.

Trades and actions retain source-linked `InstrumentSourceEvidence` with the original name, class
label and mapped class on each event. Dividends and bond coupons retain the same evidence for name
completion; broker cash interest keeps its established broker identity. Instrument summaries never
replace these original facts. Older in-memory inputs without evidence use their instrument fields;
missing evidence is not proof of an original label.

Valid trade/action facts are retained when `asset_class` is blank or unrecognized. Blank classes
produce a `MissingField` error; unrecognized labels produce `UnsupportedAssetClass`, both on
`asset_class`, with messages saying the event was retained. Required identity, date/datetime,
quantity and monetary validation still rejects unusable rows before classification evidence is
created. Recognized Crypto and PrivateFund categories retain their mapped class and existing
unsupported-treatment warnings.

A trade/action class disagreement retains every event and source and leaves the instrument class
`Unknown`. Within one CSV the parser reports `InconsistentValue`; merging reports
`InstrumentAssetClassConflict` from surviving event evidence in every file layout. Parser diagnostics
may therefore differ with file layout, but retained facts, unresolved classification and error state
do not. A later Stock row cannot clear an earlier Stock/Fund conflict. Missing/unrecognized evidence
also stays unresolved alongside a known class. Distinct unrecognized labels remain separate
`Unknown` variants, with `mUnrecognizedLabel` in merger diagnostics; original recognized labels
remain in event evidence. Sources and presentation follow the normal ordering rules.

Before presentation grouping, the merger classifies every valid event with a non-empty transaction
ID by `(broker, transaction ID)`. Events in one identity group are exact duplicates only when all
of these values match:

- event kind, tax date, optional source timestamp and optional ordering timestamp;
- broker-neutral instrument identity and tax-relevant classification; and
- every event-specific monetary, unit, ratio, currency, fee, tax and descriptive value.

Trade/action duplicate comparison uses each event's original mapped class, never its instrument's
summary. When both inputs retain unrecognized labels, different labels conflict even though both
map to `Unknown`. Exact duplicates combine all original evidence and sources. Accumulated evidence
must agree across the whole group, including when its first candidate has no source evidence.
Duplicate classification happens before instrument aggregation; rejected same-ID groups cannot
contaminate the class of unrelated surviving events.

For an ISIN-identified instrument, a display-name difference does not make the events conflict;
the normal instrument-name warning retains each name and its sources. An exact duplicate produces
one event under its earliest `StableSourceOrder` source and combines every contributing source
reference. When two or more of the merged copies come from the same file (the same request
position, not the same filename), the merger also adds one `DuplicateTransactionInFile` warning
per such file. Copies are counted per event, not per source reference, so one event that already
carries several rows of its file is not a repeat, and copies with the same rows count as one copy.
The warning lists the source references of that file's copies, so the user sees every repeated
row, and it carries no raw rows or values. Copies from different files add no warning, because
loading the same export twice must count once. If any candidate in the identity group differs, no
candidate enters the merged ledger and one `ConflictingDuplicate` error retains all sources and
involved event kinds; the group then gets no `DuplicateTransactionInFile` warning. Missing or empty
transaction IDs never trigger this classification. Different transaction IDs do not establish
economic equivalence, including for corporate actions.

After transaction classification, instrument collections are ordered by their documented identity,
and events inside each concrete collection use `ChronologicalEventOrder`.

The chronological reference sequence crosses every presentation collection and uses tax date,
effective timestamp presence/value and stable input sequence. It does not process one broker or
one presentation collection to completion first. The merger does not apply the processing-only
same-day corporate-action priority, calculate FIFO, or resolve economic corporate-action identity.
Collection aggregation and chronological-reference construction are separate implementation
phases. The merger populates one reference per retained event after presentation ordering is final.
Consumers use this sequence, not collection traversal, as the chronological ledger. The sequence
retains all supplied years; reporting-year preparation belongs to the later processing stage.

Parser and merger diagnostics share the result without changing parser diagnostics. A
`SourcedParseDiagnostic` contains the original `ParseDiagnostic` plus its broker and source request
index. Merger diagnostics have their own typed codes and retain every relevant source reference.
Parsing diagnostics precede merging diagnostics. Parser diagnostics use file request order and
preserve each parser's diagnostic creation order. Merger diagnostics use earliest source request
index, earliest source event index, tax date, ISIN, severity (errors first), diagnostic code enum
order, then deterministic creation order; missing source, event, date and ISIN values sort last.
Messages are never sorting keys. Report scopes and final application diagnostic IDs are added
later: after all pipeline stages are combined, the full report-aware ordering from
[`diagnostics.md`](diagnostics.md) applies.

Input outcomes are defined as follows:

- no inputs produce empty presentation and chronological collections with no diagnostics;
- a row-level parse error preserves the parser's valid events and diagnostic;
- a file-level parse failure contributes its diagnostics and no events, while other inputs remain
  available;
- when every file fails, merged domain data is empty and every parser error remains visible; and
- inputs from different known brokers are accepted and combined through the same broker-neutral
  result.

The merger result has no report status. The later application service derives report and overall
statuses after applying each diagnostic's scope. Empty merged data is therefore not itself an error,
and parser errors cannot disappear merely because another input supplied valid data.

Broker values whose meaning is not verified remain source data, not calculated tax inputs. In
particular, a Trade Republic split row's decimal `shares` value does not establish the split ratio.
The application result asks the frontend for the action's new-shares-to-old-shares ratio. Only a
validated user confirmation turns that action into a processable split or reverse split.

The current `CorporateAction` model uses `UnresolvedSplit` with an empty `mRatio` for
unverified split evidence. In that state, `mUnitsDelta` stores the signed quantity rounded to
`Units` precision; it is not a verified change in inventory. `mUnitEvidence` retains the original
signed decimal, canonical exact value, discarded digits and source reference. Neither a positive
nor a negative quantity establishes economic direction. Explicitly resolved evidence uses `Split`
with a known `2 / 1` ratio or `ReverseSplit` with a known `1 / 10` ratio, for example. Direction
comes from the verified new-shares-to-old-shares ratio, never from the source quantity's sign.
`mRatio` is an exact `SplitRatio` (two whole numbers, new and old shares), never a ratio rounded
to the `CorpRatio` scale, so a broker that supplies a verified ratio can keep it exact.

Current consumers preserve this distinction:

- the Trade Republic parser creates unresolved actions and preserves signed source evidence;
- the statement merger compares type, quantity, exact source value and optional ratio, retaining
  each source through deduplication without resolving an action;
- the parser dump labels an unresolved value as `source_quantity` and the ratio as unresolved;
- the exact-arithmetic dump shows labeled hypothetical previews or an explicit caller-supplied
  preview ratio, without establishing the real economics from the CSV quantity.

Ratio confirmation belongs to the corporate-action stage ("Corporate-action contract" below);
the remaining identity rules and inventory application belong to the corporate-action processor
described in `fifo.md`. A resolved input supplied to a merger test represents explicit synthetic
evidence; it does not demonstrate broker verification or resolution.

After transaction deduplication, economic corporate-action identity is resolved separately across
all sources. One confirmed action changes all open lots of its ISIN across brokers exactly once.
Grouping by ISIN and calendar year is used only for
`corporate_action_sources_mismatch_possible`: always warn when action records for that ISIN/year
come from multiple source files or brokers, and show every date, type, ratio and source. The year
is never an action identity or a reason to combine two distinct actions. Ambiguous grouping returns
`corporate_action_identity_required`; a ratio prompt covers the combined action's complete source
set. The detailed identity, ordering and conflict rules are in `fifo.md`.

The processing request also retains a separate analysis view for 30-day loss eligibility, including
required previous-December and next-January events. It does not mutate the selected-year closing
FIFO position. Partially eligible losses become separate report rows, without duplicating their
inventory consumption. The user's answer to the loss question is an explicit input of the
decision session, as defined in `tax_rules.md` and `diagnostics.md`.

### Reporting-year preparation contract

`TaxProcessor::prepareReportingYear` accepts a `TaxProcessingRequest` containing the optional
selected-year integer and a merger-produced `StatementMergeResult`. The returned
`TaxProcessingResult` owns that complete merged result once, including all upstream diagnostics,
source facts and years. Its candidate views contain `StatementEventReference` indices into the
owned presentation, so copying or moving the result does not invalidate them. Keep that presentation
unchanged while using the indices.

A missing year or a value outside 2000..9999 returns no `mLedger` and one `InvalidReportingYear`
processing diagnostic at `HistoricalValidation`, on `reporting_year`, affecting all four reports.
It invents no source, date or instrument. Upstream data and diagnostics remain available.
A valid year returns a `ReportingYearLedger`, even when all its views are empty:

- `mCapitalEvents` retains trades and corporate-action evidence through inclusive 31 December.
  Its first `mHistoricalCapitalEventCount` entries precede 1 January and establish opening inventory.
- `mSelectedYearEvents` retains all six event kinds within inclusive 1 January–31 December for
  later classification and report preparation. These are candidates, not final report rows.
- `mLossAnalysisEvents` retains trades and corporate-action evidence between 1 January minus
  30 days and 31 December plus 30 days, inclusive. For 2024 these bounds are 2 December 2023
  and 30 January 2025. All supplied ISINs remain candidates; equivalent-instrument declarations
  and the exact windows of individual losses belong to later loss processing.
- `mHasSelectedYearSales` records whether an in-year source trade is a sale. A false value supports
  the later capital `no_data` explanation only after classification and blocking errors are resolved.

Every view preserves merger chronology and uses only `mTaxDate` for membership. Earlier history
outside the analysis window remains owned and available as its opening context. Following-year
analysis events never enter the selected-year ledger or output candidates. Unresolved classifications
and action evidence are preserved; preparation does not infer support, apply actions, reorder same-day
splits, consume inventory, calculate loss eligibility or establish statement coverage. Those operations
belong to later processors. Boundary preparation success does not assign final report statuses,
clear upstream errors or mean that XML was generated. Full application status/API integration remains
later work. Parser diagnostics JSON is not changed by it.

Report exclusions preserve source facts and replacement allocations needed by retained losses.
Unresolved dependencies on excluded data require a loss decision for the dependent sale; they
cannot be cleared by removing the source instrument's report rows. Relevant following-January
actions use the same identity, ratio and multiple-source warning rules in the analysis view.

Exact FIFO lot basis retains the complete product of the acquisition-date converted
eight-decimal `UnitPrice` and imported Units. Proportional allocations remain exact rational
values until their specified output boundary. Controlled quantity adjustments retain their
original source values, apply through a run-local overlay and rebuild affected history: the
pool stage repeats the FIFO run, the action questions and the quantity stage until nothing new is
confirmed or corrected, and gives the later stages the corrected statement. The
backend tracks the cumulative absolute EUR impact of inventory reconciliation per ISIN/run; details are in `calculations.md`
and `fifo.md`. This additional processor state does not change parser diagnostics.

The arithmetic layer returns typed numeric failures without report assumptions. The intermediate
`ProcessingDiagnostic` carries the stage, affected reports and safe source/instrument context.
The numeric adapter supplies caller-scoped arithmetic errors; reporting-year preparation reuses
the same envelope for year validation. `ApplicationResultBuilder` combines
the parser, merger and processing diagnostics into the application result: report scope, blocking
flag, deterministic order and IDs, the status of each form and the overall status, with version 2
JSON. `DecisionSession` holds the answers and recalculates the run, as defined in
`diagnostics.md`, "Structured user-decision contract". The 30-day loss rule is the first stage
that asks a question ("Loss eligibility contract"); the exclusion choices remain later integration.

Foreign-currency processing follows [`calculations.md`](calculations.md). The application result
contains one structured request for every missing official `(currency, tax date)` rate. A
user-entered official rate is shared across brokers and instruments only for that exact pair. A
broker rate is an event-specific fallback, requires an explicit frontend warning, and remains
marked as non-official in the result.

The exchange-rate request is not available yet: a missing rate is a blocking error today.

The application result also contains the selected-year informational fee summary and benefit
summary. The fee summary groups preserved fees by broker and original currency. It provides a
broker-rate EUR equivalent only when every required fee rate is available from that broker. A
fee never creates an official-rate request. Missing fee rates leave the affected EUR totals
unavailable and do not block FURS XML output.

Final models and result statuses are split by form: capital gains (Doh-KDVP), dividends (Doh-Div),
bank-deposit interest (Doh-DHO), and other interest (Doh-Obr). Broker cash interest and ordinary bond
coupons remain separate subtypes inside the common other-interest report. A broker parser supplies
facts such as product category and payer identity; the tax processor applies date-specific rules.
Unknown classifications and missing income fields produce structured requests. Explicit
classification exclusions remain visible in `excludedItems`, separate from each report's failed ISINs.

A Doh-Obr error safely isolated to an ISIN or to one broker's cash interest produces
`other_interest_failure_resolution_required`. The user excludes all listed failed ISINs' Doh-Obr
income, and the cash interest of every listed broker (an error of one cash-interest payment names
its broker, as it has no ISIN), or fails that form; `FailureResolutionStage` serves all three
forms with the form and the request type as parameters. Each report owns its exclusion list and failure choice; capital and
other-interest choices never apply to each other. Preserve original evidence and unaffected income.
Doh-Div has the same choice (`dividends_failure_resolution_required`): the dividend stage makes
one missing-rate error for each payment, with its ISIN, so one ISIN's failure never fails the
whole form; after the exclusion the caller removes the rows of the listed ISINs
(`excludeDividends`).
Non-isolatable errors and final XML failures retain their normal scope, as defined in `diagnostics.md`.

A standalone reference catalogue (`reference_data.md`) supplies Slovenian-company and imported
payer metadata with source and revision provenance, as suggestions for the user to confirm. It
can prefill payer and treaty facts; a dividend payer with the residence country `SI` makes that
dividend not reported (a Slovenian bond's coupon is reported), and payment evidence determines the
actual foreign tax.
Treaty exemptions require a separate applicable legal basis. The income result retains treatment
decisions. A payer that is not found is reported with the payer facts left empty and a warning;
a payer with several rows is a skippable question where the user picks one row (see "Income
report model contract"); the user completes missing facts in eDavki. Changing a referenced
catalogue revision invalidates affected decisions. The files are built into the program and read with the shared CSV
reader into a `ReferenceCatalogue` (`include/taxbroker/reference_catalogue.hpp`). A lookup by exact
ISIN returns `Found`, `Conflict` or `NotFound`; a broken file stops the load with the
file and logical row.

The deposit allowance considers all imported qualifying bank interest. A TR total at or below
the threshold asks one yes/no question about other banks. Slovenian retail government bonds
are outside the current TR/IBKR import scope; their special treatment is not part of this workflow.

The complete application diagnostic model, revision-bound decisions, deterministic ordering,
privacy boundary, and four independent form statuses are defined in [`diagnostics.md`](diagnostics.md).
Application JSON uses version 2 for this expanded result. Parser diagnostics JSON is a developer
format until the beta (see "JSON contract" below).

### FIFO matching contract

`FifoMatcher::match` takes the merged statement, the `ReportingYearLedger` made from it and the
corporate actions whose ratio and effective date the user confirmed (`ConfirmedCorporateAction`:
the action rows, the reduced ratio and the date). It reads `mCapitalEvents` in order and changes
nothing in the statement. The result has one
`FifoIsinResult` for each trade instrument, in the presentation order (by ISIN), and the numeric
diagnostics of the run. Each entry holds:

- `mMatches`: the internal FIFO matches of the sales dated in the selected year, in ledger order.
  A match links one sale to the units it takes from one purchase, with the exact purchase basis,
  the exact sale value (sale `UnitPrice` times matched `Units`) and the signed gain (sale value
  minus basis), all in ticks of `10^-16` EUR and never rounded. A negative gain is a loss-making
  portion. A sale that uses two purchases has two matches. They are not Doh-KDVP filing rows.
- `mEarlierMatches`: the matches of the sales before the selected year, in ledger order. Only
  the 30-day loss rule reads them: an earlier loss takes its replacement units first.
- `mOpenLots`: the lots still open in FIFO order at 31 December, each with its purchase event,
  date, remaining units and exact remaining basis.
- `mAppliedActions`: the confirmed actions the pool applied, with their effective dates.
- `mStatus`, `mStoppedAt` and `mAvailableUnits`: why and where the pool stopped, if it did.
  A pool stopped at a corporate action keeps the lots at the start of the action's day.
- `mShortSale`: for a pool stopped at a corporate action because of a sale before it that needs
  more units than were held (see `CorporateAction` below); the lots are then those just before
  that sale.

A confirmed action is applied at the start of its effective date, before the trades of that day,
not at its row: the units of every open lot of the ISIN change by the exact ratio, the total is
rounded once to eight decimals and spread by the largest remainder ([`fifo.md`](fifo.md), "Exact
lot adjustment"), and each lot keeps its purchase date, FIFO place and exact basis. Confirmed
actions after the last trade still change the lots at 31 December. A rounding gives a
`corporate_action_quantity_rounded` warning; the rounding of all actions of one ISIN must stay
below EUR 0.01 (`corporate_action_unrepresentable`, key `.rounding_budget`), and a lot that would
become 0 units or an overflow gives `corporate_action_unrepresentable`. Either error stops the pool
at the action with its lots unchanged.

An earlier-year sale uses up lots; its matches go to `mEarlierMatches`, never to `mMatches`. One
pool exists for each ISIN, across files, brokers and accounts; broker, file and row never change
the matching. `isPastHoldingPeriod(match)` tells whether a match uses units past the holding period
([`tax_rules.md`](tax_rules.md), "Long-held investments"); the loss rule and the report builder
share it.

A pool stops at its first event that it cannot apply and ignores its later events. Other ISINs
continue. The failing sale is never applied partly: the lots stay as they were just before it,
and the matches made before the stop stay in the result as evidence.

| Status | Stopped at |
| --- | --- |
| `Matched` | Nothing; every trade up to 31 December was applied. |
| `CorporateAction` | The first corporate action without a confirmed ratio, before the trades of its day: a trade of the ISIN on that date is not applied, even when it comes first in the ledger, and `mStoppedAt` is the action. The confirmed actions before it are applied. Also a sale with too few units (but some open) whose ISIN has a sale in the selected year, when that action is a split row at most `kBrokerDateMarginDays` (10) days after the sale: the broker may have booked the split late, so the pool stops at the action with `mShortSale` set and no `incomplete_history` error. |
| `NonEurTrade` | The first trade whose currency is not EUR, also before the selected year. |
| `InsufficientUnits` | A sale that needs more units than the earlier purchases left open. The result has one `incomplete_history` error. |
| `NumericFailure` | An invalid trade value, an overflow, or a confirmed action that cannot be applied (`mStoppedAt` is its first row). The result has a diagnostic. |

The matcher reports two kinds of diagnostics, both for the capital report only:

- numeric failures (`unrepresentable_value` and `arithmetic_overflow`, stage `FifoTax`); and
- one `incomplete_history` error for an `InsufficientUnits` pool (stage `HistoricalValidation`).
  It names the investment and carries the failing sale's tax date and sources (file, row and
  transaction ID). It has no message parameters, so no unit count or amount is in the message.
  Only the first failing sale of an ISIN is reported, because the pool stops there;
  `mAvailableUnits` keeps the open units. The error blocks the capital report until the user
  chooses to exclude the ISIN or to fail the report (`FailureResolutionStage`).

The ratio questions and the other corporate-action errors come from the corporate-action stage
("Corporate-action contract"), the missing-rate errors of foreign trades from the capital report
gates (see "CLI report command (walking skeleton)"). The trade prices are converted before FIFO.
The 30-day loss rule follows the corporate-action stage ("Loss eligibility contract"). The
Doh-KDVP filing rows are built from the matches as described in "Doh-KDVP report model contract".

### Corporate-action contract

`CorporateActionResolver::resolve` (`src/processors/corporate_actions.hpp`) takes the merged
statement, the reporting-year ledger, the FIFO result of the same run, the confirmed actions that
FIFO applied and the session's `RunDecisions`, and changes nothing in its inputs.
`matchCapitalPools` runs the two in turns: match, resolve, and when the resolver found a new
answer, match again with that action confirmed. So a confirmed action is applied and the next
action of the ISIN is asked in the same calculation, with the position after the earlier one.
The CLI, the debug tools and the tests call `matchCapitalPools`. It asks for the ratio of a
corporate action without a verified ratio, as [`fifo.md`](fifo.md), "Finding the split ratio",
describes it. It never depends on the broker, except to tell whether two rows of one day come
from one broker.

- **Which ISINs.** Every ISIN with a sale in the selected year and a corporate action up to 31
  December (or a split row after it that a December loss window needs, see "Rows after 31
  December" below), except a bond, derivative, crypto or private fund and an instrument that is left
  out of capital gains (`ReportingYearLedger::mLeftOutIsins`, set by the classification stage):
  its sales are blocked by a class error or it is not reported, so its actions are not listed, not
  asked and give no error of their own.
  Its actions are listed, grouped by date, as far as the pool
  reached: the applied ones, the one the pool stopped at, and the later ones. A pool stopped for
  another reason before an action (missing history, a foreign trade without a rate, an action
  that could not be applied) lists no further action; its own error blocks it. A later action
  is never asked before the earlier one is applied, because the position before it depends on that.
- **Result.** `CorporateActionResult` has one `ResolvedCorporateAction` per date (by ISIN, then by
  date): its events, effective date, state and, for `RatioConfirmed`, the reduced ratio and the
  effective date (`mConfirmedDate`) that the user supplied (never the broker); `mEffectiveDate`
  stays the broker's row date. States: `Applied`, `RatioConfirmed` (only between two rounds of
  `matchCapitalPools`), `WaitingForRatio`, `WaitingForEarlierAction`, `Cancelled`,
  `SameDayAmbiguous`, `DifferentDatesAmbiguous`, `Unsupported`, `NoPosition` and
  `NumericFailure`.
- **Applied action.** `Applied` with the confirmed ratio and date, and one
  `corporate_action_applied` warning (key `.user_ratio`, parameters `newShares`, `oldShares` and
  `effectiveDate`) that says the user supplied the ratio.
- **First action of the pool.** A merger row is `Unsupported` (`unsupported_corporate_action`, key
  `.merger`, no request). Two rows of the same broker on that date are `SameDayAmbiguous`
  (`corporate_action_ambiguous`, key `.same_day`, no request); rows of different brokers on that
  date are one action. Split rows of different brokers on dates at most `kDifferentDatesWindowDays`
  (10) days apart are one action with rows on two dates (`joinOneEventPairs`) when each date is the
  other's only such neighbour, neither has a merger row and no broker has rows on both; its question
  has `mAskSameEvent`. Other such rows make the actions `DifferentDatesAmbiguous`
  (`corporate_action_ambiguous`, key `.different_dates`, parameter `days`, no request), until the
  identity question exists; a merger row never pairs with a split.
  No open units just before it is `NoPosition` (`corporate_action_missing_position`); so is a
  pool with `mShortSale` without a unit at the start of the sale's day or any date before it with
  units. Open units whose checked sum
  does not fit `Units` are `NumericFailure` (`corporate_action_unrepresentable`, no request).
  Otherwise the stage asks one
  `corporate_action_ratio_required` question with `CorporateActionRatioQuestion` data (every
  source record, the open units of all brokers just before the row date, and `mDateRanges`: every
  effective date the action may have, as ranges with the units just before the action on them)
  and one `corporate_action_ambiguous` error (key `.ratio_required`) linked to it. The ranges run
  from the day after the previous applied action's effective date (or the first trade) to the day
  before the next action's row date or 31 December (30 January for an action with a row after 31
  December); a trade counts in the units before the action when it is dated before the date.
  Dates with no units, and every date after a sale of more units than were held, are in no range.
  Each range also has `mLowestChangeAfter`, the lowest running total of the trades after the
  action up to the last date (without the trades at most 10 days before the next action's row,
  which that action's question checks, except those on the date itself: each such trade date is
  a range of its own), which the answer check uses. For a pool
  with `mShortSale` the ranges start from the sale's day and end on it at the latest, the question
  has `mEarlierSaleDate` instead of the units on the row date, and the error has key
  `.ratio_required_before_sale` (parameters `saleDate` and `days`). A recorded ratio gives
  `RatioConfirmed`, and the next round applies it; a cancel gives `Cancelled` and
  `corporate_action_ambiguous` (key `.cancelled`, no request), or, for a pool with `mShortSale`
  (a sale up to 31 December), the `incomplete_history` error of the sale. `SeparateEventsAnswer`
  (only for `mAskSameEvent`) gives `DifferentDatesAmbiguous` and `.different_dates`.
  `matchCapitalPools` adds each confirmed action once, so its loop always ends.
- **Rows after 31 December.** The stage also groups the action rows of `mLossAnalysisEvents`
  after 31 December. One is asked like a first action when the pool stopped at it (`mShortSale`,
  the matcher's first unconfirmed action includes these rows), or when the pool is `Matched` and
  a loss sale of the year (a loss-making match within the holding period) has its window end on
  or after the row date; a merger row there is not asked (the loss stage blocks the sale). A
  confirmed one with a date after 31 December is in `FifoIsinResult::mLaterActions` (never
  applied to the lots) and is marked `Applied` with the same warning. Other rows after
  31 December are ignored. The units before such a row are the lots at 31 December plus the
  January trades before it, each after the `mLaterActions` up to its date (`previewSplitUnits`).
  A January sale there of more units than were held makes the question end at that sale, as for
  `mShortSale`, when the row is at most 10 days later, but a cancel gives `.cancelled` (the sale
  is not in the selected year); otherwise, and with no units before the row, the action is
  `NoPosition`.
- **Later actions.** A merger, two rows of the same broker on a later date, or split rows of
  different brokers close together get the same errors as above (not linked to the open
  question, because answering it does not resolve them).
  Each other later action gets `WaitingForEarlierAction` and `corporate_action_not_applied` (key
  `.waiting_for_earlier_action`, parameter `earlierDate`), linked to the open ratio question of
  the first action when there is one, so capital gains needs input rather than failing.
- Every diagnostic has the stage `corporate_action`, affects capital gains only and carries the
  action's row date and the sources of all its rows. No units, amounts or ratios as text are in the
  messages.

### Loss eligibility contract

`LossEligibilityChecker::check` (`src/processors/loss_eligibility.hpp`) takes the merged
statement, the reporting-year ledger, the FIFO result of the same run and the session's
`RunDecisions`. It applies the 30-day loss rule as [`tax_rules.md`](tax_rules.md), "How the app
applies the rule", describes it, and changes nothing in its inputs. ISINs whose pool stopped are
skipped: the report builder blocks them anyway. So are bonds, derivatives, crypto and private
funds: they never reach Doh-KDVP, and the capital report gates block them. An ISIN of unknown
class is checked, because it may still be classified as a share or a fund.

- **Inputs per ISIN.** The sales up to 31 December with their matches (`mEarlierMatches` and
  `mMatches`), every purchase in `mCapitalEvents`, and from `mLossAnalysisEvents` the purchases
  and corporate actions after 31 December. Sales after 31 December are left out. The stock after
  each sale comes from walking the ISIN's events. The confirmed actions of the pool
  (`mAppliedActions` and `mLaterActions`) make units comparable: a trade's units times the old
  numbers of the actions dated on or before it and the new numbers of the others (`UnitScale`);
  the stock is adjusted at each action and rounded once like the FIFO lots; a replacement is
  stored in the sale's units, rounded up to 8 decimals (the safe side). When the product of the
  new or of the old numbers is above 2^64, the ISIN's loss sales are `NumericFailure` with one
  `arithmetic_overflow` error.
- **Result.** `LossEligibilityResult` has one `LossEligibilityIsin` per trade instrument, by the
  same index as the FIFO result, each with its sales of the year that have a loss unit
  (`LossSaleEligibility`): sale, window, loss units, the replacements taken (purchase, date, units),
  the known ineligible units, the units blocked by a `yes` answer and a state: `Decided`,
  `WaitingForAnswer`, `Cancelled`, `CorporateActionInWindow` (an action after 31 December inside
  the window without a confirmed ratio, or a merger, with the action) or `NumericFailure`. Neither
  of the last two is asked.
- **Question.** The open sales of one ISIN give one `loss_eligibility_confirmation_required`
  request with `LossEligibilityQuestion` data and one `loss_eligibility_unresolved` error per open
  sale (key `loss_eligibility_unresolved.confirmation_required`, the sale's sources and date),
  linked to the request. The checker looks the question up in `RunDecisions`: `no_other_activity`
  decides the open units as eligible, `other_activity` as blocked (with one warning per ISIN, key
  `loss_eligibility_unresolved.other_activity`, the sources of its open sales and the date of the
  first), and a cancel gives each open
  sale the error with key `loss_eligibility_unresolved.cancelled` and no request. The request's
  diagnostic indices count from the checker's first diagnostic; `appendStageOutput` moves them to
  the numbering of `ApplicationResultInput`.
- **Without a way to ask.** A caller that does not ask (the debug tools) passes
  `OpenLossQuestions::AssumeBlocked`. Without a recorded answer, the open units are
  then treated as with `other_activity` (`mAssumedBlocked`), with no request and one
  `loss_eligibility_unresolved` warning per ISIN (key
  `loss_eligibility_unresolved.assumed_blocked`, the sources of its open sales and the date of the
  first), so the user can correct the loss in eDavki.
- No units, amounts or prices are in the messages.

### Doh-KDVP report model contract

Tax processing produces broker-neutral report models, and the XML writers only format them. The
models hold no CSV, JSON, HTTP, frontend or XML fields and need no XML library. This section
covers shared metadata and the Doh-KDVP model (`include/taxbroker/report_models.hpp`); the income
models (Doh-Div, Doh-DHO, Doh-Obr) are in "Income report model contract" below.

**Shared metadata.** `ReportMetadata` holds the taxpayer (`TaxpayerMetadata`), the reporting year
and the filing type (`Original` or `SelfReport`). It exists once for a run, and a form writer
receives it next to the model of its form; no form model repeats a taxpayer field. Only the tax
number and the residence are required; name, address, city, post code, post name, birth date,
email and phone are optional, as in the FURS schema. The model does not check the values. The
code that reads the user's input does, whether the user typed it or loaded it from a file. The
taxpayer fields can come from a saved profile once profiles are kept between runs, and the user
can change any of them before each run. The filing type is chosen for each run (`Original` unless the user picks
`SelfReport`) and is never saved with the profile. Only the Doh-KDVP writer uses it: eDavki
does not import the self-report workflow for Doh-Div, Doh-DHO and Doh-Obr (draft import,
2026-11-10, [`verification.md`](verification.md)), so those writers always write `O` with
`Original` and `SelfReport` `false`. The
writer adds constants that are not data: the taxpayer type `FO`, the period dates (1 January and
31 December of the year), the list type `PLVP`, row numbers and the workflow codes.

**Doh-KDVP model.** `CapitalGainsReportKDVP` holds one `KdvpInventoryList` (a *popisni list*) for
each ISIN with a sale in the selected year, in ISIN order. A list has the ISIN, the name, `IsFond` and
its rows in ledger order. A row is a `KdvpPurchaseRow` (F1 date, F2 method `B`, F3 units, F4 unit
value) or a `KdvpSaleRow` (F6 date, F7 units, F9 unit value, F10), plus F8, the stock. The rows
are the filing rows of [`tax_rules.md`](tax_rules.md), "Doh-KDVP filing rows"; the internal FIFO
matches are not part of the model. A row keeps a `StatementEventReference` to its source trade
(an index, no file or row), so a later summary can name the sources. `UnitPrice` and `Units` keep
their eight decimals, and rounding to the XML text is the writer's job.

`KdvpLossRule` has only `NotBlocked` (F10 true) and `Blocked` (F10 false). It has no "unresolved"
value, so an open loss check can never be written as a decision. Only the loss rule
produces `Blocked`; a partly eligible sale is several sale rows that share the source sale.
`IsFond` is always false for now: the class check (what belongs in Doh-KDVP) is the job of the
classification gate, not of the builder.

**Building the rows.** `CapitalGainsReportBuilder::build` takes the merged statement, the
reporting-year ledger, the FIFO result, the loss eligibility result and the ISINs that the user
agreed to leave out. The inputs must come from the same run: the FIFO and loss results have one
entry per trade instrument, by the same index. For an ISIN with at least one sale in the selected
year:

- A purchase row is made for each purchase that the matches of the year use, at its ledger
  position, with F3 the sum of its matched units. The matches of one purchase are grouped by
  their exact unit value (the match basis over its units): an unchanged lot has one group and
  F4 is the trade price; a lot a split changed lists the units after the split with F4 the exact
  basis over them, rounded once to 8 decimals; a purchase used both before and after a split has
  one row for each group, in the order the sales use them (a group is a run of the same state:
  after a split and a reverse split that cancel, units sold before, between and after them are
  three). A sale row is made for each sale of the year, or several rows for a sale whose loss is
  blocked in part (below), with F7 the sum of its matched units. F8 is the running total of the
  listed purchase units minus the listed sale units.
- Matches that use units past the holding period ([`tax_rules.md`](tax_rules.md), "Long-held
  investments") are tax free and are left out of F3 and F7. A sale with only such units has no
  row, and an ISIN with no listed unit has no list. The units left out are in `mLongHeld` (ISIN,
  name, sale, units) for every ISIN that is not blocked, so they stay visible beside the report.
- The blocked quantity of a sale (`ineligibleUnits`) is given to its loss-making matches within
  the holding period, in FIFO order: each takes units until the quantity is used up. The units
  taken are `Blocked` (F10 false), all other listed units `NotBlocked`; neighbouring runs with the
  same value are one row. A sale with no blocked unit, or whose units are all blocked, is one row.
- The ISIN is blocked, with no list, for the first of these reasons that applies:
  `FifoStopped` (the pool stopped; all its sales of the year are named), `UnitValueTooLarge` (the
  F4 of a purchase row does not fit a `UnitPrice`, for example after a large reverse split; only
  the sales that use such a unit value are named) or `LossNotReportable` (a sale whose loss-rule
  state is not `Decided`; only those sales are named). A loss on units past the period is left
  out with them, because it cannot reduce the tax base. A gain of zero is not a loss. A stopped
  pool without a sale in the year does not block. A loss from an earlier-year sale is not in the
  matches of the year and never blocks.
- An ISIN with a sale in the year that the user agreed to leave out goes to `mExcluded` with
  every sale of the year and the reason the builder blocked it, if it did; it gets no list and no
  long-held units, also when the builder would not have blocked it (the gates block a private
  fund, for example). Any other blocked ISIN goes to `mBlocked`. The report exists only when `mBlocked` is
  empty, and then holds a list for every ISIN that is not left out.

The builder adds no diagnostics and no message keys. The capital report gates turn `mBlocked`
into errors (see "CLI report command (walking skeleton)"), and the application result
turns them into report statuses. `FailureResolutionStage` (see "Choosing whether to exclude
failed capital ISINs" in `diagnostics.md`) asks which of those ISINs to leave out and passes the
answer to the builder.

### Income report model contract

The income models are in `src/processors/income_report_models.hpp`. Like the Doh-KDVP model they
are broker-neutral, hold no CSV, JSON, HTTP, frontend or XML fields and need no XML library. The
taxpayer and the year are not repeated: a writer gets `ReportMetadata` beside the model of its form.
The processors build the models. The Doh-Div, Doh-DHO and Doh-Obr writers (`DivXmlWriter`,
`DhoXmlWriter`, `ObrXmlWriter`) exist, and the report command calls them.

- **Amounts.** Every amount is EUR `Money` at 4 decimals and is never rounded in the model; the
  writer rounds once to 2 decimals. A foreign payment and its foreign tax are converted before
  the model is built, at the official ECB rate of the receipt date (the built-in or supplied
  files, as for trades). The broker `fx_rate` is never used for a form value.
- **Rows.** Doh-Div (`DividendReportDiv`) and Doh-Obr (`OtherInterestReportObr`) have one row per
  payment, with the source event. Doh-DHO (`DepositInterestReportDHO`) has one row per payer,
  interest kind and source country: the exact sum of its imported payments, which the row lists.
  There is no user-entered amount; the Doh-DHO question about other banks is a yes/no answer and
  the user adds those banks in eDavki.
- **Only resolved amounts.** A row exists only when its date, gross amount and withholding are
  known. Its kind is a meaning (`DividendKind`, `DepositInterestKind`, and for Doh-Obr a variant
  of `BrokerCashInterest` or `BondCoupon` that keeps the coupon's ISIN), never a form code: each
  writer maps it to the code it verifies.
- **Withholding.** `ForeignWithholding` is the EUR foreign tax of the payment (0 allowed) with a
  flag that the export had no tax. A blank source tax is a foreign tax of 0 with the flag set,
  never a question. A Doh-Div payment of a Slovenian payer is not a row: the company
  deducts the tax itself for a tax resident, so nothing is reported and the payment is an excluded
  item (`slovenian_tax_settled`). A Doh-Obr coupon of a Slovenian issuer is a row, because the payer
  does not withhold on listed bonds. Doh-DHO keeps one plain EUR foreign tax sum.
- **Payer facts are optional.** `IncomePayer` holds the name, address, residence country, tax
  number and identification number only as far as the app found them, plus the reference-file row
  they came from (`CatalogueSource`). The same holds for the source country. An empty fact is
  left out of the XML, and a warning tells the user to complete it in eDavki. When an ISIN has
  several reference rows, the user picks the row to use or skips; a skip, or no answer, gives
  empty facts, and the report is still made. The schemas allow
  this (every element of Doh-Div and Doh-DHO is optional); eDavki imports such a file as a draft
  and lists critical content errors for the missing facts, which the user completes
  (`verification.md`).
- **Treaty claim.** `TreatyClaim` is present only on a row with a foreign tax and a payer country,
  with the treaty text written as `ReliefStatement` and the reference row it came from (none for a
  text the user typed). The stage makes it from the reference files, or from the user's text. The
  text is optional: a Doh-Obr claim the user only said yes to has none and is written as `DA`;
  Doh-Div always has a text and its writer stops the file for a claim without one.
- **Filing evidence.** The model holds no evidence list. The user attaches it in eDavki; the result
  tells the user to do so when a row has a foreign tax above 0 or a treaty claim, derived from the
  rows.

### Dividend stage

`prepareDividendPayments` (`src/processors/dividend_payments.hpp`) turns the dividends of the
reporting year into the Doh-Div model. It is a stage of its own, run by the income reports stage
below. It reads the merged statement in ledger order, the payer catalogue, the official rates and
the recorded decisions, and returns the `DividendReportDiv` rows, the excluded items, the open
payer questions, the diagnostics and the rates it used.

- **Per ISIN, first.** The payments of the year are grouped by ISIN (one payer lookup each, in ISIN
  order). The payer is found by `findPayer`: one entry gives the facts and the reference row;
  nothing found gives empty payer name, address and numbers and `dividend_payer_not_found`; entries that disagree ask a
  `PayerChoiceQuestion` (`income_details_required`) and the answer picks one row or skips. A skip
  and an open question both give empty facts and the same warning. The question never stops a form:
  `ApplicationResultBuilder` lists it with Doh-Div but does not make the form `needs_input`.
- **Country.** The country of a row (payer and source country, for a dividend and a coupon) comes
  from the export countries of the payments (`mExportCountries`) and the used reference row. Two or
  more different countries are a `PayerCountryQuestion` (`income_details_required`, candidates
  sorted by country, each with its origin `export`, `reference` or `export_and_reference`), asked
  after the payer question is answered or skipped; the answer is a country code (any ISO 3166-1
  code, not only a candidate) or a skip. A skip or no answer gives no country and
  `payer_country_unresolved` (key `.dividend` or `.interest`); the question never stops a form.
  With fewer than two countries the first of: the one export country, the used reference row, the
  one country of the reference rows that disagree, the first two letters of the ISIN when they are
  an ISO 3166-1 country (`payer_country_from_isin`, once per ISIN and form). The list of countries
  is `isIsoCountryCode`, also used by the reference files, the session and the XML check.
  `tax_rules.md`, "Payer country of an income row", has the rules.
- **Slovenian payer.** A dividend payer with the country `SI` has no row. One excluded item per ISIN
  (`dividend`, `slovenian_tax_settled`) lists the sources of its payments, without a warning
  (a tax rule leaves it out on purpose). Its payments need no rate.
- **Per payment, then.** One `selectOfficialRates` call covers the gross and the foreign tax of
  every reported payment, each in its own currency (`calculations.md`). Its warnings and its errors
  go on as they are; the error of a pair without a rate blocks Doh-Div, and the payments with that
  pair have no row. An amount that does not fit gives the numeric error of the payment, also without
  a row. A blank tax is 0 with the blank flag and no warning.
- **Notice.** One `foreign_tax_evidence_required` warning per run when a row has a foreign tax above
  0.
- **Treaty claims.** After the rows, per payer country that has rows with a foreign tax above 0:
  when the reference files give one valid text for the country, `TreatyClaim` is set on its taxed
  rows (with the first reference row), without a question or warning. Otherwise one
  `TreatyClaimQuestion` (`income_details_required`, never blocking) lists the taxed ISINs; a typed
  text sets `TreatyClaim` on the taxed rows without a reference row; no answer or no claim leaves
  them without a claim. Rows without a payer country get neither.

`appendDividendPayments` adds the diagnostics, requests and excluded items to an
`ApplicationResultInput`, as `appendClassification` does for the classification stage.

### Interest stage

`prepareInterestPayments` (`src/processors/interest_payments.hpp`) sorts the interest of the
reporting year into the Doh-DHO and Doh-Obr models. Like the dividend stage it is run by the income
reports stage below; both share the payer, rate and exclusion helpers of
`src/processors/income_payers.hpp`.

- **Route by who holds the cash.** Each payment has one route. A bond coupon is a Doh-Obr row.
  The parser sets `mCashHolder` on every payment of interest on cash (`CashHolder`, in the common
  interest model): `Bank` means the cash is a bank deposit, so the interest is deposit interest
  (Doh-DHO), and `Broker` means other interest (Doh-Obr cash interest). Trade Republic is a bank
  from 6 December 2023 and a broker before it; Interactive Brokers is always a broker. The stage
  names no broker and no date, and a cash payment without a holder is a programming error
  (`std::logic_error`). A payment is never split, so a payment received after the date counts as
  a whole.
- **Not positive.** A payment of 0 or less, or a Doh-Obr row that rounds to 0.00, is no row, is not
  in the total and gets `interest_payment_not_positive`.
- **Payers.** A coupon's issuer is looked up by ISIN with the dividend rules (found, not found,
  a skippable pick; a Slovenian issuer is not excluded). The payer of a broker's
  cash interest is the only `broker_payers.csv` row of that broker (`brokerPayersOf`); with none or
  several the facts stay empty and `interest_payer_not_found` is given once per broker.
- **Doh-DHO.** The deposit payments of a broker are one row (payer, kind, source country) with the
  exact sums of the interest and the foreign tax and the list of the payments. The total of all
  deposit payments is compared with EUR 1,000 at four decimals. Above it the rows are made. At or
  below it `deposit_interest_total_required` is asked: yes makes the rows and the warning
  `deposit_interest_add_other_banks`, no gives no rows and the reason
  `below_deposit_interest_threshold`, and no answer makes the rows and the warning as for yes. The
  open question is listed but never blocks Doh-DHO, as the payer choice and the treaty questions
  do not block their forms. A payment without a
  rate gives no total and no question; the rate error fails Doh-DHO.
- **Doh-Obr.** One row per payment, in ledger order, with the payer facts and the foreign tax.
- **Notice.** One `foreign_tax_evidence_required` warning per run when a row has a foreign tax above
  0 and a payer country other than `SI`, as for dividends. It is made after the exclusions of the
  form, from the rows that stay, so a left-out investment never asks for evidence.
- **Treaty claims.** For each row with a foreign tax above 0 and a payer country other than `SI`:
  a coupon takes the one `bond_coupon` text of the country and cash interest the one
  `cash_interest` text, without a question, as a `TreatyClaim` with that text. A row without such a
  text has no claim: nothing is asked and nothing is warned, because the element is the text or
  left out, never `DA`. Rows without a payer country have no claim. Dividend texts are never used
  here.
- **Accrued interest.** An ordinary bond with a sale in the year gets the warning
  `bond_sale_accrued_interest_not_reported` (after its type answer; not for a bond whose payer rows all have country `SI`); its coupons are written. A coupon that follows a purchase of its bond, by the dates of the whole history, gets
  the warning `coupon_accrued_interest_not_deducted`; the row keeps the whole coupon.

### Income reports stage

`prepareIncomeReports` (`src/processors/income_reports.hpp`) chains the income stages for the
reporting year and returns the three models (`IncomeReports`: Doh-Div, Doh-DHO, and Doh-Obr with
cash interest and coupons together) and the rates it used. It makes no XML. The order is fixed:

1. `IncomeClassifier::classify` asks the type of each unclear interest or non-share dividend
   payment. The next two stages read the statement it rewrote, when it rewrote one.
2. `prepareDividendPayments`, then the Doh-Div failure choice (`kDividendsFailure`): the failed
   ISINs the user leaves out are removed from the model with `excludeDividends`.
3. `prepareInterestPayments`, then the Doh-Obr failure choice (`kOtherInterestFailure`, which also
   returns the failed brokers) and `excludeCoupons` with `excludeBrokerCashInterest`. The answer
   to the deposit allowance question sets the Doh-DHO reason for having no data.

Each stage appends its diagnostics, requests and excluded items to the same
`ApplicationResultInput`, so each form keeps its own status: a failed or open income form never
changes another form. The `taxbroker report` command runs this stage after the Doh-KDVP stages, so
the capital output and the order of its questions do not move; the income questions are asked in
the terminal, and the income models are not written to files until the income XML writers exist.

### XML writer stage

`writeReportXml` (`src/xml/report_xml_stage.hpp`, in `taxbroker_xml`) is the one place that
calls the four writers. It runs each writer for a form whose model has rows and guards it on its
own: a writer that throws gives one `xml_serialization_failed` error for its form and a log line,
and the other forms keep their XML. The `taxbroker report` command calls it with the capital model
only; the income models are added with the income files. See "XML writer stage" in `diagnostics.md`.

### Doh-KDVP XML writer

`KdvpXmlWriter::write` (`src/xml/kdvp_xml_writer.hpp`) turns `ReportMetadata` and
`CapitalGainsReportKDVP` into one Doh-KDVP document. It lives in its own `taxbroker_xml` target,
the only one that links pugixml, so `taxbroker_core` stays free of XML. It only formats and
checks: it never recalculates FIFO, stock, loss eligibility or holding periods, and never shortens,
cleans or rounds a value to make it fit. The result holds the XML text or, when any field does
not fit the form, one `xml_value_out_of_range` error per field and no XML.

- **Constants the writer adds:** taxpayer type `FO`, `PeriodStart` 1 January and `PeriodEnd`
  31 December of the year, `InventoryListType` `PLVP`, `SecurityCount` the number of lists and the
  other four counts `0`, F2 `B` (*nakup*) and F5 `0.0000` on every purchase row. F5 is the
  inheritance or gift tax paid at the purchase, a known zero for a bought security; the FURS
  instructions fill column 5 on every purchase row, and the legacy output that eDavki accepted
  wrote `0.0000`. The filing type becomes the workflow code (`DocumentWorkflowID`) and its name
  (`DocumentWorkflowName`), written in `edp:Workflow` and in `KDVP`: `O` with `Original` (as in the
  legacy output), or `I` with `SamoPrijava` for a self-report (*samoprijava*). The legacy `S` with
  `SelfReport` is wrong: eDavki expects `I` and `SamoPrijava` (maintainer's eDavki draft import,
  2026-10-27, [`verification.md`](verification.md)).
- **Taxpayer fields:** tax number, name (`name`), address (`address1`), city, post code
  (`postNumber`), post name, birth date and residence (`resident`) in the header; residence
  again as `IsResident`, phone as `TelephoneNumber` and email as `Email` in `KDVP`. An absent
  optional field is left out.
- **Left out:** `Securities/Code`, `KDVPItem/Name`, `F11`, the foreign-tax and tax-relief
  fields. A list without a name has no `Securities/Name`.
- **Numbering and order:** lists and rows in model order. `ItemID` counts from 1, the row `ID`
  from 0 in each list, as in the legacy files that eDavki accepted.
- **Numbers:** F3, F4, F7, F8 and F9 are the exact stored values with four to eight decimals
  ([`calculations.md`](calculations.md), "XML precision"). The largest `int64` at eight decimals
  has 11 integer digits, so no stored value is too large for the form.
- **F10:** on every sale row, `true` for `NotBlocked` and `false` for `Blocked`; purchase rows have
  none. Two sale rows of one source sale stay two rows.
- **Checks** (each failure is an error for the field): tax number of eight digits and not
  `00000000`; year 1000 to 9999; dates with a year from 1 to 9999; ISIN of 1 to 12 characters;
  list name of at most 100 characters and post code of at most 12 (characters, not bytes);
  every text valid UTF-8 without control characters, U+FFFE or U+FFFF; F3, F4, F7, F9 and F8 not
  negative. The schema accepts a negative F8 and an empty ISIN, but only the
  incomplete-history developer export (a list with `mUnsafe`) may write a negative F8 or leave
  F10 out.
- **An empty model** (no lists) gives a valid document without `KDVPItem`; the caller decides that
  the form has no data and writes no file.

The FURS schemas are in `resources/xml/schemas/`: the official `Doh_KDVP_9.xsd`, `Doh_Div_3.xsd`,
`Doh_DHO_4.xsd`, `Doh_Obr_2.xsd` and `EDP-Common-1.xsd` as downloaded, and in
`snapshot-2026-05-12/` the older copy that the legacy app used. The unit and integration tests
check every document they write against both with libxml2. The application does not link libxml2 and does not check the schema at run time.

The integration tests also read the rows of each document back and run their own FIFO over them
(`tests/support/kdvp_row_fifo.hpp`, no production code), which must give the gains and `F8`
values written as literals in each test. A valid schema check is not eDavki acceptance: the schema
makes every F1 to F11 optional. The eDavki draft imports are recorded separately, in
[`verification.md`](verification.md).

Doh-Div, Doh-DHO and Doh-Obr have one committed golden file per scenario in
`tests/test_data/income/` (described in its `README.md`). Each is the whole document from a
synthetic Trade Republic CSV through parse, merge, the income stages and the writer, compared byte
for byte and checked against the form's schema; the values were worked out by hand and also stand
as literals in the test. As for Doh-KDVP, a valid schema is not eDavki acceptance; an eDavki draft
import accepted the Doh-Div, Doh-DHO and Doh-Obr files (2026-11-10, `verification.md`).

### Doh-DHO XML writer

`DhoXmlWriter::write` (`src/xml/dho_xml_writer.hpp`) turns `ReportMetadata` and
`DepositInterestReportDHO` into one Doh-DHO document, in the `taxbroker_xml` target with the
shared helpers of `xml_common` (amount rounding, country and text checks, header). It checks the
whole model first and writes nothing when a value does not fit the form (one
`xml_value_out_of_range` error per field, with the payer name when it is valid text); the rules and
limits are in `tax_rules.md`, "Doh-DHO form contract". An empty model gives a document without
rows and with totals of `0.00`; the caller decides that there is nothing to file. The report command
writes no file for a form without rows.

### Doh-Obr XML writer

`ObrXmlWriter::write` (`src/xml/obr_xml_writer.hpp`) turns `ReportMetadata` and
`OtherInterestReportObr` into one Doh-Obr document with both subtypes, cash interest and bond
coupons, one `Interest` row per payment. Like the other income writers it checks the whole model
first and writes nothing when a value does not fit the form (one `xml_value_out_of_range` error per
field, with the payer name and, for a coupon, its ISIN); the rules are in `tax_rules.md`, "Doh-Obr
form contract". The interest stage finds the row problems first, so that the user can leave out the
failing coupon's ISIN or the failing broker's cash interest; the writer is a guard for the
document and repeats those checks. The shared checks and number helpers are in `xml_common`; the
text check `xmlTextLength` is in the core (`src/utils/xml_text.hpp`), because the stage uses it
too. The report command writes no file for a form without rows.

### CLI report command (walking skeleton)

```text
taxbroker report --year YYYY --taxpayer taxpayer.json [--self-report] [--no-input]
                 [--developer-export] [--support-report] [--ecb-rates FILE...]
                 [--reference-files FILE...] --out DIR --trade-republic FILE...
```

Every input file comes after its own option, as a list: the files after `--trade-republic`,
`--ecb-rates` or `--reference-files` up to the next argument that starts with `-` (or the end).
The same option may be given again and adds to its list. A list option without a file is an
error, and so is a file outside a list or `--`; a file whose name starts with `-` is given as
`./-name.csv`. The broker comes from the option (`--trade-republic` now, an IBKR option later),
never from guessing the file.

The `taxbroker` executable (`src/cli/`) produces a real Doh-KDVP file for a bounded set of cases
and blocks every other case with a structured error; it never guesses. It also writes Doh-Div,
Doh-DHO and Doh-Obr from the income stages; each form has its own status and a blocked form never
stops another. `taxbroker --help` and
`taxbroker report --help` explain the options, the taxpayer file, the output files, the exit codes
and what is supported, blocked and skipped. The whole command is in the `taxbroker_cli` library
(`runCli`), so the tests run it in process; the executable only sets up logging. The
orchestration is in the application service (below); the command only reads the files, asks the
questions, writes the files and prints the summary. The summary reads only the `ApplicationRequest` and the `ApplicationResult`: every fact it
prints (the investments of the year, the units past the holding period, the used rates, the rows of
each income form, the data sources) is a field of the result, so the service and the API show the
same facts.

**Application service** (`include/taxbroker/application_run.hpp`, library `taxbroker_service`).
`ApplicationRun::start(request)` is the one operation behind the command and, later, the local API.
It runs the stages one after the other, in a fixed order and on one thread: the supplied ECB rate
files, the supplied reference files, the parsers (one per broker, the position in the request is
the source index), the merge, the reporting-year preparation, and then the calculation of a
`DecisionSession` (the pipeline below). A supplied rate or reference file that is not valid is a
structured `ApplicationInputFault` and gives no run: no result exists for it. A year that is not
supported gives a result whose four forms are failed. The run hides the data the calculation reads
behind one pointer, so it can be moved, and its header shows only the request, the result, the
session and the faults; answers go through `session()`. It is the one public entry point for the
command line, the local API and later other callers. The service lives outside the core library
because the XML writers (pugixml) link the core. Every stage stays a function of its own and is
tested on its own.

**Pipeline.** The command reads every user file into memory once and builds an
`ApplicationRequest` (`include/taxbroker/application_request.hpp`); a file that cannot be read
fails the run (exit 2) before anything is calculated. Each `--trade-republic` file is parsed with
`parse` of the Trade Republic parser, with its position on the command line as the source index.
The rates are loaded with `loadOfficialRates` (the built-in years and the `--ecb-rates` files; a
supplied file replaces the built-in year it covers) and the reference data with
`loadReferenceData` (the built-in files and the `--reference-files` files, each replacing the
built-in file of its name). A supplied file that is not valid fails the run (exit 2) before
anything is calculated. The summary says where the data came from. Then merge and
reporting-year preparation, once. The rest runs as the calculation of a `DecisionSession`, once
per revision: the classification, the conversion of foreign trades to EUR, FIFO, the
corporate-action stage, the loss rule (`OpenLossQuestions::Ask`), the report builder (no exclusions), the capital
report gates, `FailureResolutionStage` (after `exclude` the command builds the rows again without
the investments the user left out) and the Doh-KDVP XML writer; then the income stages
(`prepareIncomeReports`) and the three income writers through `writeReportXml`. The session builds
the application result, which gives each form its status.
A loss that the files decide in full is written as they decide it. A sale whose blocked units are
only part of its listed units is written in several rows.

**Questions** (`src/cli/decision_prompts.hpp`). After the first calculation the command asks
the open loss and ratio questions in the terminal, one at a time in request order, and the
exclusion question after the questions of investments it does not list (an answer to one of
those can add an investment to its list) and before those of the investments it lists (exclude
drops them, so asking first saves a useless answer), and reads each answer from standard input. It
only shows the request and passes the answer on: the session checks it, records it and calculates
again, and the next question is the first open one of the new result (new revision, new request
IDs). A rejected answer prints the session's message and the question is asked again; a line the
command cannot read (another word, a number that is not whole, a share count of 0 or below, a
date not in the form `YYYY-MM-DD`) is asked again without a session call.

- Classification question: `capital` (`capital_gains`), `exclude`
  (`exclude_from_supported_reports`) or `cancel`, in any case. It is asked before every other
  question, because an exclusion drops the loss and ratio questions of its investment.
- Loss question: `yes` (`other_activity`), `no` (`no_other_activity`) or `cancel`, in any case.
- Ratio question with rows on two dates (`mAskSameEvent`): first whether they are one split,
  `yes`, `no` (`SeparateEventsAnswer`, no ratio is asked) or `cancel`.
- Ratio question: `new : old` (whole numbers) or `before -> after` (share counts above 0 with up
  to 8 decimals, turned into the ratio after / before as `splitRatioFromShareCounts` does), or
  `cancel`; then the effective date (an empty line takes the broker's row date); then the
  preview (`previewSplitUnits`, with the warning of `isUnusualSplitRatio`) and `yes` or `no` (no
  asks for the ratio again). A ratio without a preview (a 0 in `new : old`, equal numbers, a
  position that does not fit) is passed on as it is, so the session's rejection names the problem.
- A share count takes a decimal comma (the Slovenian standard) or a point: `0,52 -> 5,2` and
  `0.52 -> 5.2` are the same. One number never has both, and there is no digit grouping, so
  `1,234` is 1.234 and `1.000,5` is refused; the confirmed preview catches a mistake. This is
  only for typed answers: broker CSV import stays strict ([`calculations.md`](calculations.md)).
  The CLI output keeps the point new logic is implemented.
- A request without question data cannot be answered; it stays open.
- `--developer-export` turns developer mode on: when every listed error of the capital failure
  question is missing purchase history, the prompt also takes `developer`. The command then shows
  one combined confirmation (the warning, every name and ISIN, the risk) and asks `confirm` or
  `cancel` once; on `confirm` it sends the three confirmations of the service, one after the
  other, and writes `Doh-KDVP_YYYY_UNSAFE.xml` (never the normal name) with the files' own
  sales and an `UNSAFE` line in the summary; on `cancel` it asks the failure choice again. The
  option cannot be used with `--no-input`, because the confirmations need a person.
- With `--no-input`, nothing is read and the questions stay open. When the input ends, the
  current and every later question stay open. An open question keeps capital gains
  `needs_input`, a cancelled one makes it `failed`; either way no XML is written (exit 1).
- The summary lists the open requests ("Open questions", with ID, type and investment) and the
  answers of this run ("Answered in this run"). The answers live only in the session's memory:
  nothing else is written, and the next run asks again.

A confirmed split is applied. A sale that lists units of a lot the split changed is written with
the units after the split.

**Excluding investments.** When each error that blocks capital gains names its own investment
(one or several), the command asks one exclusion question for all of them. `exclude` writes the
XML from the other investments. The summary then says `generated without N excluded
investments`, lists each excluded investment with its reason codes under "Excluded investments
(not in the XML)", and marks the resolved errors. If nothing is left, the status is `no data` (exit 0). `fail` and `cancel` write no XML
(exit 1). Like every answer, it is used only in that run.

**Classification** (`TaxClassifier::classify`, `src/processors/tax_classifier.hpp`). It runs
first in each calculation, from the merged statement, the ledger, the parse and merge diagnostics
and the answers, and returns diagnostics, requests, `excludedItems` and the set of left-out
ISINs that the command copies into a copy of the ledger (`mLeftOutIsins`). The FIFO
matcher, the corporate-action stage, the loss rule, the builder and the gates skip a left-out
instrument; `appendClassification` adds the result to the application input.

| Class | Treatment |
| --- | --- |
| `Stock`, `Fund` | Capital gains, nothing new. |
| `Bond` | Left out by a tax rule. A warning and an excluded item per bond with a trade or maturity row in the selected year: units bought, sold and matured (maturities and their cancellations added up by sum), the first maturity date. A bond that only has maturity rows (the purchase is in no file) is listed too. |
| `Crypto` | Left out by a tax rule. A warning and an excluded item per crypto instrument with a trade or corporate-action row in the selected year. |
| `Derivative`, `PrivateFund` | Unchanged: the gates block their sales. |
| `Unknown` | One question per instrument with a trade or action up to 31 December; its parse and merge class errors no longer block. Answer `capital_gains`: nothing changes. Answer `exclude_from_supported_reports`: left out, a warning and an excluded item with the original labels. Cancel: a blocking error. |

The XML is written only when the builder made a report with at least one list and no gate gave
an error that an exclusion did not resolve. A report without lists is `no_data` with
`no_selected_year_disposals`, or without a reason when investments were left out, or when an
excluded item had a sale (or, for a bond, a maturity) in the year. An excluded item that was only
bought gives the normal reason. The capital status of the application result decides the files and the exit code, so a
blocking parser or merger error (a rejected trade or corporate-action row, a file-level error, a
`conflicting_duplicate` of a trade or corporate action) also blocks the report, whatever ISIN or
year it belongs to. The class errors of an unknown-class instrument (the three parser class errors
and the `instrument_asset_class_conflict`) are the exception: the classification question owns
them.
An event the merger rejected is not in the ledger, so only this rule blocks it.

**Capital report gates** (`CapitalReportGates::check`, `src/processors/capital_report_gates.hpp`).
They look only at ISINs with a sale in the selected year, as the builder does. All errors affect
capital gains only and carry no units, amounts or prices:

| Case | Code (stage) | One diagnostic per |
| --- | --- | --- |
| Class private fund or derivative | `unsupported_tax_treatment` error (`fifo_tax`), parameter `assetClass` | ISIN; sources of its sales of the year, date of the first |
| Pool stopped at a foreign trade without a rate | `missing_exchange_rate` error (`fifo_tax`), the key and parameters of the pair's reason (`.no_rate_file`, `.rate_file_ends_early`, `.no_rate_on_date`, `.replaced_by_eur`); `.trade` without parameters for a trade with no known currency | ISIN; date and sources of that trade |
| Adjusted unit value that does not fit F4 (a `UnitPrice`) | `corporate_action_unrepresentable` error (`fifo_tax`), key `corporate_action_unrepresentable.unit_value` | ISIN; date and sources of its first sale that uses it |
| Date after 31 December with a merger row inside the window of a December loss (a split row there has its ratio question or its own error instead) | `corporate_action_not_applied` error (`corporate_action`), key `corporate_action_not_applied.loss_window` | one per merger date, even when several sales share it; date and sources of the merger row |
| Private-market event dated in the selected year | `unsupported_tax_treatment` **warning** (`fifo_tax`) | event |

The class check blocks only `PrivateFund` and `Derivative`. `Stock` and `Fund` pass (`IsFond` stays
`false` for both). A bond and crypto are left out of capital gains by the classification stage,
and an unknown class is asked there (see below), so the gates give nothing for them. A bond or
crypto sale that reaches the gates whose ISIN is not in the left-out set is a `std::logic_error`: debt
securities and crypto never reach Doh-KDVP. A pool stopped for missing history or a numeric failure already has its FIFO diagnostic and gets nothing more, and
one stopped at a corporate action up to 31 December has its diagnostics from the corporate-action
stage. Only
the first stop of a pool is reported, because FIFO stops there. Private-market events (`PRIVATE_MARKET_BUY`,
`PRIVATE_MARKET_SELL`) have only a cash amount, without units or a price, so they are
skipped, a sale included; the warning keeps each one visible. A row that does have shares or a
price stays a skipped event and gets one more parser warning (`unsupported_asset_class`, key
`unsupported_asset_class.private_market_units`, field `shares`, or `price` when only the price
is filled), because those units are not used.
For the same reason a rejected private-market row, or a merger error on a private-market event,
affects no report: it stays an error and is listed in the summary, but it blocks nothing.
When private markets are supported, these errors get the
capital scope back.
An open or cancelled loss question already has its `loss_eligibility_unresolved` error from the
loss rule, so the gates add nothing for it. Each gate goes away when its rule exists: applied
splits and confirmed corporate-action ratios, conversion with official exchange rates, the split
rows of a partly blocked loss, and classification requests.

**Taxpayer file.** One JSON object with `taxNumber` (eight digits, not `00000000`) and
`isResident` (only `true` for now), and the optional strings `name`, `address`, `city`,
`postCode` (four digits, 1000 to 9999), `postName`, `birthDate` (`YYYY-MM-DD`), `email` and `phoneNumber`. Any other key is an
error, `filingType` included: the filing type is chosen for each run (`--self-report`), never
read from the file. An error names the key, never the value.

**Output.** In the `--out` folder (created when missing): `Doh-KDVP_<year>.xml`, `Doh-Div_<year>.xml`,
`Doh-DHO_<year>.xml` and `Doh-Obr_<year>.xml`, each only when its form is `generated`, and
`summary_<year>.txt` for every run that got that far; the same summary goes to stdout. A form that
is not generated writes nothing and leaves a file of an earlier run as it is; the summary names
such a file ("is from an earlier run, not from this run"), so it is never taken for this run's
result. A `generated` form replaces its file. With `--support-report` the command also writes
`support_<year>.json` (`diagnostics.md`, "Support report") through the same temporary-file step as the
summary, and prints `Support report: support_<year>.json`; when a bad ECB or reference file stops
the run it writes only that file. The summary has the input files (file names and
source IDs only), a "Forms" block with one status line per form (rows for an income form, a
reason for `no_data`, `without N excluded investments` or brokers), each ISIN with a sale in the year
(listed with its sale rows and units, the units with `F10` false and how many of them were assumed
without an answer, blocked with its codes and
sales, or only units past the holding period), the excluded items ("Excluded items": ISIN, name,
category, reason and, for a bond, the units bought, sold and matured and the maturity date), the
units past the holding period (`mLongHeld`: ISIN, sale date, units), the
skipped private-market events, and the diagnostics per form (`Doh-KDVP errors`, `Doh-KDVP
warnings`, and the same for an income form that has a diagnostic; a diagnostic that affects
several forms is listed under each), then those that affect no form (with their severity). The
investments and brokers the user left out of Doh-Div or Doh-Obr are listed like the Doh-KDVP
ones ("Excluded investments (not in Doh-Div)", "Excluded brokers (not in Doh-Obr)"). The list of
official rates holds the rates of the trades and of the income, one line per currency and day. It
has units and dates, no EUR amounts.

Every written file is first written as `<name>.tmp` in the output folder. Then `replaceFiles`
(`src/cli/output_files.cpp`) replaces the files one by one: an earlier file is first moved to
`<name>.old`, then the new file is renamed in. When a step fails, the steps already done are undone
in reverse order and every `.tmp` file is deleted; on success the `.old` copies are deleted. A
folder at the path of a file that this run writes is refused before anything is written. The
`.tmp` and `.old` names are the command's own: a file of the user with such a name in the output
folder is replaced (the help says so). So a run
that fails keeps the files of an earlier run as they were; only when an undo step itself fails does
stderr name the files that could not be restored. The summary's lines `ECB rate files to load` and
`ECB rate files to replace` list the years of the capital warnings and of the income forms'
`missing_exchange_rate` errors.

**Exit codes.** `0`: every form is `generated` or `no_data`. `1`: at least one form is
`needs_input` or `failed`; the other forms are still written.
`2`: the command could not run (an option, a missing or invalid file, the output folder); no
output file is changed (a missing output folder may already be created) and stderr names the
problem with file names only, never a folder.

## Local service security

The local HTTP server (`taxbroker_server`) accepts the user's broker files, so these rules hold
before any endpoint accepts a file. The server is a presentation layer over the application
service; it holds no tax logic. The request checks and the launch file are the functions of
`src/server/request_guard.hpp` and `src/server/launch_file.hpp`, which the routes call
(`src/server/routes.hpp`, see "Local HTTP server" below).

How the login works: at start the server makes a one-time link token and a session token. It
writes a private launch file that redirects to `http://127.0.0.1:<port>/#token=<link token>` and
opens that file in the default browser. The frontend sends the link token once in
`POST /api/session`; the server answers with an `HttpOnly; SameSite=Strict` session cookie. Every
later request is checked with the session cookie, `Host`, `Origin` and the client header.

| # | Rule | Proof |
| --- | --- | --- |
| R1 | A native launch binds `127.0.0.1` only, never `0.0.0.0` or `::`. `TBR_SERVER_HOST` can change the host (an IPv4 address or `localhost`; an IPv6 address is refused, because the link always uses `127.0.0.1`); a non-loopback host logs one warning at start ("The server listens on a non-loopback address; publish its port on 127.0.0.1 only."). | Server tests: only `127.0.0.1` accepts a connection (`127.0.0.2` does not); the warning is logged once for `0.0.0.0`. |
| R2 | A native launch binds port 0 (a random free port) and reads the port back. `TBR_SERVER_PORT` sets a fixed one; Docker uses `8080`. | Server test: port 0 gives a non-zero port. `ServerConfigTest`: port 80 is refused (a browser leaves the default port out of `Host`, so the check could not match). |
| R3 | In Docker the server listens on `0.0.0.0`, which is safe only because `docker/docker-compose.yml` publishes `127.0.0.1:8080:8080` for `dev` and `prod` (host address `127.0.0.1`, host port `8080`, container port `8080`). The host port must equal `TBR_SERVER_PORT`, because the `Host` and `Origin` checks compare with the server's own port. It needs Docker Engine 28.0 or later: older versions let other computers on the same network reach a port published on `127.0.0.1` (`scripts/run.sh` checks this). | `docker compose -f docker/docker-compose.yml config` shows `host_ip: 127.0.0.1` for both services; `ss -ltn` on the host shows `127.0.0.1:8080` and no `0.0.0.0:8080`. |
| R4 | Two secret tokens per launch, a link token (redeemed once, valid for 120 seconds: `LinkToken`) and a session token. Each is 32 bytes from the operating system random source (`getentropy`, Windows `BCryptGenRandom`; no fallback generator, a failure stops the launch), as 64 hex characters (`makeToken`). Both are kept only in memory and never logged. | `RequestGuardTest` (length, two tokens differ, redeem once, redeem at 119 and 120 seconds, a wrong token does not use it up); server test: the log has no token. |
| R5 | Every `/api/` request except `POST /api/session` needs the session cookie `tbr_session_<port>` (`HttpOnly; SameSite=Strict; Path=/`, found by `findSessionToken`), compared in constant time (`tokenMatches`). The link token is accepted only in `POST /api/session`, once. A missing or wrong token gives 401 with no detail. The port in the cookie name keeps launches apart, because cookies are kept per host name, not per port. | `RequestGuardTest` (equal, different, shorter, cookie text, cookie lookup); server tests: missing, wrong and right cookie, a second redeem, a redeem after 120 seconds. |
| R6 | The browser gets the link token once, in the URL fragment of the launch page (`http://127.0.0.1:<port>/#token=...`). A fragment is not sent to the server, so it is not in server logs. The server opens the launch file in the default browser (`xdg-open <file>` without a shell); the link is never a command argument, because on Linux every user can read `/proc/<pid>/cmdline`. In Docker the server cannot start a browser: `TBR_LAUNCH_FILE` names a file in the shared workspace, and `scripts/run.sh` waits on the host for that file and opens it (`--no-browser` skips this). The server always prints the link too, also after a native start, because `xdg-open` may fail later (no desktop, SSH); it works once and for 120 seconds. The frontend sends the token in `POST /api/session` and removes the fragment with `history.replaceState`. | Frontend test (`login.test.ts`): the exchange and the removed fragment, also when the exchange fails; server test: the cookie is set only after a right link token. |
| R7 | `Host` must be exactly `127.0.0.1:<port>` or `localhost:<port>`, the name compared without case (`isAllowedHost`). Anything else is rejected (against DNS rebinding). | `RequestGuardTest` (own host, `localhost`, other port, foreign name, empty, upper case); server test: a rejected `Host`. |
| R8 | `Origin`, when present, must be `http://127.0.0.1:<port>` or `http://localhost:<port>` (`isAllowedOrigin`). A state-changing request (`isStateChangingMethod`: any method other than GET, HEAD and OPTIONS) without `Origin` is rejected. | `RequestGuardTest` (foreign, `null`, missing on a state-changing and on a safe request, the method names); server test: POST without `Origin`. |
| R9 | No CORS headers at all: `Access-Control-Allow-Origin` is never sent. The frontend is served from the same origin. | Server test: an `OPTIONS` preflight gets no allow headers. |
| R10 | Uploads stay in memory (`InputFile`). The server never writes an upload to disk and never takes a host path from a request. The only file it writes besides the log and the launch file is the taxpayer profile, and only on `POST /api/profile` (the user ticked "Remember my details"); a run writes nothing. | `RequestJsonTest` and `AnswerCallJsonTest` (a `path` key anywhere is refused); `ServerRunTest.ASuccessfulRunWritesNoFile` (an upload, an answer and a download leave the same files in the test's state, runtime and launch folders) and `APathKeyIsRefusedAndTheServerWritesNothing` (a `path` key gives 400 and the same files); `ASuccessfulRunWritesNoFile` also checks that no profile file exists, and `ASavedProfileIsOnlyTheProfileFileAndSurvivesARestartOfTheServer` that a save adds the data folder, the app folder in it and `profile.json`, nothing else; code review item. |
| R11 | Only the per-file limit applies: each file keeps `kMaxInputFileBytes` (100 MiB). There is no total request limit besides what follows from it: an upload may have the request part and 32 files, so its body is read up to 33 times the file limit, and only for a request that passed the guard (see "Local HTTP server"). A larger file gets 413, also a file part sent without a file name. The `request` part is small and is limited to `kDefaultBodyLimit` (1 MiB); a longer one gets 413 `request_too_large` before it is parsed. | `MultipartTest` (limit and limit + 1); `ServerRunTest` (a file over the limit gives 413, one of exactly the limit is accepted); `RunRoutesTest` (a file part without a file name of 11 bytes under a 10-byte limit gives 413, one of 10 bytes does not; a `request` part of 1048577 bytes gives 413, one of 1048576 bytes is not refused for its size); `ServerTest` (the body of a caller without a session is refused above 1 MiB before it is read). |
| R12 | A response holds no absolute path, raw row or money value outside the result fields. Source names are sanitized when the result is serialized. The one stated exception is the `location` text of `GET /api/profile`: the file path with the home folder written as `~` (so without the user name), or left out when the file is not under the home folder. | The JSON serializer tests, `InputFaultJsonTest` and `ServerRunTest` (no response body holds `/home/`, `/tmp/`, `/workspace/` or `C:\`, also for file names that are paths). |
| R13 | A log line holds no money value, raw row, name, ISIN, absolute path, query or token (`diagnostics.md`). Every refusal writes one warning with the status and a fixed word: `Request refused: 403 host_not_allowed` (the guard: `host_not_allowed`, `bad_target`, `origin_repeated`, `origin_missing`, `origin_not_allowed`, `client_header`, `no_session`, `session_wrong`), `Login refused: 401 link_used` (`link_used`, `link_expired`, `link_wrong`, or 400 `bad_request`) and `Run request refused: 400 too_many_files (limit 32)` (the run API: the limit for a size or count refusal, a reason word or message key where a code has several causes). A step that works writes one info line with counts and statuses only (`Run started`, `Run restarted`, `Run result`, `Run details`, `Answers accepted` with the question types, `Answers deleted`, `Report downloaded`, `Support report downloaded`), so a bug report shows what ran. The profile routes write `Profile loaded (fields <n>, invalid <n>)`, `Profile saved`, `Profile deleted`, the warning `Profile unreadable`, the errors `Profile not saved (<exception type>)` and `Profile not deleted (<exception type>)`, and the refusals `Profile request refused: 400 bad_taxpayer (<taxpayer key>)`, `413 request_too_large (limit 32768)`, `405 method_not_allowed` and `404 not_found`; never a value or a path. A body that stops writes `Request not finished: timeout` or `Request not finished: connection_closed`. The command line and the server also write one start line (`TaxBrokerReport <version> started: <program> (<build type>, <os> <cpu>)`, compile-time facts only), the load stage lines (`Stage rates`, `Stage reference`, `Stage parse and merge`, `Stage year`: counts and milliseconds), `Calculation done (<ms> ms)` and, at `debug`, one line per calculation stage; the log level is `error` by default and can be raised with `--log-level` (one command line run) or `/api/log-level` (the rest of a server session), and a higher level holds more lines, never a private fact. The log level route writes `Log level set: <level>` and the refusals `Log level request refused: 400 bad_log_level`, `413 request_too_large (limit 1024)`, `405 method_not_allowed` and `404 not_found`. The log file route writes `Log downloaded: <n> bytes` and the refusals `Log request refused: 404 no_log_file` and `405 method_not_allowed`. The command line writes one warning `Report stopped: <reason>` where it stops (`input_files`, `taxpayer_file`, `input_file`, `input_fault (<kind> <message id>)`, `output_folder`, `output_file`, `output_replace`), never a file name. An unexpected exception of the command line writes `Report failed unexpectedly (<exception type>)`, and an XML writer that throws writes `XML writer of <form> stopped (<exception type>)`; never the exception text. A handler that throws writes `Run request failed: <route> (<exception type>)` and the HTTP layer writes `A request handler failed: <verb> <step> (<exception type>)`, never the exception text. | Review of the log lines of the server; `ServerRoutesTest` (one line per guard and login refusal, one line when the body limit is asked first, no token in the log); `ProfileRoutesTest` (every profile line, and that no log line holds a name, a tax number or the folder); `RunRoutesTest` (one line per refusal, the limit in it, no file name or content in it; the start, result, answer, delete, download and support report lines with no ISIN, name, request ID or file name; the failure line); `ExceptionKindTest`; `ReportXmlStageTest` (the writer line holds the kind, not the text); `CliLogTest` (the start, stage, result lines at `info`, the `Report stopped` lines, the calculation lines at `debug`, nothing at the default level, no ISIN, name, tax number, file name, folder or amount in the log, the unexpected-failure line); `BuildInfoTest`; `LogLevelRoutesTest`; `LogFileRoutesTest` (the download of the older and the current file, the cut of a large file, the refusals); `ApplicationRunIntegrationTest` (one `Run started` or `Run restarted` line); `ApplicationResultJsonTest` (the summary and the details); `ServerTest` (the 413 lines of the HTTP layer, the guard's own line for a large or chunked refused body, the `Request not finished` lines and none for an idle connection, one `Accept failed` line per series, the failed-handler lines, the browser program line); `TradeRepublicParserTest` (the row line holds the message key, not the cell text). |
| R14 | The per-user state folder holds the log: Linux `$XDG_STATE_HOME/taxbroker/` (an absolute path only), else `~/.local/state/taxbroker/`; macOS `~/Library/Application Support/TaxBrokerReport/`; Windows `%LOCALAPPDATA%\TaxBrokerReport\`. On POSIX the folder is created with mode `0700`. The launch file is in `$XDG_RUNTIME_DIR/taxbroker/` when `XDG_RUNTIME_DIR` is an absolute path (a per-user folder that is cleared at logout, and a folder that a browser installed as a snap can read, unlike the hidden state folder; tried on Ubuntu with Brave installed as a snap, started by `xdg-open` from the static `taxbroker_server`), else in the state folder. In Docker the log lives in `/var/log/taxbroker` and the launch file is `TBR_LAUNCH_FILE`. The log rotates at 5 MiB and keeps 3 rotated files (`taxbroker.1.log` to `taxbroker.3.log`) besides the current one; the command line and the server write the same file. Without an absolute `HOME` (and without `TBR_LOG_FILE`) no log file is written and the program goes on. The saved taxpayer profile is `profile.json` in the per-user data folder, which is another folder: Linux `$XDG_DATA_HOME/taxbroker/` (an absolute path only), else `~/.local/share/taxbroker/`; Windows `%APPDATA%\TaxBrokerReport\` and macOS `~/Library/Application Support/TaxBrokerReport/` come with the packaging step. The folder has mode `0700` and the file `0600` (written to `profile.json.tmp` and renamed, a symbolic link is refused). Without an absolute `HOME` or `XDG_DATA_HOME` there is no data folder: the server still starts, `GET` and delete answer `saved: false`, and a save is 500 `profile_not_saved`. In Docker the `prod` service keeps it in the named volume `taxbroker_data` (`XDG_DATA_HOME=/var/lib/taxbroker`) and `dev` in `runtime/data`. | `LoggerFileTest` (the file in the state folder, `TBR_LOG_FILE` first, no file without an absolute `HOME`, the rotation: the files `taxbroker.log` to `taxbroker.3.log` and no fourth, the limit 5 MiB and 3 files); server tests: the mode of the launch folder, `StateFolderTest` (the folder choice and `0700`, also for the data folder), `ProfileStoreTest` (the modes, the replacement, the symbolic link, the location text, no data folder), `ProfileRoutesTest` (no data folder). |
| R15 | The server runs as a normal user; the `prod` image runs as `app`. | `docker/Dockerfile` (`USER app`). |
| R16 | Accepted risks. (a) A process of the same user can read the input files directly, so no token can protect against it. (b) A browser extension with access to all sites can read the page. (c) Another local program on `127.0.0.1` receives the cookie, because cookies are per host name, not per port; every request is still checked for the session token. (d) No TLS on the loopback: reading loopback traffic needs root, which can read the process memory anyway, and browsers treat `http://127.0.0.1` as a secure context. A desktop window without a port (webview) would remove (b) and (c); it needs a different web engine per platform and a non-static Linux build, so it is not used. | Accepted risk. |
| R17 | Every `/api/` request needs the header `X-TaxBroker: 1` (`kClientHeaderName`, `kClientHeaderValue`), else 403. A foreign page cannot send it without CORS. | `RequestGuardTest` (the constants); server test: a request without the header gets 403. |
| R18 | Every response carries the seven security headers of `securityHeaders()`: content policy, `nosniff`, no referrer, no framing, same-origin opener and resource policy, and `Cache-Control: no-store`. `script-src` never allows `'unsafe-inline'` or a foreign host. | `RequestGuardTest` (the seven names and values); server test: headers on a success and on an error. |
| R19 | The launch file is readable by the user only (mode `0600` on POSIX, also when it existed), is never written through a symbolic link (`writeLaunchFile`) and is deleted after the exchange and at shutdown. | `LaunchFileTest` (mode, replaced file, symbolic link, missing folder); server test: the file is gone after the exchange. |
| R20 | The frontend is bundled into the server: no CDN, no foreign script, no analytics. | `scripts/frontend.sh check`: the build holds exactly four files, no foreign address, no inline script or style (with a self-test that each rule fails on a damaged copy); `ServerStaticFilesTest`. |

Implementation status: the server listens (`taxbroker_server`, library `taxbroker_server_lib`),
logs the user in, serves the frontend files, the message texts and the run API ("Local HTTP API"
below). Proven by
tests: R1, R2, R4 (the log holds no token), R5 to R9, R13 (the server log lines), R17, R18 and R19
by `ServerRoutesTest` and `ServerTest`; R6 (the launch and the link output) and the launch folder
of R14 by `ServerTest` and `StateFolderTest`; R10 to R12 by `ServerRunTest` (uploads, answers and
downloads over real sockets); R20 by the frontend build check and `ServerStaticFilesTest`. R3 is
the compose file. The log file of R14 is proven by `LoggerFileTest`.

### Local HTTP server

`src/server/` holds the server in small parts: `server_config` (the environment options),
`state_folder` (the folders), `browser_open`, `routes` (the checks and the routes, no sockets),
`http_server` (Boost.Beast and Asio) and `server` (the start). It reaches the backend only
through the public headers. The routes are unit-tested by calling `Routes::handle`; the sockets
are tested with real connections on `127.0.0.1`.

One thread runs the `io_context`, so many connections can be open (a browser keeps several
alive), but only one handler runs at a time and the business code stays single-threaded. A
header read, a write or a wait between two body chunks that takes longer than 30 seconds closes
the connection; the header block is limited to
8 KiB (431). A bad request gets 400 and a failing handler 500, with no detail.

The header is read first. Then `Routes::bodyPolicy` decides the body limit from the header alone:
`POST /api/run` that passes the guard (steps 1 to 7 below, with the session cookie; no token is redeemed) may send
33 times the file limit (the `request` part and 32 files); every other request, also one that
fails the guard, may send 1 MiB (413 above, at once when `Content-Length` says so). So a caller
without a session cannot make the server hold a large body. `Expect: 100-continue` gets
`100 Continue` only when the guard passed. The body is read in chunks; the 30 seconds apply to the
header, and to the wait between two chunks of the body, so a slow upload is not cut off while
bytes still arrive. The body is moved out of the parser into the request, not copied.

Start order: read the options; bind and read the port back; make the two tokens; choose the
launch file (`TBR_LAUNCH_FILE`, else the runtime folder, else the state folder) and make its
folder with mode `0700` (a `TBR_LAUNCH_FILE` folder must exist already); write the launch file
(`0600`); open it in the browser (not when `TBR_LAUNCH_FILE` is set); print the link; serve. On
SIGINT, SIGTERM or `stop` the server closes the port and deletes the launch file.

`Routes::handle` checks a request in this order and stops at the first failure:

| Step | Check | Fails with |
| --- | --- | --- |
| 1 | Exactly one `Host` header that passes `isAllowedHost` (R7) | 403, no body |
| 2 | The target starts with `/` (the query is cut off) | 400 |
| 3 | A path outside `/api`: `GET` and `HEAD` of the exact path of a frontend file (`/` is `index.html`) give the file (200); another method on it gives 405 with `Allow`; another path, also `/index.html` and a path with `..`, gives 404. A build without the frontend serves only a placeholder page at `/` | |
| 4 | At most one `Origin` header that passes `isAllowedOrigin` (R8) | 403, no body |
| 5 | Exactly one `X-TaxBroker: 1` header (R17) | 403, no body |
| 6 | `POST /api/session`: the body is JSON with a string `token`; `LinkToken::redeem` (R4, R5); success is 204 with `Set-Cookie`, the launch file is deleted (R19) | 400 `{"error":"bad_request"}` or 401 |
| 7 | The session cookie (several `Cookie` headers are joined) passes `tokenMatches` (R5) | 401, no body |
| 8 | `GET /api/session` is 204 (the frontend asks after a reload); another method on it is 405 with `Allow`; `GET /api/messages` is the message texts (JSON, see "Frontend"), another method on it is 405; `GET /api/version` is `{"version":"<project version>"}` (the CMake project version, for the page footer), another method on it is 405; a path `/api/run` or below it goes to `RunRoutes`, `/api/profile` or below it to `ProfileRoutes` and `/api/log-level` to the log level route (`GET` answers `{"level":"error"}`, `POST {"level":"info"}` sets it; `error`, `warn`, `info` or `debug`; another body is 400 `bad_log_level`, over 1 KiB is 413), `/api/log` to the log file route (`GET` answers the log as `taxbroker-log.txt`; another method is 405, no log is 404 `no_log_file`) ("Local HTTP API"); an unknown path is 404 | |

The guard runs before the route lookup, so an unknown `/api/` path without a cookie gives 401,
not 404: the paths cannot be discovered. Every response carries the seven `securityHeaders()`
(R18) and never an `Access-Control-*` header (R9).

### Frontend

`frontend/` is a Preact 10 and TypeScript project built with Vite. It has no business logic: the
backend decides every tax fact, status and question, and the frontend only shows them and sends
the answers. `package.json` names exact versions (no `^`), `package-lock.json` is committed, and
every install is `npm ci --ignore-scripts`, so no install script of a dependency ever runs.

- **Four files.** The build writes `index.html`, `assets/app.js`, `assets/app.css` and
  `assets/logo.webp` to `build/frontend/dist`, with fixed names (no hash; every response is
  `Cache-Control: no-store`). With the CMake option `TAXBROKER_FRONTEND_DIR` set to that folder
  the four files are embedded in `taxbroker_server` as byte arrays (the configure fails when one
  is missing), so the application reads no file at run time. Without the option the server
  serves only a placeholder page, so the C++ builds and tests need no Node. `scripts/run.sh` and
  the production image always embed the frontend.
- **Content policy.** The page has one `<script type="module" src>` and one stylesheet link, no
  inline script, style or `style=` attribute, no web font and no foreign address (R18, R20); the
  one exception is the link to the project's repository on the About page, which the user opens
  and the page never fetches.
  `scripts/frontend.sh check` fails the build when the output has another file, when a file is
  over its size budget (`app.js` 128 KB, `app.css` 15 KB, `logo.webp` 12 KB, `index.html` 4 KB, not
  compressed), when an address other than an XML namespace name or that repository link appears
  in a file, or when
  `index.html` has an inline script or style.
- **Tests.** Unit tests are Vitest files named `*.test.ts` that sit next to the module they test
  (`run/taxpayer.ts` and `run/taxpayer.test.ts`). The build never bundles them, because nothing
  imports them, and `tsc --noEmit` checks them like the code. The screens (`screens/*.tsx`) hold
  no logic and have no tests: the logic lives in the modules beside them. A test for a component
  would be a `*.test.tsx` file, and `include` in `vite.config.ts` would then need that pattern.
- **Login.** The link `/#token=...` is exchanged once with `POST /api/session` and the fragment
  is removed with `history.replaceState`, also when the exchange fails (R6). A page without a
  fragment (a reload) asks `GET /api/session`; so does a refused exchange (401), because a used
  link in a second tab may still have a working cookie. The states are `ready`, `link_invalid`
  (401 on the exchange and no cookie) and `session_ended` (401 or no server). Every other call
  goes through one client that adds `X-TaxBroker: 1` (R17); a 401 or a lost server ends the
  session in that one place, and no more call is sent.
- **Texts.** The language is Slovenian by default, English can be chosen (kept in the browser as
  `tbr.language`, only `sl` or `en`). The texts of the interface are in
  `frontend/src/i18n/ui_texts.ts`, typed so that a missing language fails the type check. The
  texts of the diagnostics are not copied: `GET /api/messages` (behind the session, built once
  per process from the embedded table) returns `messages` (every key with `en`, `sl` and its
  `params` of name and kind `number`, `text` or `term`) and `terms` (every term id with `en` and
  `sl`). The frontend fills `{name}` from `messageParams`, translates a `term` parameter from
  `terms` and shows the "unknown message" text with the key for a key it does not know. It never
  shows the English `message`.
- **Quality goal.** Plain screens with one message and one action, a small set of design tokens
  (`theme.css`: colors of the project icon, system fonts), no heavy library, and no polling in the
  shell. The header has a dark/light icon button (inline SVG, no icon pack). The theme is the
  attribute `data-theme` of `<html>`, light by default and kept in the browser as `tbr.theme`; the
  system setting is not used, because it does not work on every Linux desktop. A later screen
  that polls must back off and stop while the tab is hidden (`document.visibilityState`).
- **Pages.** After the login the page has four pages: the welcome page (home), the report screen
  (the input screen below), Manual and About. The page is the address hash (`#/`, `#/report`,
  `#/manual`, `#/about`; anything else, a login link included, is the home page), so the back
  button and a reload keep the page and the server serves only `/`. The header shows the logo and
  name (a link home), the four links in that order, the language switch and the theme button; on
  a narrow window the links wrap to a second row. The report screen is mounted once and only
  hidden on the other pages, so the chosen files and the result stay. A new page takes the focus
  on its heading. The welcome page and About show the no-liability warning (the meaning of the
  README "Disclaimer"). Every page has a small footer, centered at the bottom, with the version
  from `GET /api/version` (asked once after the login; no footer when it is not known). About
  shows the license, the privacy promise and a pointer to `THIRD_PARTY_NOTICES.txt` beside the
  program, and links to the project's repository (a new tab, no referrer; the policy header
  `Referrer-Policy: no-referrer` also applies). Manual is one line until the user guide is written.
- **Input screen.** After the login the page shows the form of one run, in
  `frontend/src/run/` (pure code, tested without a DOM) and `frontend/src/screens/` (Preact
  views). One area for each kind of file, never guessed from a name or content: one area per
  broker (the table `input_kinds.ts`, now only Trade Republic; a new broker is one more row),
  then "ECB exchange rate files" and "Reference files" in a folded "Other files" block. The area
  sets the `kind` and `broker` of its files. The broker files can be moved up and down and
  removed. The year is a select from the current year down to 2000 (default the year before the
  current one); a year without built-in rates (before 2016, or the current year) shows a hint that
  trades not in EUR need the rate file of that year. The filing type is chosen for each run,
  original by default. The taxpayer form sends `isResident: true` always (only Slovenian tax
  residents are supported), trims the text and leaves out an empty field (the server refuses an
  empty string); it repeats no rule about a value.
- **Saved taxpayer details.** Under the taxpayer fields a box "Remember my details" (off by
  default) with a hint where the file is (`location` of `GET /api/profile`, or "in the app's data
  folder"). The page loads the profile once after the login: a saved text fills only an input that
  is still empty, and the box starts ticked when a file exists. With the box ticked, "Create the
  reports" first sends `POST /api/profile` with the same taxpayer object and then the run; a 400
  marks the field and stops, a 500 shows "Your details could not be saved." and the run goes on.
  Unticking the box while a file exists opens an in-page warning with Cancel (focused; Escape is
  Cancel) and Confirm delete, which calls `POST /api/profile/delete`; the run button waits while
  the warning is open. No browser dialog (it can be suppressed and is not translated). Notices
  for an unreadable file, for invalid fields (left empty, named) and for an unknown key (one
  general notice). The pure state (`rememberBoxStep`) and the loading (`formFromProfile`) are in
  `run/profile.ts`.
- **Request order and file numbers.** The upload has the part `request` first, then `file0` and
  on: the broker files of the first broker area, the next broker areas, the rate files, the
  reference files. The source IDs of the diagnostics count only the broker files (`source-0001`
  is `file0`), so a broker file that shares its name with another gets the suffix
  `(file N)`, N being its position among the broker files, and the result block lists the files
  in that order. There are no user labels: nothing typed by the user can change the order or the
  work. The server keeps its answers when the files change (a new upload keeps them, see "Local
  HTTP API"); the page marks the shown result "out of date" on any change of the inputs and
  offers "Create the reports again".
- **Before sending.** The page refuses a file over 100 MiB and more than 32 files itself
  (`run/limits.ts`, the numbers of `kMaxInputFileBytes` and `kMaxUploadFiles`), because a refused
  large body can end in a lost connection that the client would read as "session ended". The
  answers 400 (a code text, for `bad_taxpayer` the text under the field the server names), 413
  and 422 (an `inputFault` text from the message table, with the file name and row) are shown
  next to the form; a lost session shows the "open the link again" screen. There is no
  `GET /api/run` on load (it logs a 404 warning before the first upload), so a reload shows the
  empty form.
- **Result screen.** After a run the result block shows the overall status with its tone (a
  partial success looks different from a failure), then one card per form in the fixed order
  Doh-KDVP, Doh-Div, Doh-DHO, Doh-Obr, each with its own status: `generated` and `no_data` (with
  the reason when the backend gives one) are not shown as errors, `needs_input` gives the number of
  open questions, and `failed` is red. A tone is a class beside a text label, never color alone.
  An error that no longer blocks (its question was answered) is shown, and counted, as a warning.
  A card also lists the failure choice (`failureResolution`), the investments and brokers left out
  of that form (each with the text of its linked diagnostics), the unsafe developer-export marker
  with its investments, and the number of income rows whose foreign tax needs proof in eDavki.
  A diagnostic goes in the card of the one report whose `diagnosticIds` lists it; one that two or
  more reports list, or none, goes once in "Messages for several reports" with the forms of its
  `affectedReports`. Diagnostics that differ only in their sources, tax date and ID are one entry
  with a count (never one with a question); the sources are behind a "Details" button, one per
  line, built only when it is opened. A source is the shown name of sent file N for `source-000N` (with `(file N)` on a
  repeated name), else the sanitized server name, else "file N", then the row and transaction ID.
  The classification exclusions (`excludedItems`) are their own list. The pure code is
  `run/diagnostics_view.ts` and `run/result_view.ts`.
- **Tests.** Vitest tests of the login, the client, the page hashes, the version call, the language, the theme, the message texts,
  the input screen's pure code (labels, years, list changes, the request, the answers), the profile calls and the state of the "Remember my details" box,
  and the result screen's pure code (source labels, placement, grouping, the cards); the
  browser functions (`location`, `history`, `fetch`, storage) are arguments, so they need no DOM.

### Local HTTP API

`RunRoutes` (`src/server/run_routes.hpp`) holds the one run of the server (an
`ApplicationRun`) and only converts HTTP and JSON to the service types and back; every check of
an answer, every recalculation and every status stays in the service. All routes are behind the
guard and the session cookie. The JSON readers and writers are in `taxbroker_api`
(`request_json`, `answer_json`, `taxpayer_json`, `report_files`); the multipart reader is
`src/server/multipart.hpp`.

| Route | Body | Success | Errors |
| --- | --- | --- | --- |
| `POST /api/run` | `multipart/form-data`: the part `request` (JSON) and one part per file | 200 and the result JSON (schema 2); a second upload restarts the run, keeps the answers and raises the revision | 400 `{"error":"<code>"}`; 413 `file_too_large` or `request_too_large`; 422 `{"inputFault":{...}}` |
| `GET /api/run` | none | 200 and the result JSON | 404 `no_run` |
| `POST /api/run/answers` | `{"runRevision":2,"answers":[{"requestId":"request-0001","answer":"above_limit"}]}` | 200 and the result JSON | 400; 404 `no_run`; 422 `{"rejections":[...]}` |
| `POST /api/run/decisions/delete` | `{"runRevision":3,"decisionIds":["decision-0001"]}` or `{"runRevision":3,"all":true}` | 200 and the result JSON | as above |
| `GET /api/run/reports/<report>/xml?revision=<n>` | none | 200, `application/xml; charset=utf-8`, `Content-Disposition: attachment; filename="Doh-KDVP_2024.xml"` | 400 no valid `revision`; 404 no run, unknown report or no XML; 409 another revision |
| `GET /api/run/support-report` | none | 200, `application/json; charset=utf-8`, `Content-Disposition: attachment; filename="taxbroker-support-2024.json"`; the support report (`diagnostics.md`, "Support report") | 404 `no_run` |

- `<report>` is the key of the result JSON: `capitalGains`, `dividends`, `bankInterest`,
  `otherInterest`. The file names are those of the command line (`reportXmlFileName`), with
  `_UNSAFE` for a developer export.
- The frontend shows one save button in each form card whose report has `downloadAvailable`. It
  asks for the revision of the shown result and saves the file under the server's name, or
  `<form>.xml` when the header is missing. While the shown result is out of date the button is
  disabled, because the file would belong to inputs the user no longer has. A 409 tells the user
  to create the reports again, and any other refusal shows the general error text. A card for
  `no_data`, `needs_input` or `failed` has no button. A note under the button says the file is not
  filed yet.
- The frontend answers the open questions that have a screen (the 30-day loss question, the split
  ratio and the quantity correction) with `POST /api/run/answers`, one answer for one question per
  call, with the revision of the shown result. A 200 replaces the shown result with the new one
  (a new revision; every question form starts fresh, because request IDs are numbered again for
  each result). A 422 shows each rejection, by its message key, beside the input it marks. A
  rejection that says the revision or the question is out of date (`stale_revision`,
  `unknown_request`, and for a delete `stale_revision_delete` and `unknown_decision`) makes the
  page load `GET /api/run` again and tell the user that the questions were refreshed; a 404
  `no_run` tells the user to create the reports again. The recorded answers are listed with a
  delete for each and for all (`POST /api/run/decisions/delete`). Answer and delete buttons are
  disabled while the shown result is out of date. The page never sends an answer by itself and
  preselects none.
- Developer mode in the browser: a "Developer mode" button at the very bottom of the page, under a dividing line and centered,
  opens a panel with a checkbox that is off at every start and never saved (not in
  the profile); the button reads "Developer mode: ON" in red while the mode is on, also when the
  panel is closed. Only when it is on does the
  request carry `developerMode: true`; then the capital failure question lists the third choice
  `developer_export` in red, last. Choosing it and pressing "Send answer" only asks for the
  confirmation. The page then shows one red confirmation screen for
  `developer_export_confirmation_required` (the warning, every `name (ISIN)`, the risk) with
  Confirm and Cancel. Confirm sends the three confirmations of the service one after the other,
  each with the new revision, as the command line does; it stops at the first answer that is not a
  result, or when the next step lists other ISINs, and shows what it has. Cancel sends `cancel`
  and the failure question comes back. A result with `unsafe: true` shows a red notice above its
  download button, and the button says "Save the UNSAFE Doh-KDVP file".
- Another method on a run path is 405 with `Allow`. A request that throws is the 500 of the HTTP
  layer (empty body) and leaves the run as it was.
- The `request` part: `reportingYear`, `filingType` (`original` or `self_report`), optional
  `developerMode`, the `taxpayer` object (the keys of the taxpayer file) and `files`, a list of
  `{"part":"file0","kind":"broker","broker":"trade_republic"}`, `"kind":"ecb_rates"` or
  `"kind":"reference"` (up to 32; more is `too_many_files`). An unknown key is `bad_request`.
  A taxpayer object that the taxpayer reader refuses (a tax number that is not eight digits, an
  empty text, a date that does not exist, ...) is 400 `{"error":"bad_taxpayer","field":"taxNumber"}`
  with the key it is about, so the form can mark its input; the field is a fixed key of the
  taxpayer file, never a value, and an unknown key gives no `field` (the name is the sender's
  text). A taxpayer that is not an object stays `bad_request`. The broker name is looked up in
  one table of `request_json.cpp`; no route branches on a broker. The file name of a part is
  reduced to its last path part.
- 400 codes: `bad_request`, `path_not_allowed` (a key named `path` anywhere, R10), `no_broker_file`
  (an input check for the frontend, not a tax rule), `missing_part`, `unreferenced_part`,
  `duplicate_part`, `too_many_files` (more than 32 files in the list, or more than 33 parts in the
  upload), `unsupported_broker`, `bad_taxpayer`, `bad_multipart`. Every refusal, also a 404, 405, 409 and 422,
  writes one warning to the log with its status and code (R13), so a bug report with the log line
  names the cause. Where one code has several causes, the line adds a fixed word in brackets:
  `bad_multipart (content_type|body|part_header)`, `bad_taxpayer (<taxpayer key>)`, `input_fault (rate_file|reference_file <message
  key>)` and `answer_rejected (<count>: <message key> x<n>, ...)`. A step that works writes an
  info line (`docs/logging.md`, "What the server logs").
- The type of an answer comes from the request: the route looks up each `requestId` in the current
  result and reads the answer by that request's type and question data (`parseAnswerJson`). A
  shape that does not fit gives 422 with `answer_rejected.answer_does_not_fit` at that index and
  nothing reaches the session. A request that is not open in the current revision gets a
  placeholder answer, so the session's own checks give `stale_revision` or `unknown_request`.
  Values inside a fitting shape (a ratio of 0, a correction above the open units) are checked by
  the session, with its own keys.
- A 422 body holds no English sentence: each entry is `{"index":0,"messageKey":"...",
  "messageParams":{...}}`. An input fault is `{"inputFault":{"kind":"ecb_rate_file"` or
  `"reference_file","messageKey":"...","messageParams":{...},"source":{"file":"rates.csv","row":3}}}`,
  with the file name only (R12).

#### The saved taxpayer profile

`ProfileRoutes` (`src/server/profile_routes.hpp`) and `ProfileStore` (`profile_store.hpp`) keep
the taxpayer details of the page in `profile.json` (see R14 for the folder). The file holds the
keys of the taxpayer file with `isResident`, in key order on one line, so it is also a valid
`--taxpayer` file for the command line. The saved text is read leniently by
`readTaxpayerProfile`: each key is checked by the same function as a typed one, a bad value is
left out and named, an unknown key is ignored with one flag, and a file over 64 KiB, not JSON or
not an object is unreadable.

| Route | Body | Success | Errors |
| --- | --- | --- | --- |
| `GET /api/profile` | none | 200 `{"location":"~/.local/share/taxbroker/profile.json","saved":false}`, or with a file `{"ignoredKeys":false,"invalidFields":["birthDate"],"location":"...","saved":true,"taxpayer":{...},"unreadable":false}` (no `taxpayer` when unreadable; no `location` when the file is not under the home folder) | none |
| `POST /api/profile` | the taxpayer object (the same reader as the run, at most 32 KiB) | 204, the file is written | 400 `bad_taxpayer` with `field`; 413 `request_too_large`; 500 `profile_not_saved` |
| `POST /api/profile/delete` | none | 200 `{"location":...,"saved":false}`, also when there was no file | 500 `profile_not_deleted` |
| `GET /api/log-level` | none | 200 `{"level":"error"}`, the level of the log now (`trace` shows as `debug`, `critical` and `off` as `error`) | none |
| `POST /api/log-level` | `{"level":"error|warn|info|debug"}`, exactly one key, at most 1 KiB | 200 `{"level":"<new level>"}`; the level lasts until the server stops | 400 `bad_log_level`; 413 `request_too_large` |
| `GET /api/log` | none | 200 the log as text, `taxbroker-log.txt` (attachment): the older file `taxbroker.1.log` and then `taxbroker.log`, each up to its last 5 MiB | 404 `no_log_file` (no file sink, or no text); 405 with `Allow: GET` |

- Another method is 405 with `Allow`; another path below `/api/profile/` is 404.
- The server writes the profile only on `POST /api/profile`, which the page sends only when the
  user ticked the box (R10). A save replaces the file by a rename, so a crash leaves the old
  file or the new one.
- No response and no log line holds a path of the machine (R12, R13).

## Official rate files

`loadEcbRateFiles` (`include/taxbroker/official_rates.hpp`, `src/parsers/ecb_rate_file.cpp`) reads
the ECB Data Portal `csvdata` files described in [`calculations.md`](calculations.md), "Official
ECB rate files". Rate files are a separate input next to the broker exports, one file per calendar
year. The result is an `OfficialRateTable` and a list of `ParseDiagnostic` errors.

The program also contains the rates of the years 2016 to 2025 as compact files
(`resources/ecb/rates_<year>.csv`, columns `currency,date,rate`, embedded by `src/CMakeLists.txt`
into `builtInEcbRateFiles()`). `parseBuiltInRates` reads one of them; a fault there throws
`std::logic_error`, as it is a build error. `loadOfficialRates` returns the supplied files and,
for each year that no accepted supplied file covers, the built-in file of that year. The built-in
years (`BuiltInRateYear`) are made once per process and shared read-only (`mBuiltInYears`). Making
one only scans its file for the year, the first and last date and the rate count; the rates are
parsed on the first lookup in that year, so a run parses only the years it needs. `mRates` holds
only the rates of the supplied files; `find` and `publicationDays()` look in both. Every
`OfficialRate` and `OfficialRateFile` has an origin (`BuiltIn` or `Supplied`), and a supplied file
that replaced a built-in year has `mReplacesBuiltIn`.

- The table is found by exact currency code and date (`find`); there is no nearby-date lookup.
- `selectOfficialRates` (`src/processors/official_rate_selection.hpp`) takes the rate needs
  of a run (currency, tax date, report, sources) and the table. It returns one rate per currency
  and tax date, takes the last earlier rate (at most 4 days back, no warning), and gives one
  blocking `missing_exchange_rate` error per pair it cannot fill, also kept per pair in
  `mMissing`. The conversion stage (`convertCapitalTrades`, `src/processors/trade_conversion.hpp`) calls it for
  the foreign trades of the capital pools and gives the missing-pair errors to the gates.
- Each `OfficialRate` keeps its value (foreign units per 1 EUR, eight decimals), observation date,
  series key, origin, display name and logical row.
- A file is accepted whole or rejected whole. A rejected file adds no rate, and the other files
  still load. Two files for the same year that both pass the checks are both rejected. The caller
  decides whether any rate file error stops the run.
- Domain code does not read the files; a caller passes the paths. We will use CLI options and add file picker functionality. The processor's use of the table, the missing-rate request and the broker
  fallback are also in plan to do it.
- `scripts/dump_ecb_rates.sh <file...>` loads files with the same code and writes a summary of
  names, years and counts to `runtime/debug/`, for checking a real download. With
  `--compact FOLDER` it writes instead the compact `rates_<year>.csv` files that are built in.

The reference files (`include/taxbroker/reference_catalogue.hpp`) work the same way:
`loadReferenceData` takes the `--reference-files` and returns the catalogue, the origin of each
of the three files, or the first fault of a file as a message key with its parameters (file name,
logical row, no value of the file). `docs/reference_data.md` describes the files.
The built-in lists (payers, relief statements, broker payers) are parsed once per process and
shared read-only: a `ReferenceCatalogue` holds each list as a `shared_ptr`, so a load parses only
the files the user passes in and a copy of the catalogue copies no entry.

CSV files that are not broker exports (the supplied ECB rate files, the reference files and the
diagnostic message table; the built-in compact rate files have their own reader) are read by
`readCsvText` (`src/utils/csv_text_reader.hpp`). It checks an unclosed quote, a missing or repeated required column, a line break inside a field and the field count of each row,
and numbers the rows logically (header = 1). It returns the rows before the first fault and that
fault; each caller maps the fault to its own message. The Trade Republic parser has its own
reading.

## Parser diagnostics

Parser diagnostics have three separate responsibilities:

1. `include/taxbroker/diagnostics.hpp` defines the in-memory domain model. A `ParseResult` owns its
   diagnostics, so parsing separate CSV files concurrently does not require shared mutable state.
   Each diagnostic carries a message key and named parameters for translation, and the English
   sentence for logs and developer tools (see "Message keys and translation" in
   [`diagnostics.md`](diagnostics.md)).
2. `include/taxbroker/api/diagnostics_json.hpp` and its implementation under `src/server/api/`
   convert that model into the versioned frontend contract. The parsing and tax-processing core
   does not depend on a JSON library.
3. The optional `taxbroker_tr_dump` developer tool writes a JSON artifact. Production parsing does
   not write diagnostics to disk; the future local API should return them directly in its response.

Application logs remain useful for developers and operational troubleshooting, but the frontend
must never parse log messages.

### JSON contract

```json
{
  "schemaVersion": 1,
  "broker": "trade_republic",
  "status": "completed_with_errors",
  "summary": {
    "warningCount": 1,
    "errorCount": 1,
    "infoCount": 0,
    "totalCount": 2
  },
  "diagnostics": [
    {
      "severity": "warning",
      "code": "unsupported_asset_class",
      "message": "Asset class 'PRIVATE_FUND' was preserved, but its tax treatment is not supported yet.",
      "messageKey": "unsupported_asset_class.tax_unsupported",
      "messageParams": {
        "assetClass": "PRIVATE_FUND"
      },
      "source": {
        "file": "TransactionExport.csv",
        "row": 275
      },
      "transactionId": "example-transaction-id",
      "field": "asset_class"
    }
  ]
}
```

The `status` value is derived from diagnostics:

- `success`: no diagnostics;
- `completed_with_warnings`: one or more warnings and no errors;
- `completed_with_errors`: at least one error.

Warnings describe preserved data that needs attention later. Errors describe rejected rows or a
file-level parsing failure. Optional location fields are omitted when unavailable rather than sent
as `null`.

`message` is the English sentence for logs and developer tools. The frontend builds its text from
`messageKey` and `messageParams`; it never parses `message`. The texts come from
`GET /api/messages` ("Frontend").

The strings used by `severity`, `code`, `messageKey`, `broker`, and `status` are API values for the
frontend. No JSON contract is frozen before the beta: `schemaVersion` stays 1, and a field may
still be renamed or removed. From the first released frontend on, these values are not renamed
within a schema version. Add a new code or key for new behavior, and increment `schemaVersion` for
a breaking contract change. This versioning protects the current backend and future frontend
contract; it does not cover the legacy project.

For privacy, the response exposes only the uploaded filename, never an absolute host path. It also
does not duplicate raw financial field values. The CSV row number, transaction ID, field name,
message key with its parameters, and an English message provide enough context for the local UI.
The separate application result defined in [`diagnostics.md`](diagnostics.md) copies the key and
parameters of parser diagnostics.

### Local debug artifacts

Running `scripts/dump_tr_parse.sh` produces:

- `runtime/debug/tr_parsed_debug.txt` for the parsed C++ data;
- `runtime/diagnostics/tr_parse_diagnostics.json` for the frontend-shaped diagnostics.

The tool shows what the parser retains from a local export, including parsed financial data and
complete event metadata. Only the source filename is stored, never its path. The `runtime/`
directory is ignored by Git; its potentially private artifacts must remain local.

Running `scripts/dump_tr_kdvp.sh` writes the Doh-KDVP XML of the selected year to
`runtime/debug/tr_kdvp_<year>.xml`; with `--synthetic` it reads the synthetic
`tests/test_data/csv/kdvp_dump_fixture.csv` instead of the real export and writes
`runtime/debug/tr_kdvp_synthetic_<year>.xml`. Blocked ISINs are left out, so with a blocked ISIN
the file is not a complete return; the tool says so.

Running `scripts/report.sh` runs `taxbroker report` in Docker and writes to
`runtime/report/<year>/`; with `--synthetic` it reads `tests/test_data/cli/report_fixture.csv`,
`report_income_fixture.csv` (2024 dividends, bank interest and bond coupons), the made-up USD rates
`report_ecb_rates.csv` and `tests/test_data/cli/taxpayer.json`, so all four forms are written. A real taxpayer file stays below `runtime/`. The script takes
the same file options (`--trade-republic`, `--ecb-rates`, `--reference-files`) with the same
list rule.

Parser logs may later be shared for support, so they follow a stricter boundary: source index, CSV
row, and diagnostic reason are allowed; paths, transaction IDs, and raw financial values are not.
