# FIFO across brokers and missing trade history

## What this document explains

This document explains how the app matches sales to earlier purchases. It also explains what
happens when the imported files do not contain enough purchase history.

The selected-year rules are in [`tax_rules.md`](tax_rules.md). Event ordering and source details
are defined in [`architecture.md`](architecture.md). Shared conversion and rounding rules are in
[`calculations.md`](calculations.md). Application error scope and per-XML status are defined in
[`diagnostics.md`](diagnostics.md).

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

FIFO matching must not use floating-point comparison, an unsupported tolerance, or early
rounding.

### Rounding differences and inventory balance

The importer keeps the original quantity text and any digits discarded at the eight-decimal
`Units` boundary. Purchases, sales, and the running position are otherwise added and subtracted
exactly.

A shortage smaller than `0.00000100` may be reconciled automatically only when the source proves
it is a rounding difference and it passes the EUR impact budget below. Proof can be discarded source
digits, a documented full-position sale, a broker ending position, or a broker control total. The
processor applies the smallest possible adjustment to the documented source quantity and records a `quantity_rounding_reconciled` notice
with the original value, adjusted value, difference, and supporting source. It must not create a
purchase lot or cost basis.

If several rounded parts come from one known exact total, use the largest-remainder method so the
parts add back to that total. A small difference without supporting evidence returns a
`quantity_correction_required` request. Without a confirmed correction, or when the difference is
`0.00000100` or greater, the shortage is `incomplete_history`.

For controlled reconciliation, bound the absolute EUR acquisition and disposal-value changes
using the affected prices; use the larger impact, not a signed difference that can cancel out.
Track the sum of absolute reconciliation impacts for the ISIN across all processed history in the
current run, including rounded corporate-action quantities. It must remain strictly below EUR
`0.01`. Use exact integer/rational comparisons rather than rounding the impact down to cents.
A difference of EUR `0.01` or more, an unknown impact, or an exhausted budget is not accepted as
rounding reconciliation, even with user acknowledgement. Request missing rates or source correction
as appropriate; otherwise use the normal incomplete-history or corporate-action error flow. Repeating a tiny adjustment does not reset the
budget. Display rounding adjustments and their cumulative impact beside the final result. For
action rounding, value the absolute residual in post-action share units using the highest affected EUR unit value. A warning acknowledgement alone never increases the budget.

Example: a documented full-position sale reports `1.00000000` units while imported inventory is
`0.99999999`. At EUR `50.0000` per unit, reducing the sale by `0.00000001` has EUR `0.00000050`
impact and can be reconciled with its evidence. Two corrections each with EUR `0.006` impact would
exceed the combined budget: the second is not accepted as rounding recovery.

### Confirming a source-quantity correction

The request contains the ISIN, event source and tax date, original quantity and source text, proposed
quantity, quantity difference, evidence if available, estimated EUR impact and current budget,
and the resulting position. If impact is unknown, include its missing currency/date requirements
instead of an invented EUR value. The user must confirm the proposed correction or supply a corrected
quantity. Validation rejects non-positive, out-of-range, or more-than-eight-decimal quantities.
A correction of a documented source error larger than the reconciliation limit requires supporting
broker data and is recorded as `source_quantity_corrected`, not as automatic rounding recovery.

Preserve the original event and apply a run-local correction overlay. After confirmation, rebuild
that ISIN from the complete input history and recalculate any loss checks. Do not patch only its
closing balance or create a purchase lot. Record old and new quantities, evidence or the explicit
user assertion, and whether the correction was automatic or user supplied. A changed input event
invalidates its correction. Cancelling or rejecting a correction returns to the ordinary ISIN
failure-resolution flow; it does not silently approve the proposed quantity.

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

## Supported corporate actions

The processor supports a split or reverse split only when:

- the investment keeps the same ISIN;
- the action changes only the number of units;
- no cash or other investment is received; and
- no part of the position is sold or cancelled for cash.

A supported action changes the open FIFO lots. It is not a purchase or sale and does not create a
report row by itself.

The action must keep:

