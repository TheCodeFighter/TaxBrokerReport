# Reporting year and trade history (capital gains)

## What this document explains

The user chooses one calendar year for the tax report. Only results from that year appear in the
report, but the app must still read older trades to calculate those results correctly.

The app first combines the imported files, removes duplicates, and sorts all events as described
in [`architecture.md`](architecture.md). Detailed FIFO and corporate-action rules belong in
[`fifo.md`](fifo.md).

## Useful terms

- **Selected year**: the calendar year chosen by the user.
- **Tax date**: the date that decides which tax year an event belongs to. In the code, this is
  `EventMetadata::mTaxDate`.
- **Ledger**: the full ordered history of buys, sales, and corporate actions.
- **FIFO**: shares bought first are treated as sold first.
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

The app must accept any valid calendar year. It must reject an invalid year before processing
starts. It must not silently replace an invalid year with the current year or another value.

## How the report is built

Every report run starts with an empty ledger:

1. Combine, deduplicate, and sort all imported events.
2. Process every buy, sale, and relevant corporate action up to the end of the selected year.
3. Use older buys and sales to build the correct FIFO position.
4. Add a result to the report only when the event that created it happened in the selected year.
5. Include every in-year capital sale for each processable ISIN, even when its holding period may
   make it tax-exempt. [`fifo.md`](fifo.md) defines the explicit incomplete-history exception.

Each report run rebuilds its own ledger. Running a report for one year must not change the result
of a later run for another year.

| Event date | What the app does |
| --- | --- |
| Before the selected year | Process it to build the correct history, but do not report its result. |
| During the selected year | Process it and report any required result. |
| After the selected year | Do not add it to the ledger or report it. |

There is one limited exception for a sale at a loss near the end of the year. A later buy may need
to be checked under the 30-day loss rule. The later buy is used only for that check; it is not
added to the selected year's ledger or report.

## Older trades and corporate actions

All supplied buys and sales before the selected year are part of the history. An older sale uses
up FIFO shares even though that sale does not appear in the selected-year report.

Corporate actions before or during the selected year are applied at the correct point in time.
Corporate actions after the selected year do not affect that report run. The corporate-action
rules decide how quantities, values, and purchase dates change.

Dividends and interest do not create FIFO shares. They can appear in their reports only when their
tax date is inside the selected year.

## Long-held investments

Slovenian rules use these holding periods:

| Year of sale | Holding period for the tax exemption |
| --- | --- |
| 2022 and later | 15 completed calendar years |
| 2021 and earlier | 20 completed calendar years |

Official eDavki guidance says that a Doh-KDVP return is not required for capital sold after the
applicable holding period. TaxBrokerReport does not remove these sales. It reports every in-year
FIFO match and includes its purchase and sale information, so FURS can decide the final tax
treatment.

The holding period must therefore never change:

- which FIFO shares are used by a sale;
- which matched shares are included in the report; or
- how a sale containing both older and newer shares is split into FIFO matches.

## The 30-day loss rule

The Slovenian `pravilo navidezne odsvojitve` is also called the deemed-disposal, shadow-trade, or
wash-sale rule. It decides whether a reported loss may reduce taxable gains. It does not cancel the
sale or return sold shares to the ledger.

For a sale at a loss on date `D`, check the full period from 30 days before the sale through 30
days after the sale. Both end dates and the sale date are included.

The loss cannot reduce taxable gains when the taxpayer buys the same or substantially identical
replacement investment during that period. The same applies when the taxpayer obtains a right or
an obligation to buy it.

The check must cover all supplied brokers and accounts. Checking only the last buy before the sale
is not enough.

A replacement buy can happen in January after a December sale. In that case, the January buy:

- may change whether the December loss can reduce taxable gains;
- must not change the earlier year's closing FIFO position; and
- must not appear in the earlier year's report.

The law also has a separate rule for purchases by a family member or by a company in which the
taxpayer has the required ownership or voting interest. This separate rule is not limited to the
taxpayer's 30-day purchase window.

