# Conversion, rounding, and fees

## What this document explains

This document defines how the app converts foreign currencies, rounds numbers, shows broker fees,
and rejects values that cannot be represented safely. These rules apply to capital gains,
dividends, interest, and the final FURS XML files.

The selected-year rules are in [`tax_rules.md`](tax_rules.md). FIFO and corporate actions are in
[`fifo.md`](fifo.md). Application error scope and independent XML results are in
[`diagnostics.md`](diagnostics.md).

## Fixed-point values

Tax calculations must not use binary floating-point numbers.

| Value | Stored precision | Smallest stored step |
| --- | --- | --- |
| `Money` | 4 decimal places | `0.0001` |
| `UnitPrice` | 8 decimal places | `0.00000001` |
| `Units` | 8 decimal places | `0.00000001` |
| `ExchangeRate` | 8 decimal places | `0.00000001` |
| `CorpRatio` | 8 decimal places when stored | `0.00000001` |

An exact corporate-action fraction is kept as two whole numbers while it is applied. It is not
first rounded to `CorpRatio` precision.

Every multiplication or division uses a checked wide-integer intermediate. The processor rounds
only at a boundary defined below. It must not round an operand early to make an operation easier.

The shared C++ arithmetic primitives use Boost.Multiprecision checked 256-bit integers and reduced
exact fractions. This width is an integer range, not a decimal calculation precision. Factors are
cancelled before products; a fraction or intermediate that still exceeds the supported range
returns `arithmetic_overflow`. Results are also checked against the destination `int64_t` range
and its sign/positive-value policy. A failed operation does not mutate its input state.

### Stored precision is not calculation precision

The scales in the table describe stored values, not a smaller workspace used for calculations. A
calculation keeps the complete checked integer product or the exact quotient and remainder until
the required result scale is known. For example, multiplying eight-decimal `UnitPrice` by
eight-decimal `Units` keeps the full product of both integers, which carries 16 effective decimal
places before it is divided and rounded.

Do not round a calculation to an intermediate number of decimal places as a safety margin. Two
extra places do not prevent double rounding. For example, when the final result needs six decimal
places:

```text
exact value                         1.234500499
rounded directly to 6 places       1.234500
rounded first to 8 places          1.23450050
then rounded from 8 to 6 places    1.234501  (wrong)
```

The processor therefore rounds directly from the exact wide-integer result to the destination
scale. It does not first round to 8, 10, or any other fixed calculation scale.

Eight-decimal `Units` are ledger facts, not guard digits. Doh-KDVP
preserves all significant unit digits, up to the eight places accepted by FURS.

## Rounding rule

When the discarded magnitude is exactly half of one smallest destination step, increase the kept
magnitude by one step. Do the same when it is above half; leave the kept magnitude unchanged when
it is below half. This is rounding halves away from zero.

Examples at the four-decimal `Money` boundary:

| Input | Stored value |
| --- | --- |
| `1.23454` | `1.2345` |
| `1.23455` | `1.2346` |
| `-1.23454` | `-1.2345` |
| `-1.23455` | `-1.2346` |

Negative values are allowed only in domain fields that are explicitly signed, such as an internal
gain or loss. An exchange rate, fee paid, quantity, reportable unit price, income amount, or tax
paid must not become negative.

A required positive source value that rounds to zero is unrepresentable. For example, a required
money amount of `0.00004` must produce an error instead of silently becoming zero.

### Import boundaries

Source money is rounded once to four decimals when it enters `Money`. A source unit price is
rounded once to eight decimals when it enters `UnitPrice` (see "Unit prices" below). Source units
are rounded once to eight decimals when they enter `Units`, but the original text and discarded
digits remain available for the controlled FIFO reconciliation defined in `fifo.md`. A broker rate
is rounded once to eight decimals only when its documented source format permits extra precision. A
user-entered official rate must have no more than eight decimals and is not rounded for the user.

`importFixedPoint` retains the exact source sign, canonical decimal value and discarded digits.
Trade and action `UnitSourceEvidence` entries additionally retain the original text and its source
reference. Trade-side normalization changes stored units, not the signed source evidence. True
zero remains valid where the field permits it; a nonzero fee, tax or required amount that rounds
to zero is rejected rather than silently lost. Parser diagnostics keep their existing codes.

Unit examples:

| Input | Stored `Units` |
| --- | --- |
| `0.123456784` | `0.12345678` |
| `0.123456785` | `0.12345679` |
| `-0.123456785` | `-0.12345679` before any trade-side normalization |

