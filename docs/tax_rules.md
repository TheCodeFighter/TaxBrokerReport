# Reporting year and trade history (capital gains)

## What this document explains

The user chooses one calendar year for the tax report. Only results from that year appear in the
report, but the app must still read older trades to calculate those results correctly.

The app first combines the imported files, removes duplicates, and sorts all events as described
in [`architecture.md`](architecture.md). Detailed FIFO and corporate-action rules belong in
[`fifo.md`](fifo.md). Currency conversion, rounding, and broker-fee rules belong in
[`calculations.md`](calculations.md). Application diagnostics and independent XML result statuses
belong in [`diagnostics.md`](diagnostics.md).

## Useful terms

- **Selected year**: the calendar year chosen by the user.
- **Tax date**: the date that decides which tax year an event belongs to. In the code, this is
  `EventMetadata::mTaxDate`.
- **Ledger**: the full ordered history of buys, sales, and corporate actions.
- **FIFO**: shares bought first are treated as sold first.
- **FIFO match**: an internal link between one sale and the units it uses from one purchase. It
  is used for the purchase value, holding periods and the loss rule. It is never a Doh-KDVP row.
- **Filing row**: one purchase or sale row in the Doh-KDVP *popisni list*, as defined in
  [Doh-KDVP filing rows](#doh-kdvp-filing-rows).
- **Corporate action**: an event such as a share split, reverse split, merger, or spin-off.

## Main rule

The selected year controls what appears in the report. It does not limit the older history used in
the calculation.

For a selected year `Y`:

- the year starts on 1 January;
- the year ends on 31 December; and
- both dates are included.

The tax date controls these limits. A broker timestamp must not move an event into another tax
year.

The app must accept any supported reporting year. It must reject an invalid year before processing
starts. It must not silently replace an invalid year with the current year or another value.

The selected-year input is a required integer from 2000 through 9999, inclusive. There is no
restriction to the current year. Missing values, values below 2000 and larger integers produce
`invalid_reporting_year` before ledger preparation. Validate the integer before converting it to
`std::chrono::year`, whose narrower representation must not wrap a rejected selection.

## How the report is built

Every report run starts with an empty ledger:

1. Combine, deduplicate, and sort all imported events.
2. Process every buy, sale, and relevant corporate action up to the end of the selected year.
3. Use older buys and sales to build the correct FIFO position.
4. Add a sale to the report only when it happened in the selected year. An earlier purchase
   appears only as a purchase row for the units that in-year sales use.
5. Include every in-year capital sale for each processable ISIN. [`fifo.md`](fifo.md) defines the
   explicit incomplete-history exception. How units past the holding period are listed is open
   (see [Long-held investments](#long-held-investments)).

Each report run rebuilds its own ledger. Running a report for one year must not change the result
of a later run for another year.

| Event date | What the app does |
| --- | --- |
| Before the selected year | Process it to build the correct history. Do not report an earlier sale. List an earlier purchase only with the units that in-year sales use. |
| During the selected year | Process it and report any required result. |
| After the selected year | Do not add it to the ledger or report it. |

There is one limited exception for loss eligibility. Keep the adjacent-year events needed for the
30-day loss check in a separate analysis view. December of the preceding year can affect a January
sale, and January of the following year can affect a December sale. Relevant acquisitions,
disposals, and supported corporate actions establish replacement quantities in that view. They do
not add later events to the selected year's FIFO ledger or report.

## Older trades and corporate actions

All supplied buys and sales before the selected year are part of the history. An older sale uses
up FIFO shares even though that sale does not appear in the selected-year report.

Corporate actions before or during the selected year are applied at the correct point in time
when building its FIFO ledger. Corporate actions after the selected year do not change that ledger
or its closing position. An action in the following year may still be used in the separate 30-day
loss-eligibility check when it affects replacement quantities; it is not applied to the selected
year's inventory or reported as an event in that year. The corporate-action rules decide how
quantities, values, and purchase dates change.

Dividends and interest do not create FIFO shares. They can appear in their reports only when their
tax date is inside the selected year.

## Doh-KDVP filing rows

The app keeps two separate models:

- **Internal FIFO matches** link each sale to the purchase units it uses. They decide the purchase
  value, holding periods and the loss rule. They are never written to XML as they are.
- **Filing rows** are the purchase and sale rows of the Doh-KDVP *popisni list* for one ISIN. They
  are built from the in-year sales and their FIFO matches.

### What the official instructions say

Section 11 of the Doh-KDVP instructions (`doh_odm_kdvp_25.n.sl.pdf` for 2025 and later; the same
text is in `doh_odm_kdvp_20.n.sl.pdf`):

> *V popisni list se vpišeta ločeno za vsako vrsto vrednostnega papirja oziroma investicijskega
> kupona vsaka pridobitev in vsaka odsvojitev vrednostnega papirja oziroma investicijskega kupona.
> Posamezna pridobitev vrednostnega papirja oziroma investicijskega kupona se vpiše v samostojno
> vrstico tako, da se izpolnijo stolpci 1, 2, 3, 4, 5 in 8 ter 11. Posamezna odsvojitev
> vrednostnega papirja oziroma investicijskega kupona se vpiše v samostojno vrstico tako, da se
> izpolnijo stolpci 6, 7, 8, 9 in 10.*
>
> *Popisni list se izpolnjuje za tiste vrednostne papirje oziroma investicijske kupone, ki so bili v
> koledarskem letu odsvojeni pred potekom petnajstih let od dneva, ko so bili pridobljeni.*
>
> *V stolpec 3 se vpiše količina pridobljenih vrednostnih papirjev oziroma investicijskih kuponov v
> posameznem poslu, razvidna iz obračuna o opravljenem poslu pridobitve oziroma iz drugega
> dokumenta, iz katerega je razvidna pridobitev.*
>
> *V stolpec 8 se vpiše izračunana zaloga vrednostnih papirjev oziroma investicijskih kuponov.
> Zaloge istovrstnega kapitala se vodijo po metodi zaporednih cen (FIFO).*

In short: each purchase and each sale is its own row; a purchase row fills F1 to F5 (and F11), a
sale row fills F6, F7, F9 and F10; both fill F8, the stock calculated by FIFO. A list is needed
only for an investment sold in the year. The schema allows either `Purchase` or `Sale` in a row,
followed by `F8`.

### Project rule: list only the units this year's sales use

For selected year `Y`, the app writes a *popisni list* only for an ISIN with at least one sale in
`Y`. The list contains:

- **One sale row for each sale in `Y`**, with the sale quantity (`F7`) and unit value (`F9`). A
  sale that uses several purchases stays one sale row. `F7` leaves out units past the holding
  period ([Long-held investments](#long-held-investments)), and a sale with only such units has no
  row. The only other exception is a sale whose loss the 30-day rule blocks only in part: it has
  one row for each run of units with the same `F10` ([The 30-day loss
  rule](#the-30-day-loss-rule)), and the quantities add up to the listed units of the sale.
- **One purchase row for each purchase that a sale in `Y` uses.** `F1` and `F4` come from that
  purchase. `F3` is the number of its units used by all sales in `Y` together, not the full
  purchase quantity, and again without units past the holding period.
- **A lot a split or reverse split changed** lists the units after the split (`F3`) and `F4` =
  the exact basis of those units over their number, rounded once to 8 decimals, half away from
  zero ([`calculations.md`](calculations.md)), with the original purchase date (`F1`). The split
  itself has no row. A purchase that sales of `Y` use both before and after a split has one row
  for each run of the same state, all at the purchase position, in the order the sales use them;
  `F4` is the price of that state. After a split and a reverse split that cancel, units sold
  before, between and after them are three rows, so a FIFO over the rows keeps each sale's gain.
- **No row for a sale before `Y`.** It was reported in its own year. The purchase units it used
  are left out of the purchase rows. Its gain or loss counts only in that year: a capital loss
  reduces only the gains of the same tax year and is not carried forward (ZDoh-2 Article 97), so
  an earlier-year loss neither appears in nor blocks the list of `Y`. The one exception in Article
  97, third paragraph (a loss on shares bought before new shares from a capital increase out of
  company funds), follows a corporate action that the app does not support and blocks.
- **No row for unused units.** Units still held at the end of `Y` appear in the list of the year
  in which they are sold.
- Rows follow the ledger order (tax date, timestamp, input sequence). A purchase row sits at its
  purchase position.
- `F8` is the running total of the listed purchase units minus the listed sale units. Every sale
  uses only earlier purchases, so `F8` is never negative, and it is 0 after the last sale row.
- `F2` (way of purchase) is `B` (*nakup*, purchase) for every broker purchase. `A`
  (*vložek kapitala*) is a capital contribution to one's own company and is not used.
- `F10` exists only on sale rows. It is `true` (*Da*) unless the 30-day loss rule blocks the loss,
  and `false` (*Ne*) for the blocked part ([The 30-day loss rule](#the-30-day-loss-rule)). A
  profitable sale gets `true`. The only exception is the developer-mode export of a sale with
  unknown purchase value ([`fifo.md`](fifo.md)), which leaves `F10` out.

This is simpler for the user than repeating the full history every year. It differs from a literal
reading of section 11: `F3` (and `F7` when units are past the holding period) is not always the
confirmation quantity, and `F8` is the stock of the listed units, not the whole holding. The gains do not change: a FIFO over the listed rows uses the
same units as the internal matches. This reading must be confirmed with an eDavki draft import
before release. eDavki imports such a list as a draft (scenario S1 in
[`verification.md`](verification.md), 2026-10-27); a draft does not show the calculation.

If the unit value of a split lot does not fit `F4` (a `UnitPrice`, at most
EUR 92,233,720,368.54775807), the ISIN is blocked (`corporate_action_unrepresentable`, key
`.unit_value`).

### Example: one sale uses two purchases

Selected year 2024:

| Date | Event | Internal FIFO matches |
| --- | --- | --- |
| 2024-01-10 | Buy 10 units at EUR 10. | |
| 2024-02-10 | Buy 20 units at EUR 20. | |
| 2024-03-10 | Sell 15 units at EUR 30. | 10 units from the 2024-01-10 purchase, 5 from the 2024-02-10 purchase. |

2024 filing rows:

| Row | Type | Date | Quantity (F3/F7) | Unit value EUR (F4/F9) | F8 |
| --- | --- | --- | --- | --- | --- |
| 1 | Purchase | 2024-01-10 | 10 | 10 | 10 |
| 2 | Purchase | 2024-02-10 | 5 | 20 | 15 |
| 3 | Sale | 2024-03-10 | 15 | 30 | 0 |

Gain: 15 × 30 − (10 × 10 + 5 × 20) = 450 − 200 = EUR 250. The sale is one row of 15, not two
sale rows of 10 and 5. The 15 unused units of the 2024-02-10 purchase are not listed.

### Example: a purchase partly sold in an earlier year

| Date | Event | Internal FIFO matches |
| --- | --- | --- |
| 2022-03-01 | Buy 15 units at EUR 10. | |
| 2023-05-10 | Sell 4 units at EUR 12. | 4 units from the 2022-03-01 purchase; 11 remain. |
| 2024-06-20 | Sell 6 units at EUR 14. | 6 units from the 2022-03-01 purchase; 5 remain. |

2023 filing rows:

| Row | Type | Date | Quantity (F3/F7) | Unit value EUR (F4/F9) | F8 |
| --- | --- | --- | --- | --- | --- |
| 1 | Purchase | 2022-03-01 | 4 | 10 | 4 |
| 2 | Sale | 2023-05-10 | 4 | 12 | 0 |

2024 filing rows:

| Row | Type | Date | Quantity (F3/F7) | Unit value EUR (F4/F9) | F8 |
| --- | --- | --- | --- | --- | --- |
| 1 | Purchase | 2022-03-01 | 6 | 10 | 6 |
| 2 | Sale | 2024-06-20 | 6 | 14 | 0 |

Gains: 2023 is 4 × (12 − 10) = EUR 8; 2024 is 6 × (14 − 10) = EUR 24. The 2023 sale is not
repeated in 2024. The purchase of 15 is split across years as 4 + 6, and its 5 unsold units are
not listed yet: 4 + 6 + 5 = 15.

### Example: stock left after the year

| Date | Event | Internal FIFO matches |
| --- | --- | --- |
| 2024-02-01 | Buy 10 units at EUR 10. | |
| 2024-05-01 | Sell 4 units at EUR 12. | 4 units from the 2024-02-01 purchase. |
| 2024-08-01 | Buy 5 units at EUR 11. | |
| 2024-10-01 | Sell 8 units at EUR 13. | 6 units from the 2024-02-01 purchase, 2 from the 2024-08-01 purchase. |
| 2025-01-15 | Sell 3 units at EUR 15. | 3 units from the 2024-08-01 purchase. |

2024 filing rows:

| Row | Type | Date | Quantity (F3/F7) | Unit value EUR (F4/F9) | F8 |
| --- | --- | --- | --- | --- | --- |
| 1 | Purchase | 2024-02-01 | 10 | 10 | 10 |
| 2 | Sale | 2024-05-01 | 4 | 12 | 6 |
| 3 | Purchase | 2024-08-01 | 2 | 11 | 8 |
| 4 | Sale | 2024-10-01 | 8 | 13 | 0 |

Gains: 4 × (12 − 10) = EUR 8 and 6 × (13 − 10) + 2 × (13 − 11) = 18 + 4 = EUR 22, so EUR 30
in total. The 2024-02-01 purchase is one row of 10 (4 + 6) although two sales use it. The 3 units
still held on 31 December are not listed for 2024.

2025 filing rows:

| Row | Type | Date | Quantity (F3/F7) | Unit value EUR (F4/F9) | F8 |
| --- | --- | --- | --- | --- | --- |
| 1 | Purchase | 2024-08-01 | 3 | 11 | 3 |
| 2 | Sale | 2025-01-15 | 3 | 15 | 0 |

Gain: 3 × (15 − 11) = EUR 12.

### Example: a split before the sale

| Date | Event | Internal FIFO matches |
| --- | --- | --- |
| 2024-03-01 | Buy 10 units at EUR 100. | |
| 2024-06-03 | Split 2 : 1 (confirmed): 20 units, EUR 50 a unit. | |
| 2024-09-02 | Sell 20 units at EUR 60. | 20 units from the 2024-03-01 purchase. |

2024 filing rows:

| Row | Type | Date | Quantity (F3/F7) | Unit value EUR (F4/F9) | F8 |
| --- | --- | --- | --- | --- | --- |
| 1 | Purchase | 2024-03-01 | 20 | 50 | 20 |
| 2 | Sale | 2024-09-02 | 20 | 60 | 0 |

Gain: 20 × (60 − 50) = EUR 200, the same as 1,200 − 1,000 from the unsplit purchase. The purchase
row shows the units after the split, the date of the original purchase and `F4` = EUR 1,000 / 20.
There is no row for the split.

### Example: one purchase sold before and after a split

| Date | Event | Internal FIFO matches |
| --- | --- | --- |
| 2023-05-02 | Buy 10 units at EUR 100. | |
| 2024-02-01 | Sell 4 units at EUR 120. | 4 units from the 2023-05-02 purchase (old units). |
| 2024-06-03 | Split 2 : 1 (confirmed): the 6 left become 12 units at EUR 50. | |
| 2024-09-02 | Sell 12 units at EUR 55. | 12 units from the 2023-05-02 purchase (new units). |

2024 filing rows:

| Row | Type | Date | Quantity (F3/F7) | Unit value EUR (F4/F9) | F8 |
| --- | --- | --- | --- | --- | --- |
| 1 | Purchase | 2023-05-02 | 4 | 100 | 4 |
| 2 | Purchase | 2023-05-02 | 12 | 50 | 16 |
| 3 | Sale | 2024-02-01 | 4 | 120 | 12 |
| 4 | Sale | 2024-09-02 | 12 | 55 | 0 |

Gains: 4 × (120 − 100) = EUR 80 and 12 × (55 − 50) = EUR 60. The units before the split and the
units after it are two rows of the same purchase, so a FIFO over the rows gives the same gains.
`F8` counts the listed units as they are in each row, old and new together; it is a running total
of the rows, not a number of the same shares.

### Example: a loss blocked in part

FURS example 10 ([FURS examples](#furs-examples)) a year later, with prices. Selected year 2024:

| Date | Event | Internal FIFO matches |
| --- | --- | --- |
| 2023-02-01 | Buy 50 units at EUR 20. | |
| 2023-12-12 | Buy 10 units at EUR 20. | |
| 2024-01-05 | Sell 40 units at EUR 10. | 40 units from the 2023-02-01 purchase. |

All 40 units are sold at a loss. The 2023-12-12 purchase blocks 10 of them; after the user answers
that nothing else was bought or sold, the other 30 keep their loss (*NE-10 DA-30*). 2024 filing
rows:

| Row | Type | Date | Quantity (F3/F7) | Unit value EUR (F4/F9) | F8 | F10 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | Purchase | 2023-02-01 | 40 | 20 | 40 | |
| 2 | Sale | 2024-01-05 | 10 | 10 | 30 | false |
| 3 | Sale | 2024-01-05 | 30 | 10 | 0 | true |

Losses: 10 × (10 − 20) = −EUR 100 (blocked) and 30 × (10 − 20) = −EUR 300. The two sale rows have
the same date and `F9`, and 10 + 30 is the sold 40. The source sale, the total units and `F8` do
not change.

The blocked units are loss units only, taken in FIFO order, and the rows follow FIFO order. When
a sale first uses profitable units and then loss units, the profitable row comes first. Example:
10 units bought at EUR 10 and 10 at EUR 20, 15 sold at EUR 18 on 2024-03-11, 8 units bought on
2024-03-20. The 5 units from the EUR 20 purchase are the loss (−EUR 10) and the 8 new units block
all 5. Rows: purchase 10 at EUR 10 (F8 10); purchase 5 at EUR 20 (F8 15); sale 10 at EUR 18,
`F10` true (F8 5); sale 5 at EUR 18, `F10` false (F8 0).

## Currency conversion and fees

Foreign values are converted separately on the tax date that applies to them. By default, the
frontend asks the user for the official Banka Slovenije rate in the form `1 EUR = rate in the
foreign currency`. One official rate is requested for each required currency and date, then reused
for all matching instruments and brokers in that report run.

The user may explicitly choose a broker rate after a prominent warning that it is probably not the
official rate required by FURS. Broker rates remain event-specific and are never presented as
official rates.

Broker fees are shown only as selected-year information, grouped by broker and original currency.
They use a broker-provided conversion rate when one is available and never ask the user for an
official rate. A broker or overall EUR total is shown only when all fees included in that total
can be converted. Fees never change a FURS purchase value, sale value, income amount, or XML
field. The exact rules and error scopes are in [`calculations.md`](calculations.md).

## Long-held investments

Slovenian rules use these holding periods. The **sale date** decides, not the purchase date:
ZDoh-2Z (*Uradni list RS*, no. 39/2022, in use from 1 January 2022) changed Article 96 of ZDoh-2
from 20 to 15 years, and the FURS instructions apply the 20 years to a sale before 1 January 2022
whenever the units were bought.

| Sale date | Holding period for the tax exemption |
| --- | --- |
| 1 January 2022 and later | 15 years from the purchase date |
| 31 December 2021 and earlier | 20 years from the purchase date |

The period ends on the anniversary of the purchase date. A sale on the anniversary is still inside
the period, and a sale on the next day is past it. For example, units bought on 2009-03-10 are
inside the 15 years on 2024-03-10 and past them on 2024-03-11. A purchase on 29 February has its
anniversary on 28 February in a year without that day. The app only reports the rows; FURS works
out the tax from the dates. The boundary decides only which units the app leaves out of the rows
(see below). eDavki imports the rows on both sides of each boundary as a draft (scenarios S3, S4,
P1 and P2 in [`verification.md`](verification.md), 2026-10-27), but a draft does not show whether
a row is taxed, so the boundary itself rests on the FURS wording.

Example of the sale date rule: units bought on 2004-05-01 and sold on 2021-06-01 are held 17
years, which is inside the 20 years, so the sale is taxed. The same units sold on 2022-01-03 are
past the 15 years that apply to a sale from 2022, so the sale is exempt.

Official eDavki guidance says that a Doh-KDVP return is not required for capital sold after the
applicable holding period. Section 11 of the instructions limits the *popisni list* to securities
sold "*pred potekom petnajstih let od dneva, ko so bili pridobljeni*" (before 15 years from the day
they were bought).

So units past the holding period get no row at all. A sale may use both older and newer units
(FIFO takes the oldest first). The app then leaves the older units out of both the purchase rows
(`F3`) and the sale row (`F7`), so `F8` stays the running total of the listed units and never goes
below zero. A sale that uses only older units has no row, and an ISIN whose sales of the year all
use only older units has no list. The units left out stay visible beside the report, never in the
XML.

Example (sale in 2025, so 15 years): buy 2 units on 2005-04-01 and 2 units on 2020-04-01, then
sell 3 units on 2025-06-02. FIFO takes the 2 units from 2005, which are past the 15 years and tax
free, and 1 unit from 2020. The list has two rows: purchase 2020-04-01, `F3` 1, `F8` 1; sale
2025-06-02, `F7` 1, `F8` 0.

A loss on units past the period cannot reduce the tax base (ZDoh-2 Article 96, third paragraph),
so it is left out with them and the loss check covers only the listed units. Lowering `F7` below
the sold quantity of the broker confirmation imports as a draft in eDavki (scenario S2 in
[`verification.md`](verification.md), 2026-10-27); a draft does not show the calculation.

The holding period never changes:

- which FIFO shares are used by a sale; or
- how a sale containing both older and newer shares is split into FIFO matches.

## The 30-day loss rule

The Slovenian `pravilo navidezne odsvojitve` is also called the deemed-disposal, shadow-trade, or
wash-sale rule. It decides whether a reported loss may reduce taxable gains. It does not cancel the
sale or return sold shares to the ledger.

For a sale at a loss on date `D`, check the full period from 30 days before the sale through 30
days after the sale. Both end dates and the sale date are included: 61 days.

The loss cannot reduce taxable gains to the extent that the taxpayer acquires the same or
substantially identical replacement investment during that period. The same applies when the
taxpayer obtains a right or an obligation to buy it. Replacement quantities matter: a replacement
of 4 units does not automatically disallow a loss on all 10 units sold.

### How the app applies the rule

The source is the FURS guidance *Dobiček iz kapitala – pravilo navidezne odsvojitve kapitala*
(January 2015, see "Official sources") and its 23 examples. The app reads it as follows; this
reading gives the FURS result in all 23 examples (maintainer decision, 2026-10-27):

1. **FIFO first.** The FIFO matches decide the purchase value of each sale and which units are
   sold. Only the units of loss-making matches within the holding period are loss units. A
   profitable match is not checked, and a loss on units past the holding period cannot reduce the
   tax base ([Long-held investments](#long-held-investments)), so it is not checked either. A gain
   of zero is not a loss.
2. **The sale's own purchase is no replacement.** A sale's original date is the purchase date of
   its first FIFO lot. A purchase counts as a replacement only when it is dated after that date
   (FURS examples 7, 17 and 23: a quick sale of part of a new purchase keeps its loss), or when
   it comes after the sale: a rebuy later on the day of the sale counts even when the sale's
   first lot was bought that day (maintainer decision; the FURS examples have no
   times of day). A purchase
   after the original date counts with its full quantity, even when the same sale sells some of
   its units: in FURS example 2 the second sale of 50 uses 25 old and 25 new units, and the whole
   new purchase of 50 blocks its loss, because the holding is 25 before that purchase and 25 after
   the sale.
3. **A sale that empties the holding keeps its loss against earlier purchases.** When no unit is
   left right after the sale, only purchases after it count (FURS examples 14 and 18; example 9
   shows a purchase after such a sale).
4. **Earliest loss first, each unit once.** Loss sales are taken in ledger order. Each takes the
   unused units of its replacements, earliest purchase first, up to its loss units (FURS examples
   11, 16, 17 and 19). A unit used once is never used again.
5. **Across years.** Every report rebuilds the whole history, so the loss sales of earlier years
   take their replacements first, and nothing is remembered between runs. A sale after 31
   December never takes a replacement before a sale of the selected year. Purchases in the
   following January (up to 30 January) are replacement candidates only: they never enter the
   selected year's FIFO ledger, closing stock or report rows.
6. **Only the same ISIN.** Substantially identical capital under another ISIN, rights and
   obligations to buy, and purchases by related persons (a family member or a company in which
   the taxpayer has the required interest; ZDoh-2 has a separate rule for them, not limited to the
   window) are not checked and not asked (in practice only the
   same ISIN matters). Names and symbols never identify a replacement.
7. **Units only.** A replacement is counted by its units; its currency and price are not needed.
   Units are compared across every confirmed split and reverse split of the ISIN: a trade dated before the split's effective date is in the units before it, a trade on or after that date in the units after it. The holding after each sale (rule 3) is changed at each split and rounded once, as the FIFO lots are, and each replacement is counted in the units of the sale, rounded up to 8 decimals (the safe side: at most 0.00000001
   unit more is blocked). Example: 12 units bought, 10 sold at a loss on 2 May, split `2 / 1` on
   10 May, 6 units bought on 20 May: the 6 new units are 3 units of the sale, so 3 of its 10 loss
   units are blocked. A split row after 31 December inside the window of a December loss is
   asked too, and used only for this comparison when its date is after 31 December
   ([`fifo.md`](fifo.md)).
   Until such a split row has a confirmed ratio, the sale stays blocked with the error of its
   ratio question. A merger after 31 December inside the window blocks that ISIN
   (`corporate_action_not_applied`, key `.loss_window`), because the units before and after it
   cannot be compared. Known limit: a split row dated after the window end is not asked, although
   a broker may book an action up to 10 days late. A reverse split that took effect inside the
   window is then compared in the old units, which blocks too little loss (later work).

The check covers all supplied brokers and accounts. Checking only the last buy before the sale is
not enough; the legacy app's shortcut of checking only the last earlier buy must not be copied.

### The question to the user

The app sees only the supplied files. An export's first or last trade does not prove that it is
complete. When the known replacements do not block all loss units of a sale, the open units need
the user's answer. The question is as simple as possible: one
`loss_eligibility_confirmation_required` request per ISIN with every sale of the year whose loss
the files cannot decide: "Within 30 days before or after the sale, did you also buy or sell this
investment anywhere that is not in your files: at another broker, in another account, or after the
last day of your export?" A rebuy after the export date, or after a run made less than 30 days
after a December sale, is not in the files either. The frontend
and the CLI show all of them together as one checklist.

- `no_other_activity`: the open units keep their loss.
- `other_activity`: every open unit is treated as blocked (`F10` `false`). This is the safe side,
  more tax and never less; the user corrects the deemed disposal in eDavki.
- Cancel: each open sale gets a blocking `loss_eligibility_unresolved` error. When every
  other error that blocks capital gains also names its own investment, the capital exclusion
  question lists it: exclude it, or write no Doh-KDVP ([`diagnostics.md`](diagnostics.md)).

While a question is open, capital gains is `needs_input` and has no XML; unrelated reports
continue. An unanswered question is never written as `true` or `false`, and developer mode cannot
bypass it. The one exception is a caller that does not ask: the debug tools treat the open
units as if the answer were `other_activity` (`F10` `false`, the safe side) and give one
`loss_eligibility_unresolved` warning per ISIN. The CLI asks the question in the terminal. The question data (sale dates, transactions, units, windows, known replacements) is
defined in [`diagnostics.md`](diagnostics.md); a changed fact, such as a purchase added in a new
file, is a new question.

### Doh-KDVP rows

`F10` is written on every sale row: `true` when none of the row's units is a blocked loss unit
(a profitable sale included), `false` when the rule blocks the loss of all of them. The writer
does not perform the eligibility calculation.

A sale whose rule blocks only part of its listed units (for example `NE-10 DA-30`, or a blocked
loss next to profitable units of the same sale) is written as several sale rows with the same
source sale date and unit value (`F9`). The loss units of the sale, in FIFO order, take the
blocked quantity first (`F10` false); every other listed unit has `F10` true; neighbouring units
with the same `F10` are one row. The quantities add up to the listed units of the sale (`F7`
without units past the holding period), the sale is not matched twice, and `F8` stays the running
total of the rows. Examples: [A loss blocked in part](#example-a-loss-blocked-in-part).

### Evidence retained after exclusions

Excluding an ISIN or classified item removes its report rows, not its trades from the loss
analysis. Because only the same ISIN is a replacement, excluding another ISIN never changes a loss.
Exclusion neither restores replacement quantities already allocated to an earlier loss nor makes
unreliable facts usable; the capital exclusion flow (#227) keeps these rules.

### FURS examples

Every purchase is followed by its quantity, every sale (O) by its quantity and the FURS result:
`NE` units whose loss is blocked, `DA` units whose loss may reduce gains (with no other
activity). The synthetic processor tests reproduce each one (`tests/unit/loss_eligibility_test.cpp`).

| Example | History (2007 and 2008) | FURS result |
| --- | --- | --- |
| 1 | 12.12. 50; O 5.1. 50; 7.7. 50; O 14.7. 50 | DA-50; DA-50 |
| 2 | 12.12. 50; O 5.1. 25; 7.7. 50; O 14.7. 50 | DA-25; NE-50 |
| 3 | 12.12. 50; O 5.1. 25; 31.1. 20; 7.7. 50; O 14.7. 50 | NE-20 DA-5; NE-50 |
| 4 | 12.12. 50; 5.1. 50; 5.2. 50; 5.3. 50; O 15.4. 100; 5.5. 20 | NE-20 DA-80 |
| 5 | 1.1. 100; 1.4. 100; O 15.4. 100 | NE-100 |
| 6 | 12.12. 50; O 5.1. 50 | DA-50 |
| 7 | 12.12. 50; O 5.1. 25 | DA-25 |
| 8 | 12.12. 50; O 5.1. 25; 31.1. 50 | NE-25 |
| 9 | 12.12. 50; O 5.1. 50; 31.1. 50 | NE-50 |
| 10 | 1.2.2007 50; 12.12. 10; O 5.1. 40 | NE-10 DA-30 |
| 11 | 1.1. 200; O 10.3. 100; O 15.3. 100; 20.3. 100 | NE-100; DA-100 |
| 12 | 1.1. 200; O 10.3. 200; 15.3. 100 | NE-100 DA-100 |
| 13 | 1.1. 50; 31.3. 50; 2.4. 50; O 10.4. 50 | NE-50 |
| 14 | 1.9.2007 100; 17.9.2007 100; 18.9.2007 100; 4.2. 100; 5.2. 100; O 5.2. 500 | DA-500 |
| 15 | 24.9.2007 100; 18.12. 50; 26.12. 25; O 6.1. 100 | NE-75 DA-25 |
| 16 | 24.9.2008 600, 350, 50, 500; O 15.11. 600; O 19.11. 350; O 19.11. 50; O 21.11. 500; 5.12. 550; O 12.12. 550 | NE-550 DA-50; DA-350; DA-50; DA-500; DA-550 |
| 17 | 29.5. 10, 4, 10; O 18.6. 22; O 10.7. 2; 16.7. 3; 19.7. 5; O 6.8. 8; 8.8. 10; O 18.9. 10 | NE-3 DA-19; NE-2; NE-8; DA-10 |
| 18 | 25.7. 120; 26.7. 20; 2.8. 400; 2.8. 400; 8.8. 600; O 17.8. 1540 | DA-1540 |
| 19 | 1.2. 500; O 10.3. 100; O 15.3. 100; 20.3. 100 | NE-100; DA-100 |
| 20 | 24.9.2007 100; O 3.2. 100; 10.2., 11.2., 12.2., 13.2. 50 each | NE-100 |
| 21 | 24.9.2007 100; O 3.2. 100; 10.2., 11.2., 12.2., 13.2. 10 each | NE-40 DA-60 |
| 22 | 1.2. 100; O 3.2. 100; 10.4. 10 | DA-100 |
| 23 | 1.2. 100; O 3.2. 30; 10.4. 10 | DA-30 |

Dates are day and month; December dates are in 2007 and the others in 2008 unless a year is
given. FURS example 13 prints a stock of 10 after its second purchase; it is 100.

### Quantitative and year-boundary examples

All examples assume a loss, the same investment, and the answer `no_other_activity` where a
question is asked:

| History | Result |
| --- | --- |
| Buy 50 on 12 December 2023; sell all 50 on 5 January 2024; no replacement. | All 50 units remain eligible despite the recent acquisition; FURS example 1 covers complete liquidation. |
| Buy 100 in September 2023; sell 4 on 20 December 2024 at a loss; buy 4 on 5 January 2025. | All 4 replacement units are ineligible, without a question, and the sale's 2024 closing FIFO position remains unchanged by the January buy. |
| Buy 50 earlier and 10 on 12 December 2023; sell 40 on 5 January 2024 leaving 20. | Apply FURS example 10: 10 units are ineligible and 30 eligible: two sale rows ([A loss blocked in part](#example-a-loss-blocked-in-part)). |
| Buy 200 earlier; sell 100 on 10 March and 100 on 15 March; buy 100 on 20 March. | The first loss uses the 100 replacement units; the second cannot reuse them (FURS example 11). |
| Sell 10 at a loss on 20 December 2023; buy 10 on 5 January 2024; sell 10 at a loss on 10 January 2024. | The December loss takes the January purchase, in the 2023 report and again when the 2024 report rebuilds the history; the January loss has no known replacement. |

The legacy app is used only to confirm the meaning of the Doh-KDVP field: `true` means that the
loss may reduce taxable gains.

## Empty selected years

A valid year with no reportable results is successful and returns an empty result. The app must
not create placeholder events or copy events from another year.

Examples include:

- no supplied event happened in the selected year;
- the selected year contains only buys or corporate actions; or
- all supplied events happened after the selected year.

Older events are still processed when they are needed to build the ledger. Missing required
history or another processing error is still an error. An empty year alone is not an error.

When there are no reportable capital sales and no unresolved blocking problems, capital gains
returns `no_data`, with reason `no_selected_year_disposals`, and no XML. The later GUI explains:
"No Doh-KDVP return is needed for [year] based on the supplied data." Purchases and supported
corporate actions alone do not require this return. Dividend and interest results remain independent.
The absence of source sales is a preparation fact, not proof that rejected or unclassified events
are harmless; later processing must resolve their scope before assigning the final report status.

## Files covering several years

One imported file may contain events from before, during, and after the selected year. The same
history may also be spread across several files.

File boundaries have no tax meaning. The app must first merge and deduplicate the events, then
apply the reporting-year rules. The result must be the same whether the history came from one file
or several files, and regardless of the order in which parsers finished.

A file must not be rejected or skipped only because its first or last event is outside the
selected year.

## Example across several years

Assume the selected year is 2024 and all rows refer to the same investment.

| Date | Event | Ledger result | 2024 report |
| --- | --- | --- | --- |
| 2008-05-10 | Buy 10 shares at EUR 5. | Hold 10 shares bought in 2008. | No row of its own; see the 2024-06-20 sale. |
| 2023-03-01 | Sell 4 shares at EUR 6. | Use 4 shares from the 2008 buy; 6 remain. | Nothing because the sale was before 2024. |
| 2023-09-01 | Buy 8 shares at EUR 40. | Hold 6 older and 8 newer shares. | No row of its own; see the 2024 sales. |
| 2024-02-01 | Trade Republic records a split. Its announcement says 2 new shares for 1 old share. | The user confirms `2 / 1`; hold 12 older and 16 newer adjusted shares (EUR 20 a share). | Nothing. |
| 2024-06-20 | Sell 20 shares at EUR 25. | FIFO uses 12 older and 8 newer shares; 8 newer shares remain. | One sale row of 8 (`F9` 25). The 12 shares from 2008 are past 15 years: no row, listed beside the report. |
| 2024-12-20 | Sell 4 shares at EUR 15. | Use 4 newer shares (a loss of 4 × (15 − 20) = EUR 20); 4 remain at year-end. | One sale row of 4 (`F9` 15), `F10` false: the January replacement buy means it cannot reduce taxable gains. |
| 2025-01-05 | Buy 4 shares of the same investment. | Check it only for the 30-day loss rule in the 2024 run. | Nothing. |
| 2025-02-10 | Sell 2 shares. | Ignore it in the 2024 run. | Nothing. |

2024 filing rows:

| Row | Type | Date | Quantity (F3/F7) | Unit value EUR (F4/F9) | F8 | F10 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | Purchase | 2023-09-01 | 12 | 20 | 12 | |
| 2 | Sale | 2024-06-20 | 8 | 25 | 4 | true |
| 3 | Sale | 2024-12-20 | 4 | 15 | 0 | false |

The purchase row lists the 12 shares the two sales use (8 + 4) after the split, at EUR 40 / 2 =
EUR 20. Gains: 8 × (25 − 20) = EUR 40 and 4 × (15 − 20) = −EUR 20.

The 2024 closing position is 4 shares. A separate 2025 report starts again from the full history
and processes the January buy and February sale normally.

## Date-boundary examples

For a 2024 report:

- 31 December 2023 is processed as older history and is not reported;
- 1 January 2024 is processed and may be reported;
- 31 December 2024 is processed and may be reported;
- 1 January 2025 is not added to the 2024 ledger or report; and
- a qualifying January 2025 buy may still be checked for a December 2024 loss when it is within
  the 30-day period.

## Supported tax categories and user classification

These rules cover a Slovenian-resident individual using ordinary TR or IBKR brokerage accounts.
Business activity, individual investment accounts (INR), and Slovenian retail government bonds
are outside the current import scope. The latter are not part of the supported TR/IBKR workflow;
do not add their special allowance or a dedicated input flow to this implementation. If such data
appears, preserve it, explain the missing support, and request classification or explicit exclusion
rather than treating it as an ordinary bond coupon.

Classify the legal instrument and income, not just the broker name or its CSV category:

| Category | Processing and output |
| --- | --- |
| Shares, investment-fund units, and other supported ownership capital | Historical FIFO and selected-year disposals in Doh-KDVP. |
| Dividends | Selected-year gross income and foreign tax in Doh-Div, subject to the settled Slovenian withholding rule below. |
| Qualifying Slovenian/EU bank-deposit interest | Shared annual allowance and Doh-DHO when a return is required. |
| IBKR cash interest | Other-interest category in Doh-Obr; no bank-deposit allowance. |
| Ordinary bond coupons | Bond-interest category in Doh-Obr, subject to the settled Slovenian withholding rule below; separate from broker cash interest in the model and summary. |
| Ordinary debt-security disposals | No taxable capital gain or deductible capital loss in Doh-KDVP; retain the source and explain the exclusion. |
| Discount or zero-coupon debt | Interest may arise on disposal or redemption; require instrument terms and acquisition data. Until these calculations are implemented, return `unsupported_tax_treatment` rather than silently excluding possible interest. |
| Ordinary private crypto disposals | Exclude from the supported capital return with a year-specific explanation based on verified FURS guidance; business activity and tokens qualifying as securities or derivatives are not covered by this exclusion. |
| Derivatives | Different tax-return rules; `unsupported_tax_treatment` until a dedicated processor and form are implemented. |
| Unknown instruments, private-market events, and benefits | Ask for classification and required facts; cash movements alone do not establish a purchase quantity or cost basis. |

Known broker facts may suggest a classification but cannot resolve contradictory source evidence.
Import retains otherwise valid trade/action facts when a broker class is missing, unrecognized or
contradictory. Keep every original class label and source; file boundaries and row order cannot
choose the tax treatment. The instrument stays unresolved until the classification stage resolves
it. This preservation does not relax required ISIN, numeric or date validation, or the separate
whole-group rejection rule for conflicting broker transaction identities.

A missing display name does not block tax reporting for a valid ISIN-identified event. Retain the
original blank and warn; presentation uses the first non-empty supplied name for that same ISIN
under the documented source ordering. If no name is available, leave the name empty.

A `tax_classification_required` request identifies the investment or standalone event, all relevant
sources, proposed treatment if justified, and `missingFields`. Offer `capital_gains`, `dividends`,
`bank_deposit_interest`, `other_interest`, or `exclude_from_supported_reports`. Other interest also
requires a subtype such as `broker_cash` or `bond_coupon`. No option is preselected for an unknown
instrument. Classification does not let the user choose an arbitrary tax rate or claim an exemption.

The missing-field list is specific to the selected treatment: for capital it includes an ISIN,
acquisition/disposal dates, units and unit values/currencies; for income it includes gross income,
receipt date, foreign tax and its currency, payer identity and country, and source country. A known
zero foreign tax is valid; an unknown tax amount is not silently set to zero. Bond treatment also
requires issuer/instrument identity and coupon-versus-discount information. Unsupported required
fields or calculations remain visible even after classification.

Apply classification to the complete instrument history only when the confirmed treatment is the
same; heterogeneous benefits or income events require event-specific choices. Store the decision
and original classification in the current run. Explicit exclusions stay visible in a separate
`excludedItems` list with sources, category and reason; they never masquerade as a processed event.
This list is distinct from per-report capital and other-interest `excludedInvestments`. A known outside-scope category gives a
warning without blocking unrelated reports. Unanswered unknown classifications leave every
potentially affected report `needs_input`. Cancellation alone is not an exclusion or a successful
empty result.

## Payer lists, settled Slovenian tax and foreign-tax relief

Maintain a reference list of Slovenian companies and a company/ETF catalogue imported from the
user-designated other project. Include every reusable entry from that catalogue, but do not claim
that it covers every issuer or ETF in existence. Record the upstream project, revision, licence,
original entry and verification date. The upstream URL/path must be identified before importing;
no matching catalogue has been located in the current legacy tree. This is a data-import task,
not a reason to infer missing tax facts. Ship a standalone, versioned resource with appropriate
attribution; processing must work offline without the other project or the legacy tree.

Entries associate exact instrument identifiers with legal payer name, address, tax/registration
identifier when available, residence country, instrument kind and effective dates. Keep fund/share
class identity for ETFs. Store verified source-country and treaty references separately, with their
own sources and effective dates. Match exact identifiers; names, exchange location, trading
currency, broker country and an ISIN prefix alone do not prove payer residence or tax treatment.
Conflicting entries, entries not verified for the payment date, or absent entries require
`income_details_required` when needed report facts cannot be established from other reliable sources.
A catalogue entry may prefill verified metadata, but never overrides contradictory payment evidence.

For dividends and other interest, distinguish foreign tax from Slovenian final withholding.
When payment evidence establishes that a Slovenian withholding agent has settled the final
Slovenian tax and no recipient return is required under the applicable rule, omit that payment
from Doh-Div or Doh-Obr. Retain it in `excludedItems` with reason `slovenian_tax_settled`, its
evidence, and `tax_category_not_reported`. A Slovenian-company list match alone is insufficient.
Unknown or contradictory withholding status produces `income_details_required` and `needs_input`
for that income form. Cancellation does not resolve the missing facts; a safely isolated Doh-Obr
ISIN may use the explicit other-interest exclusion/failure choice in `diagnostics.md`.
Explicit classification exclusion also remains available under its existing rules.
This exception does not remove the payer's capital trades or
replace the separate Doh-DHO deposit-interest threshold.

For reportable foreign income, retain the gross payment and actual foreign tax separately. A
company or ETF list helps prepare a foreign-tax relief claim; membership does not establish relief.
Record the withholding country, amount/currency, payment evidence and any applicable verified
treaty provision. Do not replace actual tax with a catalogue percentage, subtract it from gross
income, or describe the entire payment as exempt merely because a tax treaty exists. FURS decides
the allowable credit. Fund-level withholding is not automatically tax paid by the investor.

A foreign-tax credit and a treaty exemption are separate treatments. Populate foreign-tax fields
from verified payment facts. Populate an exemption claim only with a confirmed applicable legal
basis, including the required treaty article/paragraph for the income date; otherwise leave the
optional exemption field absent. A requested but unsupported exemption returns
`income_details_required`; the user may supply the basis or explicitly withdraw that claim.
The catalogue cannot preselect an exemption. Apply each form's verified instructions and XSD.

The result lists the evidence the user must attach when filing a foreign-tax claim in eDavki.
XML generation does not mean evidence was attached or a return was filed. Missing facts needed
to establish an amount or claim block that form through `income_details_required`; the separate
task of attaching already identified evidence does not block a valid XML download. Retain all
confirmed metadata and treatment decisions as recorded answers while the app is open (see
`diagnostics.md`, "Structured user-decision contract"); a changed source fact or reference-data
revision makes a different question that needs a new answer.

## Interest forms and the annual deposit allowance

Generate one result per FURS form: Doh-KDVP, Doh-Div, Doh-DHO and Doh-Obr. The last two are
independent. Ordinary bond coupons and broker cash interest have distinct report-model subtypes
but share one Doh-Obr document, using the appropriate interest-type codes. Do not create two
independent ordinary-interest returns for the same taxpayer and year simply to separate subtypes.

If a bond-income error can be isolated safely to a complete ISIN, prompt the user to exclude all
listed failed ISINs from Doh-Obr or block Doh-Obr, following
`other_interest_failure_resolution_required` in `diagnostics.md`. Before the answer, other interest
is `needs_input`. Exclusion removes all selected-year Doh-Obr income for those ISINs across brokers,
preserving valid income from other instruments and broker cash interest. Record the excluded names,
ISINs and reasons beside the result; use `no_data` if no rows remain. Choosing to block the form
produces `failed` with no XML. Neither choice changes Doh-DHO, Doh-Div, Doh-KDVP or source evidence.
Missing identity, non-isolatable errors and final XML failures retain their normal error scope.

FURS guidance treats IBKR cash interest as other interest. Trade Republic interest received from
6 December 2023 is included in the qualifying bank-deposit total; earlier TR interest belongs in
Doh-Obr. Mixed or unverified product data requires classification rather than a blanket broker-wide
rule. When an export crosses the licence change and does not distinguish the relevant amounts,
request the source breakdown instead of prorating it. The income receipt date determines the year;
interest credited in January is not silently moved into December because it accrued then.

For years from 2022, the ordinary interest rate is 25%; for 2020 and 2021 it is 27.5%; earlier
supported years use their verified rule. These rates explain treatment, not amounts deducted from
the gross income in XML. FURS assesses the final tax.

Aggregate all imported qualifying deposit interest for the selected year in EUR after the
conversion rules in `calculations.md`. The allowance is EUR 1,000 across qualifying banks, not per
broker, account, or payment. If the known qualifying total is at or below EUR 1,000, prepare no
Doh-DHO from that data and return `no_data` with reason `below_deposit_interest_threshold`, retain
the informational total, and show `deposit_interest_external_total_unchecked` until the user confirms
whether there is additional qualifying interest elsewhere. This reminder does not block downloads.

The frontend explains: "Imported qualifying interest is EUR [amount]. Include qualifying interest
from your other Slovenian/EU bank accounts when checking the EUR 1,000 annual limit. This imported
total alone does not establish that you have no filing obligation." Offer entry of external
interest or confirmation that there is none. A confirmed external amount is included
once, with a manual-source reference and enough payer details for the return. If the known combined
total exceeds EUR 1,000, generate Doh-DHO with all reportable qualifying amounts, not just the excess;
FURS applies the allowance. Missing external payer details produce `income_details_required` and
`needs_input`, rather than a complete-looking XML containing only imported interest.

Examples: TR EUR 900 with no other qualifying interest gives no Doh-DHO; TR EUR 900 plus another
EU bank's EUR 200 requires a return reporting EUR 1,100; exactly EUR 1,000 does not require a return.
IBKR cash interest of EUR 20 belongs in Doh-Obr independently of those totals. Ordinary foreign
bond coupons do not use the deposit allowance.

Doh-DHO can use verified legacy behavior as a reference. Doh-Obr, including bond coupons, is new
work. Start with synthetic broker-neutral fixtures, official form instructions and the applicable
XSD. Track real broker-export and eDavki acceptance verification separately when no live bond
example is available; passing synthetic tests must not be described as live verification.

## Required processor tests

Tests based on this document must prove that:

- the same rules work for different valid selected years;
- an invalid year fails before any processing starts;
- the tax date, not the broker timestamp, controls the year;
- 1 January and 31 December are included;
- the following 1 January is excluded;
- older buys and sales create the correct opening FIFO position;
- corporate actions before and during the year are applied;
- corporate actions after the year do not change its FIFO ledger; relevant actions still enter
  the separate loss analysis and its multiple-source warning;
- a supported corporate action is applied before same-day trades for the same ISIN;
- corporate-action errors leave the affected ISIN unprocessed without blocking unrelated reports;
- an official exchange rate is requested once and reused only for the same currency and tax date;
- choosing a broker rate requires the documented warning and keeps a visible warning in the result;
- broker fees use available broker rates without a prompt, otherwise remain in their original
  currency, and never change FURS values;
- the Doh-KDVP filing rows of the three examples in [Doh-KDVP filing rows](#doh-kdvp-filing-rows)
  match their literal rows, `F8` values and gains;
- a sale that uses several purchases is one sale row, and a purchase used by several in-year sales
  is one purchase row;
- earlier-year sales and units still held at the end of the year are not listed;
- units past the holding period are left out of `F3` and `F7`, a sale with only such units has no
  row, and they stay visible beside the report: 15 years for a sale from 2022 and 20 years
  before, counted from the purchase date, with the anniversary still inside the period and
  29 February moving to 28 February;
- a loss on units past the holding period does not block, and a loss on the listed units does;
- a loss from an earlier-year sale does not block the selected year and is not listed;
- a FIFO over the filing rows, independent of the production code, gives the literal gains and
  `F8` values;
- a lot a split or reverse split changed is listed in the units after the split, with the original
  purchase date and `F4` = exact basis over those units, rounded once to 8 decimals; a purchase
  that sales use both before and after a split has one row for each run of the same state; an
  `F4` that does not fit a `UnitPrice` blocks the ISIN;
- a sale whose loss the 30-day rule blocks only in part is several sale rows (same date and `F9`),
  the loss units taking the blocked quantity first in FIFO order, and the quantities add up to the
  listed units;
- holding time does not change FIFO use;
- replacement buys exactly 30 days before or after a loss are included in the loss check, and
  31 days are not;
- the 23 FURS loss-rule examples give their FURS results;
- a sale's own purchase is no replacement, a rebuy later on the same day is one, and a sale that
  empties the holding keeps its loss
  against the purchases before it;
- a December acquisition can affect a January loss in the next reporting year;
- a later-year replacement buy can change loss eligibility without changing the earlier ledger;
- partial replacements split eligible and ineligible quantities without duplicating a disposal;
- profitable FIFO matches are not treated as losses;
- replacement quantities are allocated chronologically and are not reused by another loss;
- complete liquidation and the FURS quantitative examples produce their documented outcomes;
- split-adjusted replacement quantities use comparable units;
- every sale row serializes explicit `F10` (`true` for a profitable sale), purchase rows have no
  `F10`, and unresolved decisions generate no capital XML;
- every broker purchase row has `F2` = `B`;
- a loss the files cannot decide produces one yes/no question per ISIN, and `yes` blocks every
  open unit;
- only the same ISIN is a replacement;
- exclusions preserve reliable replacement evidence and allocations for retained losses;
- unresolved evidence from an excluded item keeps dependent losses awaiting input and updates the
  exclusion choice when the affected ISIN list changes;
- supported income categories route to their documented forms and exclusions remain visible;
- verified settled Slovenian withholding excludes only the relevant income payment, while a
  company-list match without payment evidence does not establish settlement;
- company/ETF catalogue matches prefill verified facts, preserve provenance and work offline;
- missing, stale or conflicting catalogue facts use the income-details flow;
- foreign-tax claims preserve gross income and actual investor-level tax, require the relevant
  evidence, and never infer a treaty exemption from list membership;
- an unresolved requested exemption needs input until supported or explicitly withdrawn;
- exactly EUR 1,000 of qualifying interest does not require a return;
- external deposit interest can move an imported total above the filing threshold;
- pre-licence TR interest and IBKR cash interest do not receive the deposit allowance;
- bond coupons have their own fixtures and subtype within the shared Doh-Obr result;
- bond-income errors safely isolated to ISINs offer explicit exclusion from Doh-Obr or failure of
  that form, with independent statuses, visible exclusions and no automatic partial export;
- Slovenian retail government bonds remain explicitly outside the current import scope;
- an empty year returns empty result lists without invented data;
- one multi-year file and several equivalent files give the same result; and
- a report run does not reuse stored state from an earlier run.

## Official sources

- [ZDoh-2 in the Slovenian Legal Information System](https://pisrs.si/Pis.web/pregledPredpisa?id=ZAKO4697)
- [FURS guidance for sales of securities, interests, and investment coupons](https://www.fu.gov.si/zivljenjski_dogodki_prebivalci/odsvojil_sem_vrednostne_papirje_druge_deleze_ali_investicijske_kupone)
- [eDavki Doh-KDVP filing guidance](https://edavki.durs.si/EdavkiPortal/OpenPortal/CommonPages/Opdynp/PageD.aspx?category=vrednostni_papirji_drugi_delezi_investicijski_kuponi)
- [Doh-KDVP instructions for 2025 and later, section 11](https://edavki.durs.si/OpenPortal/Dokumenti/doh_odm_kdvp_25.n.sl.pdf)
- [Doh-KDVP instructions before 2025, section 11](https://edavki.durs.si/OpenPortal/Dokumenti/doh_odm_kdvp_20.n.sl.pdf)
- [FURS DOHKAP instructions: 15 years for a sale from 2022, 20 years before, decided by the sale date](https://edavki.durs.si/OpenPortal/Dokumenti/dohkap5.n.sl.pdf)
- [FURS explanation of ZDoh-2 Article 97, third paragraph: no carry-forward of capital losses except after a capital increase from company funds](https://www.racunovodja.com/clanki.asp?clanek=3090)
- [ZDoh-2Z, *Uradni list RS* no. 39/2022, which changed ZDoh-2 Article 96 from 20 to 15 years](https://www.racunovodstvo.net/zakonodaja/predpis/13083/zakon-o-spremembah-in-dopolnitvah-zakona-o-dohodnini-zdoh-2z)
- [eDavki explanation of the 30-day loss rule](https://edavki.durs.si/EdavkiPortal/OpenPortal/Pages/Faq/Faq.aspx?qq=ZDoh)
- [FURS detailed quantitative loss-rule examples](https://www.fu.gov.si/fileadmin/Internet/Davki_in_druge_dajatve/Podrocja/Dohodnina/Dohodek_iz_kapitala/Opis/Dobicek_iz_kapitala_-_pravilo_navidezne_odsvojitve_kapitala.docx)
- [FURS capital-income guidance, including Trade Republic and IBKR](https://www.fu.gov.si/fileadmin/Internet/Davki_in_druge_dajatve/Podrocja/Dohodnina/Dohodek_iz_kapitala/Opis/Obresti_dividende_in_dobicek_iz_kapitala.doc)
- [FURS interest guidance](https://www.fu.gov.si/zivljenjski_dogodki_prebivalci/prejel_sem_obresti)
- [FURS dividend guidance and Slovenian withholding](https://www.fu.gov.si/zivljenjski_dogodki_prebivalci/prejel_sem_dividende)
- [eDavki dividend return instructions, including foreign-tax evidence and treaty claims](https://edavki.durs.si/OpenPortal/Dokumenti/doh_odm_div.n.sl.pdf)
- [FURS international taxation and treaty sources](https://www.fu.gov.si/davki_in_druge_dajatve/podrocja/mednarodno_obdavcenje)
- [eDavki other-interest return (Doh-Obr)](https://edavki.durs.si/EdavkiPortal/OpenPortal/CommonPages/Opdynp/PageD.aspx?category=odmera_dohodnine_od_drugih_obresti)
- [FURS private crypto-trading guidance](https://www.fu.gov.si/zivljenjski_dogodki_prebivalci/trgujem_z_virtualnimi_valutami)