- each lot's original purchase date and source;
- the relative FIFO order of all lots; and
- each lot's total purchase value.

It changes the lot's units and purchase value per unit. A split increases units and reduces value
per unit. A reverse split reduces units and increases value per unit.

## Finding the split ratio

A split ratio says how many new shares replace a number of old shares. It is written here as:

```text
new shares / old shares
```

For example, `2 / 1` means that every old share becomes two new shares. A reverse split of
`1 / 10` means that every ten old shares become one new share.

The ratio must come from a source that states its meaning clearly. A broker may provide it as two
numbers or as an unambiguous field with documented meaning. The processor may use such a ratio
automatically after validating it.

The current Trade Republic CSV supplies a decimal `shares` value for a split row, but its public
meaning has not been verified. The value could be a change in units, a resulting position, or
another broker-specific value. The app must preserve it as source data but must not use it alone
to calculate a ratio or decide whether the action is a split or reverse split.

### Trade Republic user confirmation

When a Trade Republic action has no verified ratio, the frontend asks the user to find the split
announcement and enter:

- the number of new shares; and
- the number of old shares those new shares replace.

The prompt shows the investment name, ISIN, effective date, source file, and source row. It also
shows the CSV `shares` value as read-only source information and says that the app does not know
what that value means. It gives simple examples: “2 new for 1 old” and “1 new for 10 old.” It tells
the user not to continue if the announcement mentions cash, another investment, or a changed ISIN.

Both entered numbers must be positive whole numbers. Their greatest common divisor is removed, so
`20 / 10` is stored as `2 / 1`. Equal numbers are rejected because they would not change the
position. A ratio above one is a split; a ratio below one is a reverse split. The frontend shows
the resulting total units and asks the user to confirm before processing continues.

The processing result represents this prompt as a structured `corporate_action_ratio_required`
request. The affected ISIN waits for an answer, but unrelated ISINs, dividends, and interest do
not. The frontend must use this request instead of reading a log message. Invalid entries are
rejected with a clear field error and do not change inventory.

The confirmation applies only to the combined action identity, complete source set, and report run.
It must not be saved and silently reused for another action. The application result records that the ratio was supplied by
the user, together with the original action source, but must not claim that the broker supplied it.

If the user cancels or cannot find the ratio, the app reports `corporate_action_ambiguous`. The
affected ISIN follows the normal exclusion flow. No developer-mode option may bypass the missing
ratio.

After a reliable or user-confirmed ratio is known, it is applied once to every open FIFO lot for
that ISIN across every supplied broker and account. A split is an instrument event, not a separate
broker event. Multiple broker records describing the same economic action must not apply it twice.

### Combining action sources and warning about mismatches

First deduplicate exact broker transactions as defined in `architecture.md`, preserving every
source reference. Then build economic-action candidates by ISIN, effective tax date and action
type. A verified global action identifier may establish identity across sources; a broker's
transaction ID alone cannot. Combine records automatically only when dates, types and verified
ratios agree and the sources do not indicate distinct actions. Equal ratios alone are not proof
that two actions on one date are the same action.

For each ISIN and calendar year represented by actions used in either the FIFO ledger or the
separate loss-analysis view, if action records originate from more than one source file or broker,
emit one `corporate_action_sources_mismatch_possible` warning. This includes relevant actions in
January following the selected year; they affect only loss analysis, not the closing FIFO position.
This applies even when records agree or are exact copies from overlapping exports. List all action
dates, source references, types and known ratios for that ISIN/year. The user-facing message asks
the user to check for a potential mismatch; it does not claim the records are wrong. Acknowledgeable
warnings remain visible beside the download and do not themselves block processing.

The same calendar year is a warning group, never a deduplication key. Two established actions on
different dates remain two actions. If records could describe the same action but disagree about
the effective date, type or ratio, or could describe distinct actions on one date, return
`corporate_action_identity_required`. Show all candidates and ask the user to confirm one action
with its effective date or identify separate actions and their order. A user-selected ratio cannot
silently override a contradictory verified broker ratio; the user must correct or identify the
conflicting source. Unresolved conflicts produce `corporate_action_inconsistent` or
`corporate_action_ambiguous` as appropriate.