Broker numbers use one strict format: an optional sign, digits, and an optional `.` with digits
(`-1234.5`, `0.125`). Digit grouping, a decimal comma and accounting parentheses are never
accepted, because `0,125` would otherwise be read as 125 instead of 0.125. Such a value is an
`invalid_value` error on its field and the row is skipped. There is no lenient import mode.

| Input | Result |
| --- | --- |
| `0.125`, `-1234.5`, `1000.50` | accepted |
| `0,125`, `1,234.5`, `1.000,50`, `(5)` | `invalid_value` |

Once imported, money and units keep their stored precision until a later operation explicitly
defines another boundary. Formatting a value for display must not change the stored value.

### Unit prices

A trade unit price is stored as `UnitPrice` with eight decimal places. `Money` keeps four decimal
places for amounts, fees, tax and income. The reasons:

- The Doh-KDVP schema accepts up to eight decimals for `F4` and `F9` (`typeDecimalPos14_8`, at
  most 14 integer digits). The official display transform shows at least four decimals and keeps
  up to eight.
- Section 11 of the Doh-KDVP instructions defines `F4` as the purchase value per unit that comes
  from the contract, and `F9` as the sale value per unit that comes from the contract or other
  evidence:
  *nabavna vrednost ob pridobitvi (na enoto), pridobljena v posameznem poslu, razvidna v prodajni
  ali drugi pogodbi*, and
  *vrednost odsvojenih vrednostnih papirjev na enoto, razvidna iz prodajne ali druge pogodbe
  oziroma iz drugih dokazil*.
  The instructions give no rounding rule for these two fields.
- The import rounding enters the lot basis and so the gain, not only the displayed value. A price
  of `10.00005` stored at four decimals is `10.0001`. Across 1,000,000 units the basis is then
  EUR 10,000,100 instead of EUR 10,000,050, which is EUR 50 too high.
- The legacy application wrote `F4` and `F9` with eight decimals, and eDavki accepted that output.
- Trade Republic writes prices with up to ten decimals.

Rules:

- A source price is rounded once, halves away from zero, to eight decimals. Later digits are not
  kept. This changes a unit price by at most EUR 0.000000005, and FURS cannot accept more digits.
  A positive price that rounds to zero is rejected.
- A source price is a fact from the contract, so no extra digits are carried. Calculated values
  (lot basis, partial-sale allocations, adjusted unit values) stay exact until their output
  boundary.
- Known limit: a foreign price is rounded to eight decimals at import and again after its
  conversion to EUR. Compared with converting the exact source price, the result differs by at
  most one step (EUR 0.00000001) per unit when the rate is at least 0.5 foreign units per EUR,
  which is EUR 0.01 on 1,000,000 units. A stronger currency can differ by more: at 0.33 KWD per
  EUR, `1.0000000049` KWD becomes `3.03030303` EUR instead of `3.03030305` EUR, two steps.
  Whether the conversion should start from the exact source text is decided together with
  non-EUR FIFO support.

## Official exchange-rate direction

Banka Slovenije publishes exchange rates as the number of foreign-currency units for `1 EUR`.
The app uses the same direction:

```text
1 EUR = rate in the foreign currency
EUR amount = foreign amount / rate
```

For example, when `1 EUR = 1.25000000 USD`:

```text
10.0000 USD / 1.25000000 = 8.0000 EUR
```

EUR values use the exact identity rate `1.00000000` and never require user input. Every other rate
must be positive.

The official rate date is:

- the acquisition date for a purchase unit price;
- the disposal date for a sale unit price;
- the income date for a dividend or interest amount and its foreign tax.

The user copies the rate that Banka Slovenije shows as valid for that currency and date. This also
covers a date for which the site uses its applicable daily or monthly rate. The app must not
interpolate rates or silently choose an average, annual, current, or nearby rate.

## Official ECB rate files

The app ships no rate data. The user loads one file per calendar year, so new rates need no new
app version, and every conversion stays offline and reproducible. The source is the ECB reference
rate table, which *Banka Slovenije* republishes as its daily rates.

Evidence that the two tables are the same (checked 2026-11-01, public data only):

