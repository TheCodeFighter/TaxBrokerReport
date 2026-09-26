# FIFO across brokers and missing trade history

## What this document explains

This document explains how the app matches sales to earlier purchases. It also explains what
happens when the imported files do not contain enough purchase history.

The selected-year rules are in [`tax_rules.md`](tax_rules.md). Event ordering and source details
are defined in [`architecture.md`](architecture.md).

## One FIFO pool for the same investment

FIFO means that shares bought first are treated as sold first.

For one taxpayer, all supplied broker accounts use one FIFO pool for the same investment. A
purchase at one broker can therefore be matched to a later sale at another broker.

The ISIN identifies the investment:

- trades with the same ISIN share one FIFO pool;
- the broker and account do not create separate pools;
- different ISINs never share a pool, even when their names or symbols are similar; and
- a name or symbol must not be used to guess a missing ISIN.

Broker, account, filename, row, and transaction ID remain attached to each event for traceability.
They do not change which FIFO pool is used.

A corporate action may change an instrument or its units only when a documented corporate-action
rule allows it. A name change alone must not move shares between ISINs.

## How matching works

The app processes the combined ledger in the order defined in [`architecture.md`](architecture.md).
It does not finish one broker before starting another.

For each ISIN:

1. A purchase creates a FIFO lot with its remaining units, purchase date, purchase value, and
   source details.
2. A sale uses the oldest lot with units still available.
3. If that lot is larger than the sale, only the needed part is used.
4. If that lot is smaller than the sale, all of it is used and matching continues with the next
   oldest lot.
5. Each matched part keeps the details of its original purchase.

A sale can use several purchase lots. A purchase lot can also be used by several sales until no
units remain.

A sale can use only purchases that come before it in the combined ledger. A later purchase cannot
repair an earlier shortage.

## Exact fractional units

Fractional units are matched exactly with the fixed-point `Units` type. It stores eight digits
after the decimal point.

For example:

- a purchase of `0.75000000` units followed by a sale of `0.20000000` units leaves
  `0.55000000` units; and
- a later sale of `0.55000000` units uses the rest of that lot and leaves exactly zero.

FIFO matching must not use floating-point comparison, an approximate tolerance, or early rounding.

## Trades before the selected year

Older trades build the opening FIFO position for the selected year. This includes both purchases
and sales.

An older sale uses older purchase lots even though that sale does not appear in the selected-year
report. The remaining units, purchase dates, and purchase values then carry into the selected
year.

An unmatched older sale is still an incomplete-history error. The app must not ignore it merely
because it happened before the selected year.

## Cross-broker example

Assume that 2024 is selected and every trade below has the same ISIN.

| Date | Broker and account | Trade | FIFO result |
| --- | --- | --- | --- |
| 2021-02-10 | Broker A, account 1 | Buy `1.50000000` units. | Create the first lot with `1.50000000` units. |
| 2023-04-05 | Broker B, account 2 | Sell `0.40000000` units. | Use part of the first lot; `1.10000000` units remain. |
| 2023-09-10 | Broker A, account 1 | Buy `0.75000000` units. | Create a second lot. |
| 2024-06-01 | Broker B, account 2 | Sell `1.60000000` units. | Use the remaining `1.10000000` units from the first lot and `0.50000000` units from the second lot. |

The 2023 sale is not reported for 2024, but it changes the 2024 opening position. The 2024 sale
creates two FIFO matches, even though its purchase lots and sale came from different brokers and
accounts. The second lot keeps `0.25000000` units.

## Missing purchase history

For every sale processed through the end of the selected year, the app first totals all earlier
units still available for the same ISIN. The sale has incomplete history when it needs more units
than that total.

This is an error, not a warning. It applies when:

- no earlier purchase is available;
- some earlier units are available but not enough for the full sale; or
- an earlier unmatched sale has already made that ISIN's history unreliable.

When this happens, the app must:

1. report an `incomplete_history` error for the first unmatched sale for that ISIN;
2. attach the sale's broker, filename, source row, and transaction ID when available;
3. show the affected investment's name and ISIN;
4. leave the purchase lots unchanged for the failing sale;
5. stop FIFO matching for that ISIN so later errors are not based on guessed state; and
6. mark the whole ISIN as unsafe for normal Doh-KDVP output.

The failing sale is handled as one unit. The app must not report only its matched part and hide the
unmatched part.

The app must never:

- create a purchase that is not present in the input;
- use zero, the sale price, an average price, or another guessed purchase cost;
- match the sale to a later purchase;
- keep separate broker pools to avoid the shortage;
- exclude the sale or ISIN without clearly telling the user;
- reduce the error to a warning.

The user can add the missing or older broker statements and run the report again. The user can
also confirm that the normal report should continue without the whole affected ISIN.

## Incomplete-history example

Assume that 2024 is selected and every capital trade below has the same ISIN.