The app can check only events present in the supplied data. It must not invent missing purchases,
rights, obligations, family-member activity, or company activity. When the available data cannot
prove that a loss is eligible, the app must make that limitation clear.

The legacy app is used only to confirm the meaning of the Doh-KDVP field: `true` means that the
loss may reduce taxable gains. Its shortcut of checking only the last earlier buy must not be
copied.

## Empty selected years

A valid year with no reportable results is successful and returns an empty result. The app must
not create placeholder events or copy events from another year.

Examples include:

- no supplied event happened in the selected year;
- the selected year contains only buys or corporate actions; or
- all supplied events happened after the selected year.

Older events are still processed when they are needed to build the ledger. Missing required
history or another processing error is still an error. An empty year alone is not an error.

## Files covering several years

One imported file may contain events from before, during, and after the selected year. The same
history may also be spread across several files.

File boundaries have no tax meaning. The app must first merge and deduplicate the events, then
apply the reporting-year rules. The result must be the same whether the history came from one file
or several files, and regardless of the order in which parsers finished.

A file must not be rejected or skipped only because its first or last event is outside the
selected year.

## Example across several years

Assume the selected year is 2024 and all rows refer to the same investment. Prices are left out
because this example is about dates, FIFO shares, and report output.

| Date | Event | Ledger result | 2024 report |
| --- | --- | --- | --- |
| 2008-05-10 | Buy 10 shares. | Hold 10 shares bought in 2008. | Nothing. |
| 2023-03-01 | Sell 4 shares. | Use 4 shares from the 2008 buy; 6 remain. | Nothing because the sale was before 2024. |
| 2023-09-01 | Buy 8 shares. | Hold 6 older and 8 newer shares. | Nothing. |
| 2024-02-01 | Two-for-one split. | Hold 12 older and 16 newer adjusted shares. | Nothing. |
| 2024-06-20 | Sell 20 shares. | FIFO uses 12 older and 8 newer shares; 8 newer shares remain. | Report both matches. The older match exceeds 15 years but is still sent to FURS. |
| 2024-12-20 | Sell 4 shares at a loss. | Use 4 newer shares; 4 remain at year-end. | Report the loss, but the January replacement buy means it cannot reduce taxable gains. |
| 2025-01-05 | Buy the same investment. | Check it only for the 30-day loss rule in the 2024 run. | Nothing. |
| 2025-02-10 | Sell 2 shares. | Ignore it in the 2024 run. | Nothing. |

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

## Required processor tests

Tests based on this document must prove that:

- the same rules work for different valid selected years;
- an invalid year fails before any processing starts;
- the tax date, not the broker timestamp, controls the year;
- 1 January and 31 December are included;
- the following 1 January is excluded;
- older buys and sales create the correct opening FIFO position;
- corporate actions before and during the year are applied;
- corporate actions after the year are ignored;
- FIFO matches below, at, and above a holding-period limit are all reported;
- a sale containing both older and newer shares reports every FIFO match;
- holding time does not change FIFO use or report output;
- replacement buys exactly 30 days before or after a loss are included in the loss check;
- a later-year replacement buy can change loss eligibility without changing the earlier ledger;
- an empty year returns empty result lists without invented data;
- one multi-year file and several equivalent files give the same result; and
- a report run does not reuse stored state from an earlier run.

## Official sources

- [ZDoh-2 in the Slovenian Legal Information System](https://pisrs.si/Pis.web/pregledPredpisa?id=ZAKO4697)
- [FURS guidance for sales of securities, interests, and investment coupons](https://www.fu.gov.si/zivljenjski_dogodki_prebivalci/odsvojil_sem_vrednostne_papirje_druge_deleze_ali_investicijske_kupone)
- [eDavki Doh-KDVP filing guidance](https://edavki.durs.si/EdavkiPortal/OpenPortal/CommonPages/Opdynp/PageD.aspx?category=vrednostni_papirji_drugi_delezi_investicijski_kuponi)
- [eDavki explanation of the 30-day loss rule](https://edavki.durs.si/EdavkiPortal/OpenPortal/Pages/Faq/Faq.aspx?qq=ZDoh)