- The English *Banka Slovenije* page on exchange rates (heading "Banka Slovenije's exchange rates -
  ECB reference rates") names the table "ECB reference rates", ends with
  "Source: European Central Bank", and writes the direction as `EUR 1 = USD 1.2010`.
- Its 2025 table (`dtecbs-l.xml`) and the ECB 2025 file have the same 7,650 `(date, currency)`
  pairs. 7,649 values are equal. The one difference is NOK on 2025-10-23: the ECB file has
  `11.5829`, which fits its neighbours (`11.643` and `11.6185`), and the *Banka Slovenije* table
  has `11.8529`, which looks like a typing error. The ECB file is therefore the accepted source,
  and a difference from the *Banka Slovenije* page is not a reason to reject a file.

**Accepted file.** Exactly one format: the unmodified ECB Data Portal `csvdata` export of the
`EXR` dataset for one calendar year, for example

```text
https://data-api.ecb.europa.eu/service/data/EXR/D..EUR.SP00.A?startPeriod=2025-01-01&endPeriod=2025-12-31&format=csvdata
```

It has one row per date and currency, and every row states its own key. The wide
`eurofxref-hist.csv` is not accepted. The `detail=dataonly` download drops the `OBS_STATUS` column
and is rejected for that reason. Other columns, and the column order, do not matter: the columns
are read by header name. Quoted fields with commas are handled (`TITLE_COMPL` has them).

**Checks.** The whole file is rejected, with a diagnostic that names the file by its display name
and, where it applies, the row and the column, at the first of these faults:

| Fault | Rule |
| --- | --- |
| a required column is missing | `KEY`, `FREQ`, `CURRENCY`, `CURRENCY_DENOM`, `EXR_TYPE`, `EXR_SUFFIX`, `TIME_PERIOD`, `OBS_VALUE`, `OBS_STATUS` |
| a required column named twice in the header | rejected, because it is unclear which one holds the value |
| a record with a wrong field count | same rule as for the broker files |
| `FREQ`, `CURRENCY_DENOM`, `EXR_TYPE`, `EXR_SUFFIX` | must be `D`, `EUR`, `SP00`, `A` in every row: `1 EUR = OBS_VALUE CURRENCY`, the direction above |
| `CURRENCY` | three capital letters, not `EUR` |
| `KEY` | must be `EXR.D.<CURRENCY>.EUR.SP00.A` |
| `TIME_PERIOD` | a real calendar date `YYYY-MM-DD` |
| `OBS_STATUS` | only `A`, the ECB code for a normal value |
| `OBS_VALUE` | not empty, positive, at most eight decimals, strict number format |
| the same `(date, currency)` twice | rejected |
| dates from more than one calendar year | rejected |
| no rows | rejected, because the year is unknown |
| an unclosed quote, or a line break inside a field | rejected, because records would be merged (a quote that closes on a later line merges the lines into one record, even with the right field count) |
| the file cannot be opened or read, or a row is longer than the CSV reader allows (20 MB) | rejected; the other files still load |

`OBS_STATUS` has other codes in the ECB code list `CL_OBS_STATUS` (for example `E` estimated, `P`
provisional, `M` and `L` missing). Every row of the 2022 and 2025 downloads is `A`, and a suspended
currency does not show as a status: the series just stops (RUB after 2022-03-01). Until a real
file needs another code, any other value rejects the file. The rate keeps all eight decimals; the
file has at most five.

**Several files.** The user can load the tax year and older purchase years, one file per year. Two
files for the same year that both pass the checks, even byte-identical ones, are an error, and both
are rejected. A file rejected for its own fault does not count, so a good file of the same year
beside it still loads. A file of another year is not affected.

**Lookup.** A rate is found only by its exact currency code and date. The ECB publishes no rate on
weekends and TARGET holidays, and a suspended or unpublished currency has no rows, so those dates
and currencies simply have no rate. The app never takes a nearby date and never carries a rate
forward. What happens to an event without a rate (typed rate, or the broker fallback below) is
decided by the exchange-rate request, what will be implemented latter.

**Provenance.** Each rate keeps the value, the observation date, the ECB series key, the SHA-256 of
the file bytes, the file's display name and the row. A file rate is an official rate and is labelled
so; a broker rate never is. The checksum is the checksum of the file as the user loaded it. The
maintainer publishes the unmodified download of each year as a release asset with its SHA-256, the
query URL and the download date, and anyone can download the same query from the ECB and compare:
three downloads of the 2025 query, about 30 minutes apart, were byte-identical. The app does not
require the published copy.

**Attribution.** The ECB allows free reuse of its statistics "on the condition that the source is
quoted". Any report or screen that shows these rates says "Source: ECB statistics." See the ECB
statistics reuse policy (`ecb.europa.eu`, "Usage policy of ECB statistics").

Any download is explicit and opt-in. The app works with no network access.

## Asking the user for official rates

Importing foreign trades, dividends or interest does not require a broker exchange rate. Retain the
original payment amount, currency, tax date and provenance when that optional value is absent;
the event then has no broker fallback. A supplied invalid broker rate is a separate sourced
parser error under the income policy in [`architecture.md`](architecture.md). It is never
silently treated as absent or replaced with a fabricated rate.

A rate from a loaded ECB file for the exact currency and date is used by default, with no prompt
(see "Official ECB rate files"). Only a currency and date without such a rate is requested.

After merging and deduplicating the input, the processor collects every foreign currency and date
needed for a FURS report value. Older acquisition dates are included when those acquisitions are
matched to a sale in the selected year. A currency and date needed only for an informational fee
is not included and must never produce a rate prompt.

The application result returns one structured `exchange_rates_required` request containing the
missing `(currency, tax date)` pairs. The frontend must not discover them from logs.

For each pair, the default prompt:

- links to the Banka Slovenije exchange-rate page;
- shows the date and currency;
- labels the field in the form `1 EUR = [rate] [currency]`;
- accepts at most eight decimal places;
- explains which report values need the rate; and
- shows a converted example before confirmation.

An official rate entered for one instrument is reused for every taxable instrument, broker,
account, income item, and tax amount with the same currency and tax date. The user is not asked
for that pair again during the same report run.

A rate must never be reused for another currency or date. Run-local reuse does not silently save a
rate for a later report run. The processing result records the rate, its currency and date, and
that the user entered it as an official rate.

## Optional broker-rate fallback

The user may choose a broker-provided exchange rate instead of entering the official rate. This is
not the default.

Before accepting it, the frontend must show this prominent warning:

> Warning: This is probably not the official exchange rate required by FURS. Using it may make
> your tax XML incorrect. We strongly recommend entering the official Banka Slovenije rate.

The user must explicitly choose **Use broker rate anyway** after seeing the warning. A broker rate
must not be selected by a pre-checked control, a saved preference, or the Enter key on the official
rate field.

The fallback is available only when the broker parser knows the rate's direction and the rate is
positive. Each broker rate is stored with its direction and a verification state, and
`convertBrokerMoney` and `convertBrokerUnitPrice` read the direction from the stored rate; an
unverified rate is refused. Trade Republic `fx_rate` means EUR per 1 foreign unit: verified from
export samples for income, and by the maintainer's decision for trades (there is no foreign trade
sample). Only a row with a foreign `original_currency` settled in EUR, and the identity rate of a
plain EUR row, are verified; any other supplied rate is kept as unverified. The frontend shows the
direction and a conversion example. An unexplained decimal must not be guessed to mean either
foreign currency per EUR or EUR per foreign currency.