Once identity is established, one `corporate_action_ratio_required` request covers every source of
that action when the ratio is still unknown. The confirmation is keyed to the combined action and
its source set, not just the ISIN and date. New or changed source records invalidate it. One confirmed
ratio adjusts the complete ISIN pool once, including lots at a broker that omitted the action row.

Example: TR and IBKR each record a 2-for-1 split for the same ISIN on 1 June 2024. Warn about the
multiple sources, retain both references, and apply one split to the combined position. If IBKR
records 3 June instead, the warning is accompanied by an identity-resolution request. If two
verified splits occurred in February and November, warn for their shared year but apply both once.

Mergers follow the same instrument-wide principle, but a merger may change ISINs, pay cash or
require tax deferral rules. They remain unsupported until old-to-new instrument mapping and tax
treatment are specified. A split-ratio confirmation does not authorize a merger.

The ratio is kept as an exact fraction during processing. The app must not round it to the
eight-decimal `CorpRatio` scale and then use the rounded value to adjust lots.

## Exact lot adjustment

Adjusted quantities use the eight-decimal `Units` scale. Let `inventoryBefore` be the sum of all
open FIFO lots for the ISIN. The combined target is:

```text
inventoryAfter = inventoryBefore * newShares / oldShares
```

Keep the exact rational target, then round the combined target once to eight decimals with halves
away from zero. A sub-step rounding residual is permitted when the action is documented as
unit-only, the EUR impact budget is satisfied, and no source indicates cash or cancellation of
fractions. Record `corporate_action_quantity_rounded` with the exact target, rounded target,
difference and impact. Do not infer cash in lieu from that residual. Unknown cash treatment or an
unresolved budget/impact produces an action error or a request for the missing facts.

For every open lot:

1. Calculate `old lot units * newShares / oldShares` with a wide integer intermediate.
2. Keep the whole eight-decimal unit portion and its discarded remainder.
3. Add the kept portions from every lot.
4. Compare that sum with the once-rounded combined `inventoryAfter`.
5. Distribute any remaining smallest units, `0.00000001`, to lots in order of largest discarded
   remainder.
6. When remainders are equal, give the unit to the older FIFO lot first.

This is the largest-remainder method. It prevents independent rounding from making the adjusted
lots disagree with the once-rounded combined target.

Each lot keeps its exact total purchase value, including digits beyond `Money` precision as
defined in `calculations.md`. Its adjusted value per unit is that unchanged total
divided by its adjusted units. The calculation stays exact until output and is then rounded once
to eight decimal places, with halves rounded away from zero. A rounded value per unit must never
replace the preserved total purchase value used by later calculations. The shared arithmetic and
XML boundary rules are defined in [`calculations.md`](calculations.md).

If an adjusted non-empty lot would become zero, its purchase value could not be preserved without
a cash or fractional-share rule. The action is then unrepresentable and must not be applied.

## Corporate actions on the same date as trades

A supported split or reverse split is applied before every purchase or sale for the same ISIN on
its effective tax date. The day's trades therefore use the adjusted units and values.

Different ISINs do not affect each other. Dividends and interest do not change FIFO inventory, so
their order relative to the action does not change its result.

If several corporate actions affect the same ISIN on one date, reliable source timestamps decide
their order. When timestamps are missing or equal and the order changes the result, the action is
ambiguous. Stable input sequence may keep diagnostics deterministic, but it must not be used to
guess the tax result.

## Corporate-action examples

### Split before and reverse split during the selected year

Assume that 2024 is selected:

| Date | Event | FIFO result |
| --- | --- | --- |
| 2022-05-10 | Buy `3.00000000` units at `120.00000000` per unit. | Create a lot with a total purchase value of `360.00000000`. |
| 2023-08-01 | TR records a split. The announcement says 2 new shares for 1 old share. | The user confirms `2 / 1`; the lot becomes `6.00000000` units at `60.00000000`. |
| 2024-04-01 | TR records a reverse split. The announcement says 1 new share for 3 old shares. | The user confirms `1 / 3`; the lot becomes `2.00000000` units at `180.00000000`. |
| 2024-06-01 | Sell `1.00000000` unit. | Match it to the 2022 lot; `1.00000000` adjusted unit remains. |