| Date | Broker | Event | Result |
| --- | --- | --- | --- |
| 2022-03-01 | Broker A | Buy `0.60000000` units. | The FIFO pool holds `0.60000000` units. |
| 2023-08-01 | Broker B | Sell `0.25000000` units. | The older sale uses part of the lot; `0.35000000` units remain. |
| 2024-05-01 | Broker B | Sell `0.50000000` units. | Only `0.35000000` units are available, so `0.15000000` units cannot be matched. |
| 2024-06-01 | Broker A | Receive a dividend. | Dividend processing remains valid. |
| 2024-07-01 | Broker B | Receive interest. | Interest processing remains valid. |

The May sale creates an `incomplete_history` error. The app shows the investment name and ISIN
and does not guess a purchase cost for the missing `0.15000000` units. In normal mode, the user
can continue with that whole ISIN excluded.

## Normal mode

Normal mode is used unless developer mode was explicitly enabled. An ISIN with incomplete history
is excluded from the Doh-KDVP report only after the user confirms that choice.

The app must exclude every purchase, sale, and calculated match for that ISIN. It must not exclude
only the unmatched sale because the remaining rows would show an unreliable history.

| Output | Result |
| --- | --- |
| Doh-KDVP XML | Generated with every processable ISIN and without every ISIN the user agreed to exclude. |
| Dividend XML | Continues independently and is generated when its own data is valid. |
| Interest XML | Continues independently and is generated when its own data is valid. |

If no processable ISIN remains, no Doh-KDVP file is generated. Dividend and interest reports still
continue.

If another report has its own error, that error may block that report under its own rules.

## What the frontend shows

The processing result must contain both successful files and all errors. For incomplete history,
the frontend must:

- show an error with the affected investment's name and ISIN;
- explain that purchase history is missing and that older or missing statements may fix it;
- show the failing sale's source location when available;
- offer a clear choice to go back or continue without every affected ISIN;
- list every excluded name and ISIN before the user confirms;
- state on the result screen that the downloaded Doh-KDVP does not contain those investments;
- keep successful dividend and interest downloads available; and
- never describe an excluded investment as successfully processed.

The main message must be clear and specific, for example: "Cannot process Example Investment
(XX0000000001) because earlier purchase history is missing. Add older statements or continue
without this investment."

The user-facing error comes from structured processing data. The frontend must not parse logs or
hide the error behind a general failure message.

## Developer mode

Developer mode may generate a Doh-KDVP file that includes ISINs with incomplete history. This is
an unsafe override for testing and investigation. It is disabled by default and must not be
presented as the normal solution.

Before generating the unsafe file, the app must require three separate confirmations:

1. Warn that purchase history is missing and the Doh-KDVP data will be incomplete.
2. Show every affected investment name and ISIN, and warn that the XML may be rejected or may
   produce an incorrect tax result.
3. Ask the user to confirm explicitly that they still want to generate the incorrect Doh-KDVP XML.

Each confirmation requires a separate user action. One checkbox, one dialog with repeated text, or
a saved preference is not enough. Cancelling at any step returns to the normal choice of excluding
the affected ISINs or adding more statements. The three confirmations apply only to the current
run and must be repeated next time.

After the third confirmation, the developer-mode XML includes only the purchases and sales that
exist in the input. It may include the unmatched sale and an incomplete running position. The app
must not create a fake purchase or cost, even in developer mode.

The processing result and download screen must mark this file as unsafe and list every affected
name and ISIN. The `incomplete_history` error remains present; developer mode does not turn it into
a warning or a successful FIFO calculation.

## Required processor tests

Tests based on this document must prove that:

- trades with the same ISIN share one FIFO pool across brokers and accounts;
- broker or account names do not change matching order;
- different ISINs use separate FIFO pools;
- names and symbols are not used as replacement instrument identities;
- an older sale changes the opening position for the selected year;
- a partial sale leaves the correct remaining units with their original purchase details;
- one sale can use several purchase lots;
- fractional units are exact to eight decimal places;
- a sale cannot use a later purchase;
- a sale with no available units creates an `incomplete_history` error;
- a sale with some but not enough units creates the same error;
- an unmatched sale before the selected year excludes that ISIN in normal mode;
- no unmatched sale is dropped or given an invented purchase cost;
- the error contains the affected investment name, ISIN, and sale source;
- normal mode excludes every row for the affected ISIN;
- the user must confirm before normal mode continues without that ISIN;
- processable ISINs remain in the same Doh-KDVP XML;
- no Doh-KDVP file is generated when every ISIN is excluded;
- successful dividend and interest XML files remain available;
- the frontend lists every excluded investment;
- developer mode is disabled by default;
- developer mode requires three separate confirmations for each run;
- cancelling any developer confirmation does not generate unsafe output;
- developer mode includes only known source events and never invents a purchase cost; and
- developer output remains marked unsafe with the `incomplete_history` error.

## Official source

ZDoh-2 Article 103 requires FIFO records for the taxpayer's stock of the same type of capital:

- [ZDoh-2 in the Slovenian Legal Information System](https://pisrs.si/Pis.web/pregledPredpisa?id=ZAKO4697)