A broker rate applies only to its source event. It is not reused for another instrument or event,
even on the same date, because broker conversion rates may differ by transaction. If the user
later enters an official rate for that currency and date, the official rate replaces every broker
fallback for the pair and all affected results are recalculated.

Every generated result that used a broker rate keeps a visible `broker_exchange_rate_used`
warning. The warning identifies the broker, source event, currency, and date, and remains visible
on the download screen. Acknowledging the warning does not rename the broker rate as official.

This warning and confirmation flow applies to FURS report values. Informational fee conversion
uses the separate rules below and never asks the user to choose a rate.

## Exact conversion order

An official rate is stored as an eight-decimal `ExchangeRate`. To convert a four-decimal foreign
`Money` value, calculate:

```text
eurMoney = roundHalfAwayFromZero(
    foreignMoney * EXCHANGE_RATE_SCALE / officialRate
)
```

The multiplication uses a wide integer. Only the final quotient is rounded to `Money` precision.
A foreign `UnitPrice` converts the same way and its quotient is rounded once to eight decimals, so
`10.00000000 USD` at `1 EUR = 1.08450000 USD` is `9.22083910 EUR`.

When a verified broker field instead means EUR per one foreign-currency unit, the processor uses
its documented multiplication direction directly. It must not invert and round the rate before
converting the amount.

For example, a confirmed broker rate of `0.90000000 EUR per USD` converts `10.0000 USD` to
`9.0000 EUR`. This result remains marked as using a broker rate.

Each value is converted separately:

- purchase and sale unit prices;
- dividend or interest gross income;
- foreign tax.

Do not convert a grand total and divide it back across transactions. Do not use a broker's EUR
settlement amount as the FURS value when the reportable source amount is in a foreign currency.