The 2023 split is processed because it establishes the opening 2024 position. The 2024 reverse
split is processed before the June sale. Both actions keep the 2022 purchase date and total
purchase value. Neither action creates a 2024 report row by itself.

### Exact distribution across several lots

Assume two FIFO lots contain `1.00000000` and `2.00000000` units. A confirmed `5 / 3` split
changes the total from `3.00000000` to `5.00000000`.

The exact lot results are about `1.666666666...` and `3.333333333...`. Keeping eight decimal
places first gives `1.66666666` and `3.33333333`, which total `4.99999999`. The remaining
`0.00000001` goes to the first lot because it has the larger discarded remainder.

The final lots are therefore:

- `1.66666667` units for the first lot; and
- `3.33333333` units for the second lot.

Their sum is exactly `5.00000000`. If their unchanged total purchase values are `10.00000000`
and `20.00000000`, their output values per unit are `5.99999999` and `6.00000001`. The
unchanged lot totals, not these rounded output values, remain authoritative.

### A unit-only action with a sub-step residual

One lot contains `1.00000000` units with EUR `10.000000000000` exact purchase basis. A confirmed
`1 / 3` reverse split has exact target `1/3`, rounded once to `0.33333333` units. Its unchanged
basis gives an output value per unit of `30.00000030`. The absolute residual is `1/300000000`
post-action units, with an impact of about EUR `0.00000010`, below the budget. Record
`corporate_action_quantity_rounded` and continue when the source confirms unit-only treatment.
The lot remains non-empty and keeps its original acquisition date and entire basis.

## Corporate-action errors

Corporate-action errors use these diagnostic codes:

| Code | When it is used |
| --- | --- |
| `corporate_action_missing_position` | No positive covered position exists immediately before the action. |
| `corporate_action_inconsistent` | Reliable source ratios conflict with each other, or a confirmed ratio conflicts with an unambiguous source value. |
| `corporate_action_ambiguous` | A reliable ratio is unavailable, the user does not confirm one, or the order of several same-day actions cannot be established. |
| `corporate_action_unrepresentable` | Arithmetic overflows or the action cannot preserve lots and their purchase values at the required precision. |
| `unsupported_corporate_action` | The action needs tax rules that are not documented here. |

Every error includes the investment name, ISIN, action source, and a clear reason. The affected
ISIN is unsafe and follows the normal exclusion flow in this document. Other ISINs, dividends, and
interest may still be processed.

The incomplete-history developer override does not apply to corporate-action errors. Even in
developer mode, the affected ISIN must be excluded because the app has no reliable ratio or tax
treatment to use.

## Unsupported corporate actions

The app does not currently process:

- mergers;
- spin-offs;
- rights or subscription issues;
- conversions or changes to another ISIN;
- cash paid instead of fractional units;
- capital repayments; or
- any action that changes both units and another asset or cash balance.

These events produce an `unsupported_corporate_action` error. The app must preserve their source
details for the user, but it must not turn them into a split, purchase, sale, or invented ratio.

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

Normal mode first collects every unsafe ISIN. An ISIN made unsafe by incomplete history or a
corporate-action error is excluded from the Doh-KDVP report only after the user confirms that
choice. Developer mode provides an override only for incomplete history.

These exclusions apply only to Doh-KDVP. Doh-Obr has its own analogous ISIN exclusion/failure
choice for safely isolated income errors, defined in `diagnostics.md`; neither form's decision
authorizes an exclusion from the other form. Both retain original source evidence.

The app must exclude every purchase, sale, and calculated match for that ISIN from capital report
output. It must not exclude only the unmatched sale because the remaining rows would show an
unreliable history. Preserve source evidence and existing replacement allocations needed by retained
losses in the separate analysis view, as defined in `tax_rules.md`. An exclusion does not resolve
uncertain evidence for another ISIN: request its dependent loss decision before generating XML.

