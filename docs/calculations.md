# Conversion, rounding, and fees

## What this document explains

This document defines how the app converts foreign currencies, rounds numbers, shows broker fees,
and rejects values that cannot be represented safely. These rules apply to capital gains,
dividends, interest, and the final FURS XML files.

The selected-year rules are in [`tax_rules.md`](tax_rules.md). FIFO and corporate actions are in
[`fifo.md`](fifo.md).

## Fixed-point values

Tax calculations must not use binary floating-point numbers.

| Value | Stored precision | Smallest stored step |
| --- | --- | --- |
| `Money` | 4 decimal places | `0.0001` |
| `Units` | 8 decimal places | `0.00000001` |
| `ExchangeRate` | 8 decimal places | `0.00000001` |
| `CorpRatio` | 8 decimal places when stored | `0.00000001` |

An exact corporate-action fraction is kept as two whole numbers while it is applied. It is not
first rounded to `CorpRatio` precision.

Every multiplication or division uses a checked wide-integer intermediate. The processor rounds
only at a boundary defined below. It must not round an operand early to make an operation easier.

### Stored precision is not calculation precision

The scales in the table describe stored values, not a smaller workspace used for calculations. A
calculation keeps the complete checked integer product or the exact quotient and remainder until
the required result scale is known. For example, multiplying four-decimal `Money` by
eight-decimal `Units` keeps the full product of both integers, which carries 12 effective decimal
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

Source money is rounded once to four decimals when it enters `Money`. Source units are rounded
once to eight decimals when they enter `Units`, but the original text and discarded digits remain
available for the controlled FIFO reconciliation defined in `fifo.md`. A broker rate is rounded
once to eight decimals only when its documented source format permits extra precision. A
user-entered official rate must have no more than eight decimals and is not rounded for the user.

Unit examples:

| Input | Stored `Units` |
| --- | --- |
| `0.123456784` | `0.12345678` |
| `0.123456785` | `0.12345679` |
| `-0.123456785` | `-0.12345679` before any trade-side normalization |

Once imported, money and units keep their stored precision until a later operation explicitly
defines another boundary. Formatting a value for display must not change the stored value.

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

## Asking the user for official rates

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
positive. The frontend shows the direction and a conversion example. An unexplained decimal must
not be guessed to mean either foreign currency per EUR or EUR per foreign currency.

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

## Money and units

Multiplying a unit price by units uses:

```text
totalMoney = roundHalfAwayFromZero(
    unitPrice * units / UNITS_SCALE
)
```

For example:

```text
204.3000 * 0.14760000 = 30.15468000 -> 30.1547
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
match, compare them before XML output at their common stored scale. An unexplained difference is
invalid source data, not a rounding adjustment. The processor must report it and must not guess
which value is correct.

## FURS XML boundaries

The XML writer receives validated report values. It does not repeat tax calculations or repair an
invalid value.

| XML value | Output precision | Rule |
| --- | --- | --- |
| Doh-KDVP quantities and stock (`F3`, `F7`, `F8`) | 4 to 8 decimals | Write exact `Units`; remove trailing zeros only down to four decimals. |
| Ordinary Doh-KDVP unit values (`F4`, `F9`) | 4 decimals | Write `Money` without changing its value. |
| Corporate-action-adjusted Doh-KDVP unit values | 4 to 8 decimals | Round once as defined in `fifo.md`; remove trailing zeros only down to four decimals. |
| Dividend value and foreign tax | 2 decimals | Round `Money` once at XML output. |
| Interest value and foreign tax | 2 decimals | Round `Money` once at XML output. |
| Interest totals | 2 decimals | Sum the already emitted two-decimal row values. |

The current official Doh-KDVP schema accepts up to eight decimal places for quantities, stock, and
unit values. The official FURS display transform shows these fields with at least four decimal
places and preserves up to eight. Eight places are a limit, not a requirement to add zeros.

For example:

| Stored value | XML value |
| --- | --- |
| quantity `2.00000000` | `2.0000` |
| quantity `0.12345678` | `0.12345678` |
| ordinary unit value `12.3456` | `12.3456` |
| adjusted unit value `12.34560000` | `12.3456` |
| adjusted unit value `12.34567891` | `12.34567891` |

Doh-KDVP unit values must not be rounded to two decimals. A per-unit value can need fractions of a
cent, and FURS explicitly accepts and displays that precision. Dividend or interest `12.3450` is
instead written as `12.35` because those XML fields use two decimals.

If two interest rows each contain `0.0050`, each row is written as `0.01` and their XML total is
`0.02`. Summing `0.0100` first and emitting a total of `0.01` would make the total disagree with
the rows and is not allowed.

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
| `invalid_exchange_rate` | A rate is zero, negative, malformed, too precise, out of range, or has an unknown direction. |
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
| Interest value | Block the interest XML, but keep valid capital and dividend outputs. |
| Final XML field range | Block only the XML file containing that field. |

Developer mode must not bypass a missing rate, arithmetic overflow, or XML range error.

### Overflow and range examples

The largest positive `Money` value is `922337203685477.5807`. Parsing
`922337203685477.5808` produces an overflow error.

Converting `922337203685477.5807` with an official rate of `0.50000000` would double the value
beyond the `Money` range. It produces `arithmetic_overflow`; it is not clamped to the maximum.

A Doh-KDVP `F4` or `F9` value may contain at most 14 integer digits and 8 decimal places. Internal
`Money` value `100000000000000.0000` therefore produces `xml_value_out_of_range` because it has
15 integer digits, even though it still fits in `Money`.

## Required processor and XML tests

Tests based on this document must prove that:

- fixed-point parsing rounds positive and negative half values away from zero;
- unit inputs round once to eight decimals at the documented import boundary;
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
- internal overflow returns `arithmetic_overflow` without changing the value;
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
- [Repository Doh-KDVP schema snapshot](../legacy-QT-GUI/resources/xml/edavk/schemas/Doh_KDVP_9.xsd)
- [Doh-Div schema](../legacy-QT-GUI/resources/xml/edavk/schemas/Doh_Div_3.xsd)
- [Doh-DHO schema](../legacy-QT-GUI/resources/xml/edavk/schemas/Doh_DHO_4.xsd)