### Conversion boundary examples

With `1 EUR = 1.25000000 USD`:

```text
123.4567 USD / 1.25000000 = 98.76536 EUR -> 98.7654 EUR
```

At an exact half step:

```text
 0.0001 / 2.00000000 =  0.00005 ->  0.0001
-0.0001 / 2.00000000 = -0.00005 -> -0.0001
```

The negative example proves symmetric arithmetic. A field that does not allow negative money is
still rejected by its domain validation.

## Exact FIFO lot basis and money totals

Convert an original acquisition unit price on its acquisition date to eight-decimal EUR `UnitPrice`
using the conversion rules above. Multiply that price by the original eight-decimal quantity and
retain the complete wide-integer product as the lot basis, with 16 effective decimal places. This
is an exact product of imported facts, not an extra rounded calculation scale. A split does not
reconvert that acquisition at its action date and does not replace its basis with a rounded total.

For a partial sale of `matchedUnits` from `remainingUnits`, allocate
`remainingBasis * matchedUnits / remainingUnits` exactly as a checked rational. Subtract that exact
allocation from the remaining basis. The final consumption receives the entire remaining basis.
Reduce fractions to avoid needless intermediate growth; checked range failures retain their normal
diagnostic scope. Do not feed rounded XML unit values or rounded informational totals back into a lot.

The reusable `LotBasisState` stores remaining units and an exact rational number of `10^-16` EUR
basis ticks. Its consumer owns purchase date, sources and FIFO order. `consumeLotBasis` returns
the allocated basis and next state together; `adjustLotUnits` changes only units. Neither helper
applies an action, selects an exchange rate or performs chronological FIFO matching.

A FIFO match (an internal link between one sale and the units it takes from one purchase) keeps
three exact values in the same `10^-16` EUR ticks: the allocated basis, the sale value (the sale
`UnitPrice` times the matched `Units`, an exact product) and the gain (sale value minus basis,
negative for a loss-making portion). Fees are not included. Nothing is rounded to `Money` or XML
precision at this stage, and rounded values never go back into a lot.

When several exact allocations must be emitted as four-decimal money parts of one known total,
round the combined total once and allocate its smallest-unit remainder by largest fractional
remainder, breaking ties by FIFO lot and then sale order. Those emitted parts must add to the
emitted total; their rounded values do not replace the exact internal allocations.

### Foreign acquisition, split and partial disposals

Buy `0.14760000` units at `255.3750 USD` per unit on a date with `1 EUR = 1.25000000 USD`:

```text
converted original unit price       204.30000000 EUR
exact lot basis                     30.1546800000000000 EUR
informational Money total           30.1547 EUR
2 / 1 split quantity                0.29520000
adjusted value per unit              102.1500 EUR
first sale of 0.10000000: basis      10.2150000000000000 EUR
remaining 0.19520000: basis          19.9396800000000000 EUR
emitted basis totals                 10.2150 + 19.9397 = 30.1547 EUR
```

Both disposals retain the original acquisition date. The original exact basis, rather than
`30.1547`, produces the adjusted unit price. A new sale uses its own disposal-date exchange rate.
The amount of cash settled by the broker is not substituted for this tax basis.

### Money totals

Multiplying a unit price by units to produce a `Money` total uses:

```text
totalMoney = roundHalfAwayFromZero(
    unitPrice * units * MONEY_SCALE / (UNIT_PRICE_SCALE * UNITS_SCALE)
)
```

For example:

```text
204.30000000 * 0.14760000 = 30.15468 -> 30.1547
```

Adding and subtracting values already stored at the same scale is exact and must use checked
integer arithmetic. A sum is not rounded again.

FIFO comparison and inventory changes use exact eight-decimal units. A rounded XML value must
never be fed back into the ledger.

### Totals after rounding

The app must not add a hidden balancing amount or change a trade merely to make displayed numbers
add up.

- Buy and sale money totals are not expected to be equal. Their difference may represent a gain
  or loss.
- When an XML document contains detail rows and a total, round each detail row first and calculate
  the total by adding the emitted row values. Do not separately round an unrounded grand total.
- A corporate-action lot keeps its preserved exact total purchase value. Multiplying its rounded
  XML value per unit by its quantity may produce a small reconstruction difference. Do not change
  the quantity or unit value to hide it, and never feed that reconstructed amount back into FIFO.
- When the processor divides one known exact total into several rounded parts, the parts must add
  back to that total. Assign the smallest-unit remainder by the documented deterministic
  allocation rule. Do not use this rule to reconcile independent source transactions.