The frontend presents one list containing every unsafe investment's name, ISIN, error reason, and
source location when available. The list is sorted by ISIN and then name. The user chooses one of
these actions:

1. **Continue without all listed investments.** Exclude every listed ISIN and generate Doh-KDVP
   from the processable investments that remain.
2. **Do not generate Doh-KDVP.** Fail the capital-gains report and generate no capital-gains XML.

Neither action is selected by default. The user may instead return to file selection and add or
correct statements. The app must not offer exclusion for an error that cannot be isolated safely
to a complete ISIN.

| Output | Result |
| --- | --- |
| Doh-KDVP XML | Generated with every processable ISIN and without every ISIN the user agreed to exclude. |
| Dividend XML | Continues independently and is generated when its own data is valid. |
| Interest XML | Continues independently and is generated when its own data is valid. |

If no processable ISIN remains, no Doh-KDVP file is generated. Dividend and interest reports still
continue.

If the user chooses not to generate Doh-KDVP, its result is `failed` and contains no XML. If
another report has its own error, that error may block that report under its own rules.

After exclusion, the result contains one deduplicated `excludedInvestments` entry for every
excluded ISIN. Each entry contains its name, ISIN, reason codes, and diagnostic IDs. The result
screen and download screen show the complete list; they must not show only the first failure or a
count without the names and ISINs. The capital-gains result records
`failureResolution: exclude_failed_instruments`.

When the user chooses not to generate Doh-KDVP, the result records
`failureResolution: fail_report` and has an empty `excludedInvestments` list because no ISIN was
silently skipped. `failureResolution` is absent when the run did not need this choice.

## What the frontend shows

The processing result must contain both successful files and all errors. For incomplete history,
the frontend must:

- show an error with the affected investment's name and ISIN;
- explain that purchase history is missing and that older or missing statements may fix it;
- show the failing sale's source location when available;
- offer clear choices to go back, continue without every listed affected ISIN, or generate no
  Doh-KDVP XML;
- list every excluded name and ISIN before the user confirms;
- state on the result screen that the downloaded Doh-KDVP does not contain those investments;
- keep successful dividend and interest downloads available; and
- never describe an excluded investment as successfully processed.

The main message must be clear and specific, for example: "Cannot process Example Investment
(XX0000000001) because earlier purchase history is missing. Add older statements or continue
without this investment."

The user-facing error comes from structured processing data. The frontend must not parse logs or
hide the error behind a general failure message. Filename and source row number are safe location
details; the diagnostic must not contain the raw CSV row.

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

After the third confirmation, rebuild a separate source-event export for every overridden ISIN.
Include all known acquisitions needed to describe the position and all selected-year source sales,
including the unmatched sale in full. Calculate the running quantity by applying known historical
and in-year events; it may become negative after a shortage. Preserve the actual acquisition dates,
quantities and converted unit values. No fabricated acquisition date or cost is attached to an
unmatched sale, and no guessed FIFO matches are generated after the first shortage. For an
unmatched sale with unknown basis, omit optional `F10` and record that loss eligibility could not
be assessed; do not invent a gain/loss or assert loss eligibility.

Unresolved loss-eligibility questions still require their normal answers. A corporate action that
cannot be applied from reliable pre-action inventory still fails; developer mode cannot bypass it.
The signed running stock is emitted only where the applicable XSD permits it. Structurally invalid
XML or any other blocking error still fails the affected report. Rejoining a positive raw balance
does not re-establish reliable FIFO state. Normal mode always returns to the unchanged strict FIFO
rules when the override is cancelled or the input changes.

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
- a positive quantity balance remains as open inventory and is not forced to zero;
- a shortage below `0.00000100` is reconciled only with documented rounding evidence or a
  user-confirmed source correction;