If a broker supplies both detail rows and a control total that its documented format says must
match, compare them before XML output at their common stored scale. A documented source-rounding
difference may use the bounded reconciliation rules in `fifo.md`. An unexplained difference returns
a source-correction request or error; the processor must not guess which value is correct. Neither a
net settlement amount nor a fee-inclusive cash amount is a gross tax-value control total.

The Trade Republic parser compares a trade's `amount` with its unit price times units and warns
(`trade_amount_mismatch`) above EUR 0.01. This only makes a corrupted file visible: the amount is
not a control total for any FURS value, and nothing is repaired or reconciled.

An ordinary XML reconstruction difference is not itself missing history. Preserve the exact lot
basis and the correctly rounded XML unit value, and show `xml_basis_rounding_difference` when the
reconstructed value differs at `Money` precision. Include the difference without changing a price
or inventing a balancing event. This output-rounding notice is distinct from the EUR 0.01 budget
for corrections to inventory quantities. Repeated notices do not authorize source corrections.

## FURS XML boundaries

The XML writer receives validated report values. It does not repeat tax calculations or repair an
invalid value.

| XML value | Output precision | Rule |
| --- | --- | --- |
| Doh-KDVP quantities and stock (`F3`, `F7`, `F8`) | 4 to 8 decimals | Write exact `Units`; remove trailing zeros only down to four decimals. |
| Ordinary Doh-KDVP unit values (`F4`, `F9`) | 4 to 8 decimals | Write `UnitPrice` without changing its value; remove trailing zeros only down to four decimals. |
| Corporate-action-adjusted Doh-KDVP unit values (`F4`) | 4 to 8 decimals | Exact lot basis over the units after the action, rounded once to eight decimals, halves away from zero (`fifo.md`); remove trailing zeros only down to four decimals. |
| Dividend value and foreign tax | 2 decimals | Round `Money` once at XML output. |
| Doh-DHO and Doh-Obr income and foreign tax | 2 decimals | Round `Money` once at the documented form-row boundary. |
| Doh-DHO and Doh-Obr totals | 2 decimals | Sum the already emitted two-decimal row values. |

The current official Doh-KDVP schema accepts up to eight decimal places for quantities, stock, and
unit values. The official FURS display transform shows these fields with at least four decimal
places and preserves up to eight. Eight places are a limit, not a requirement to add zeros.

For example:

| Stored value | XML value |
| --- | --- |
| quantity `2.00000000` | `2.0000` |
| quantity `0.12345678` | `0.12345678` |
| ordinary unit value `12.34565000` | `12.34565` |
| ordinary unit value `10.00000000` | `10.0000` |
| adjusted unit value `12.34560000` | `12.3456` |
| adjusted unit value `12.34567891` | `12.34567891` |

Doh-KDVP unit values must not be rounded to two decimals. A per-unit value can need fractions of a
cent, and FURS explicitly accepts and displays that precision. Dividend or interest `12.3450` is
instead written as `12.35` because those XML fields use two decimals.

If two emitted interest rows each contain `0.0050`, each row is written as `0.01` and their XML
total is `0.02`. Summing `0.0100` first and emitting a total of `0.01` would make the total disagree with
the rows and is not allowed.

Grouping follows each form's instructions: Doh-DHO groups qualifying income by the required payer,
interest type and source-country fields; sum stored values exactly before rounding each grouped
row. Doh-Obr preserves the payment detail required by its form. Bank-interest threshold comparisons
use the full four-decimal selected-year qualifying total before XML rounding. An ordinary-interest
amount never enters that threshold. Tests must distinguish payment totals from emitted form rows.

XML decimals use `.` as the decimal separator, contain no digit grouping or exponent, and use the
documented number of decimal places.

## Broker fees

Broker fees remain part of the imported event. A fee is stored as a positive `Money` value with
its original currency, broker, event date, and source reference. An empty fee field means zero.
The app must not infer a fee from a difference between unit price, units, transaction amount, or
cash movement.

Fees do not change:

- the acquisition unit value sent to FURS;
- the disposal unit value sent to FURS;
- FIFO matching;
- a dividend or interest amount; or
- any value in a FURS XML file.

FURS applies the statutory normalized deduction. The app must not also claim the actual broker fee
as a separate cost.

### Informational fee summary

For the selected reporting year, the frontend shows:

- a subtotal for each original fee currency under each broker;
- a four-decimal EUR equivalent for a broker when all of that broker's fees can be converted; and
- one four-decimal overall EUR equivalent when every broker's fees can be converted.

Only deduplicated events whose tax date is in the selected year contribute to this summary. Fees
from older events remain preserved in history but are not included in the selected-year totals.

Fee conversion never requests an official rate. A non-EUR fee uses a broker-provided rate only
when the parser knows its direction, the rate is positive, and the broker associates it with that
fee event. The calculation uses the documented broker direction and rounds once to four-decimal
`Money`.

An official rate entered for a FURS value is not reused for a fee. A broker rate from another
event is also not reused unless the broker's documented format explicitly says that it applies to
the fee. The fee summary identifies converted values as using a broker rate, but it does not show
the FURS broker-rate warning because fees are not written to a FURS XML file.

When a fee has no usable broker rate, the frontend keeps it in its original-currency subtotal and
does not ask the user for a rate. The affected broker's EUR total is shown as unavailable, not as
a partial total. The overall EUR total is also unavailable. Other brokers may still show their
complete EUR totals.

Complete broker totals and the complete overall total add converted four-decimal `Money` values
without further rounding.

Example:

| Broker | Fees | EUR result |
| --- | --- | --- |
| Broker A | `1.2500 EUR` and `2.5000 USD`; broker rate `1 EUR = 1.25000000 USD` | `3.2500 EUR` |
| Broker B | `0.7500 EUR` | `0.7500 EUR` |
| Overall | All fees above | `4.0000 EUR` |

If Broker A does not supply a usable rate, its summary instead shows `1.2500 EUR` and
`2.5000 USD`, with its EUR total and the overall EUR total marked unavailable. No rate prompt or
processing error is created.

The summary is informational and is never written to a FURS XML document.

## Invalid, missing, and overflowing values

The processor uses these diagnostic codes:

| Code | Meaning |
| --- | --- |
| `missing_exchange_rate` | The required official rate was not entered and no broker fallback was confirmed. |
| `invalid_exchange_rate` | A rate is zero, negative, malformed, too precise, out of range, or has an unknown or unverified direction. |
| `unrepresentable_value` | A required value cannot be represented at its destination scale, including a positive value that becomes zero. |
| `arithmetic_overflow` | A checked multiplication, division, addition, subtraction, or rounding result exceeds its integer range. |
| `xml_value_out_of_range` | A valid internal value does not fit the target FURS XML field. |

The app must never wrap, clamp, switch to floating point, invent a rate, emit scientific notation,
or silently omit an affected value.

Error scope is limited to the affected output:

| Failure | Effect |
| --- | --- |
| Capital value needed for one ISIN | Mark that ISIN unsafe and use the normal confirmed-exclusion flow. |
| Dividend value | Block the dividend XML, but keep valid capital and interest outputs. |
| Doh-Obr value safely isolated to one ISIN | Request exclusion of failed ISINs from Doh-Obr or failure of that form, as defined in `diagnostics.md`; keep unrelated reports available. |
| Other interest value | Block only the affected interest form pending its normal correction or failure resolution; keep unrelated outputs available. |
| Final XML field range | Block only the XML file containing that field. |

Developer mode must not bypass a missing rate, arithmetic overflow, or XML range error.

### Overflow and range examples

The largest positive `Money` value is `922337203685477.5807`. Parsing
`922337203685477.5808` produces an overflow error.

Converting `922337203685477.5807` with an official rate of `0.50000000` would double the value
beyond the `Money` range. It produces `arithmetic_overflow`; it is not clamped to the maximum.

The largest positive `UnitPrice` is `92233720368.54775807`. Parsing `92233720368.54775808` as a
price produces an overflow error.

A Doh-KDVP `F4` or `F9` value may contain at most 14 integer digits and 8 decimal places. The
largest `UnitPrice` has 11 integer digits, so an ordinary unit price always fits. An adjusted unit
value is calculated from the exact lot basis and can be larger. The row builder rounds it to a
`UnitPrice`; a value above `92233720368.54775807` does not fit and blocks the ISIN
(`corporate_action_unrepresentable`, key `.unit_value`), so the writer never sees it. A value
below `0.000000005` rounds to `0.00000000`, which is written as it is: the exact basis stays in
the app and the row differs by less than half a step per unit (not checked with eDavki; no real
case is expected).

## Required processor and XML tests

Tests based on this document must prove that:

- fixed-point parsing rounds positive and negative half values away from zero;
- unit inputs round once to eight decimals at the documented import boundary;
- unit prices round once to eight decimals at import, including the half step at the ninth
  decimal, and a price below the old four-decimal step is kept;