- every reconciliation records its original value, adjusted value, difference, and evidence;
- a reconciliation never creates a purchase lot or cost basis;
- cumulative absolute EUR reconciliation impact stays below EUR 0.01 for the whole ISIN/run;
- equal-to-limit, unknown-impact, and repeatedly accumulated differences require resolution;
- quantity corrections use structured requests and preserve original events in a run-local overlay;
- confirmed corrections rebuild history and invalidate when their source changes;
- an unsupported or larger shortage produces `incomplete_history`;
- a split increases units, reduces value per unit, and preserves purchase dates and total values;
- a reverse split reduces units, increases value per unit, and preserves the same details;
- a Trade Republic decimal `shares` value is not treated as a ratio, unit change, or resulting
  position without a verified source definition;
- the Trade Republic prompt identifies the action and asks for new shares and old shares;
- valid user-entered ratios are reduced and determine whether the action is a split or reverse
  split;
- an invalid ratio entry is rejected without changing inventory;
- a cancelled ratio request leaves the action ambiguous and excludes only its ISIN after
  confirmation;
- a source-provided ratio is accepted automatically only when its meaning is unambiguous;
- adjusted lots use deterministic largest-remainder allocation and total the once-rounded position;
- representable unit-only splits continue after a bounded sub-step rounding adjustment;
- multiple action sources within one calendar year always show the potential-mismatch warning;
- relevant following-January actions receive the same warning without altering year-end inventory;
- matching action records apply once to all brokers' lots, including brokers without an action row;
- the warning year never merges distinct action dates;
- ambiguous identity or conflicting source facts require explicit resolution;
- source-set changes invalidate action identity and ratio confirmations;
- adjusted output values per unit are rounded once to eight decimal places and serialized with
  four to eight decimal places as defined in `calculations.md`;
- a supported action is applied before same-day trades for the same ISIN;
- ambiguous same-day actions produce `corporate_action_ambiguous`;
- missing, inconsistent, overflowing, and unrepresentable actions produce their documented errors;
- unsupported actions are not converted into guessed trades or ratios;
- corporate-action errors make only their ISIN unsafe;
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
- one prompt lists all unsafe ISINs and offers exclusion of all of them or failure of Doh-KDVP;
- choosing failure creates no Doh-KDVP XML and keeps valid dividend and interest files available;
- the final result lists every excluded name, ISIN, reason code, and diagnostic ID;
- the final result records whether the user chose exclusion or failure of Doh-KDVP;
- an error without a complete, safely isolatable ISIN cannot use the exclusion flow;
- developer mode is disabled by default;
- developer mode requires three separate confirmations for each run;
- cancelling any developer confirmation does not generate unsafe output;
- developer mode includes the whole known source sale and permits signed stock only under its XSD;
- developer mode never restarts guessed FIFO matching after a history gap;
- developer mode cannot bypass loss decisions, action errors or XML schema validation; and
- developer output remains marked unsafe with the `incomplete_history` error.

## Sources

ZDoh-2 Article 103 requires FIFO records for the taxpayer's stock of the same type of capital:

- [ZDoh-2 in the Slovenian Legal Information System](https://pisrs.si/Pis.web/pregledPredpisa?id=ZAKO4697)

The FURS schema allows up to eight decimal places for security quantities and values per unit. The
official display transform keeps at least four decimal places and preserves up to eight:

- [Current official Doh-KDVP schema](https://edavki.durs.si/Documents/Schemas/Doh_KDVP_9.xsd)
- [Current official Doh-KDVP display transform](https://edavki.durs.si/Documents/Transforms/Doh_KDVP_9.23-display-sl.xslt)
- [Repository Doh-KDVP schema snapshot](../legacy-QT-GUI/resources/xml/edavk/schemas/Doh_KDVP_9.xsd)

Trade Republic describes splits by ratio and directs users to the action announcement for its
details. Its public guidance does not define the split row's CSV `shares` value:

- [Trade Republic: Types of corporate actions](https://support.traderepublic.com/en-gr/1678-What-are-different-types-of-corporate-actions)
- [Trade Republic: Where to find corporate-action details](https://support.traderepublic.com/de-lu/1592-Where-can-I-find-out-about-corporate-actions)

These split rules are intentionally limited to same-ISIN actions that change units only. No
broader corporate-action tax treatment is inferred from the FIFO law or XML format.