- a foreign unit price converts to EUR and rounds once to eight decimals;
- required positive values that become zero are rejected;
- EUR uses the identity rate without a prompt;
- official rates use the foreign-currency-per-EUR direction and division;
- acquisition, disposal, dividend, interest, and tax values use their documented dates;
- the frontend asks once per missing currency and date;
- an official rate is reused across taxable instruments, brokers, accounts, and event kinds for
  the same currency and date;
- an official rate is not reused for another currency, date, or report run;
- broker-rate fallback is not the default and requires the prominent explicit warning;
- an unknown broker-rate direction is rejected;
- a broker rate remains event-specific and leaves `broker_exchange_rate_used` in the result;
- entering an official rate replaces same-date broker fallbacks and recalculates their results;
- conversion and money-times-units examples produce the exact documented values;
- no arithmetic operation uses floating point or rounds before its documented boundary;
- direct rounding of `1.234500499` to six decimals produces `1.234500`, proving that an
  eight-decimal intermediate is not used;
- Doh-KDVP quantities and unit values preserve their documented precision and are written with
  four to eight decimal places;
- a quantity with non-zero seventh or eighth decimal digits is not shortened to six places;
- XML totals equal the sum of their emitted rounded rows;
- a rounded per-unit value is not changed to force its reconstructed amount to equal a preserved
  lot total;
- deterministic residual allocation is used only when several parts come from one known total;
- dividend and interest half values round to two decimals;
- interest totals equal the sum of emitted rows;
- fees survive parsing and deduplication;
- fee subtotals are grouped by broker and original currency;
- a fee uses its own valid broker-provided rate without an official-rate prompt;
- an official rate entered for a FURS value is not reused for a fee;
- a missing or unusable fee rate keeps the original-currency subtotal, marks the affected broker
  and overall EUR totals unavailable, and creates no processing error;
- complete converted broker fee totals and the overall total equal the documented example;
- fees never change a FURS calculation or XML field;
- exact acquisition products remain lot basis instead of rounded Money totals;
- acquisition-date conversion precedes split adjustment and is not repeated at the action date;
- the documented foreign acquisition, split and partial-disposal example preserves its exact basis;
- partial allocations preserve the residual basis and final consumption leaves exactly zero;
- rounded money allocations add to their rounded known total without changing internal basis;
- reconstruction differences are notices, not invented balancing amounts;
- deposit thresholds use full stored totals and the required form-row grouping is respected;
- internal overflow returns `arithmetic_overflow` without changing the value;
- a Doh-Obr calculation error safely isolated to an ISIN uses the confirmed exclusion/failure
  choice, retaining unaffected income and errors without inventing replacement values;
- an XML range overflow blocks only its report file; and
- every diagnostic has the documented output scope.

## Sources

- [Banka Slovenije exchange rates](https://www.bsi.si/sl/statistika/devizni-tecaji)
- [FURS instructions for capital acquisitions and disposals](https://pisrs.si/api/datoteke/integracije/351999717)
- [FURS dividend instructions](https://pisrs.si/api/datoteke/integracije/352026041)
- [FURS interest instructions](https://pisrs.si/api/datoteke/integracije/355674942)
- [ZDoh-2 normalized-cost rules](https://pisrs.si/api/datoteke/integracije/355979996)
- [Current official Doh-KDVP schema](https://edavki.durs.si/Documents/Schemas/Doh_KDVP_9.xsd)
- [Current official Doh-KDVP display transform](https://edavki.durs.si/Documents/Transforms/Doh_KDVP_9.23-display-sl.xslt)
- [Repository Doh-KDVP schema](../resources/xml/schemas/Doh_KDVP_9.xsd): the official file
  as downloaded on 2026-10-23 (SHA-256 starting `c84c2208e2ad7098`), with
  [EDP-Common-1.xsd](../resources/xml/schemas/EDP-Common-1.xsd)
- [Older Doh-KDVP schema snapshot](../resources/xml/schemas/snapshot-2026-05-12/Doh_KDVP_9.xsd):
  the copy the legacy app used, without F2 codes `J` and `K` and `TaxDecreaseCooperative`
- [Doh-Div schema](../legacy-QT-GUI/resources/xml/edavk/schemas/Doh_Div_3.xsd)
- [Doh-DHO schema](../legacy-QT-GUI/resources/xml/edavk/schemas/Doh_DHO_4.xsd)
