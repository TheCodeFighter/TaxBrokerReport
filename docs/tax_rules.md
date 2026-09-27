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

The loss cannot reduce taxable gains to the extent that the taxpayer acquires the same or
substantially identical replacement investment during that period. The same applies when the
taxpayer obtains a right or an obligation to buy it. Replacement quantities matter: a replacement
of 4 units does not automatically disallow a loss on all 10 units sold.

Match disposals to acquisitions by FIFO first, then determine eligibility for the loss-making
matched quantities using the quantitative, chronological allocation in the FURS detailed loss-rule
guidance. Do not apply the rule to profitable matched quantities. Do not treat the acquisition
consumed by the same disposal as an additional replacement simply because it falls in the window.
FURS examples distinguish a complete liquidation with no replacement from a sale followed by a
replacement purchase. Process competing loss disposals chronologically and use each replacement
quantity at most once. Supported splits normalize compared quantities to the same share units.

The report model retains eligible and ineligible quantities separately. Split a partially eligible
disposal into report rows with the same source sale date and unit value; the quantities must add
back to the original sale and must not consume FIFO inventory twice. For loss rows, Doh-KDVP `F10`
is `true` when the loss may reduce gains and `false` when it may not. For rows without a loss,
omit this optional field. The writer does not perform the eligibility calculation.

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
rights, obligations, family-member activity, or company activity. An export's first or last trade
does not prove its coverage. For a loss not fully disallowed by known replacement activity, request
confirmation of complete history for its window and of relevant activity not represented in the
files. Same ISIN is an automatic identity match; substantially identical capital with another ISIN
requires an explicit user declaration rather than a guess from its name.

Return a `loss_eligibility_confirmation_required` request containing the affected sales, inclusive
window dates, known replacement quantities, unresolved questions, and source references. The user
can add statements, confirm complete coverage with no additional disqualifying activity, or declare
additional activity and its affected quantities. A declaration must identify the affected sale and
cannot allocate more than its remaining loss quantity. Rights, obligations, and related-party
activity must be declared explicitly; they are not inferred from trades. Do not serialize an
unanswered eligibility question as `true` or `false`. While it is unresolved, capital gains is
`needs_input`; unrelated reports continue. Cancellation produces `loss_eligibility_unresolved` and
uses the normal ISIN exclusion or report-failure choice. Developer mode cannot bypass this question.

### Evidence retained after exclusions

Excluding an ISIN or classified item removes its report rows, not its source evidence from the
loss-analysis view. Keep reliable acquisitions, disposals, action facts, and confirmed declarations
when they affect a retained sale, including declared substantially identical capital with another
ISIN. Exclusion neither restores replacement quantities already allocated to an earlier loss nor
makes unreliable facts usable. Recalculate dependent loss decisions after a correction or exclusion.

If excluded data leaves replacement identity, quantity, timing or allocation unresolved, return
`loss_eligibility_confirmation_required` for each affected retained sale. Explain the dependency
and request corrected evidence or an explicit declaration of its effect. Confirmation of complete
coverage cannot override a known unresolved conflict. Keep capital gains `needs_input` until it is
resolved; cancellation uses `loss_eligibility_unresolved` for the dependent ISIN and the normal
exclusion/failure flow. A changed affected-ISIN list invalidates the earlier exclusion choice.

Example: a retained loss of 10 units in A has a confirmed equivalent replacement of 4 units in B.
Excluding B for an unrelated historical shortage still leaves 4 units of A's loss ineligible.
If B's replacement quantity is disputed instead, A's eligibility needs input; exclusion of B
cannot make all 10 units eligible. This applies to both `excludedInvestments` and `excludedItems`.

### Quantitative and year-boundary examples

All examples assume a loss, the same investment, and no undeclared disqualifying activity:

| History | Result |
| --- | --- |
| Buy 50 on 12 December 2023; sell all 50 on 5 January 2024; no replacement. | All 50 units remain eligible despite the recent acquisition; FURS example 1 covers complete liquidation. |
| Buy 100 in September 2023; sell 4 on 20 December 2024 at a loss; buy 4 on 5 January 2025. | All 4 replacement units are ineligible and the sale's 2024 closing FIFO position remains unchanged by the January buy. |
| Buy 50 earlier and 10 on 12 December 2023; sell 40 on 5 January 2024 leaving 20. | Apply FURS example 10: 10 units are ineligible and 30 eligible. |
| Buy 200 earlier; sell 100 on 10 March and 100 on 15 March; buy 100 on 20 March. | The first loss uses the 100 replacement units; the second cannot reuse them (FURS example 11). |

The complete FURS examples, including examples 14 and 17, are required reference scenarios for
complete liquidation and competing loss windows. Preserve their chronological event tables and
expected eligible quantities in synthetic processor fixtures.

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
| 2024-02-01 | Trade Republic records a split. Its announcement says 2 new shares for 1 old share. | The user confirms `2 / 1`; hold 12 older and 16 newer adjusted shares. | Nothing. |
| 2024-06-20 | Sell 20 shares. | FIFO uses 12 older and 8 newer shares; 8 newer shares remain. | Report both matches. The older match exceeds 15 years but is still sent to FURS. |
| 2024-12-20 | Sell 4 shares at a loss. | Use 4 newer shares; 4 remain at year-end. | Report the loss, but the January replacement buy means it cannot reduce taxable gains. |
| 2025-01-05 | Buy 4 shares of the same investment. | Check it only for the 30-day loss rule in the 2024 run. | Nothing. |
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
confirmed metadata and treatment decisions for the run and invalidate affected decisions when
their source facts or reference-data revision change.

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
- FIFO matches below, at, and above a holding-period limit are all reported;
- a sale containing both older and newer shares reports every FIFO match;
- holding time does not change FIFO use or report output;
- replacement buys exactly 30 days before or after a loss are included in the loss check;
- a December acquisition can affect a January loss in the next reporting year;
- a later-year replacement buy can change loss eligibility without changing the earlier ledger;
- partial replacements split eligible and ineligible quantities without duplicating a disposal;
- profitable FIFO matches are not treated as losses;
- replacement quantities are allocated chronologically and are not reused by another loss;
- complete liquidation and the FURS quantitative examples produce their documented outcomes;
- split-adjusted replacement quantities use comparable units;
- loss rows serialize explicit `F10` eligibility and unresolved decisions generate no capital XML;
- missing coverage and non-imported activity produce a structured confirmation request;
- exclusions preserve reliable replacement evidence and allocations for retained losses, including
  declared equivalent instruments with different ISINs;
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
- [eDavki explanation of the 30-day loss rule](https://edavki.durs.si/EdavkiPortal/OpenPortal/Pages/Faq/Faq.aspx?qq=ZDoh)
- [FURS detailed quantitative loss-rule examples](https://www.fu.gov.si/fileadmin/Internet/Davki_in_druge_dajatve/Podrocja/Dohodnina/Dohodek_iz_kapitala/Opis/Dobicek_iz_kapitala_-_pravilo_navidezne_odsvojitve_kapitala.docx)
- [FURS capital-income guidance, including Trade Republic and IBKR](https://www.fu.gov.si/fileadmin/Internet/Davki_in_druge_dajatve/Podrocja/Dohodnina/Dohodek_iz_kapitala/Opis/Obresti_dividende_in_dobicek_iz_kapitala.doc)
- [FURS interest guidance](https://www.fu.gov.si/zivljenjski_dogodki_prebivalci/prejel_sem_obresti)
- [FURS dividend guidance and Slovenian withholding](https://www.fu.gov.si/zivljenjski_dogodki_prebivalci/prejel_sem_dividende)
- [eDavki dividend return instructions, including foreign-tax evidence and treaty claims](https://edavki.durs.si/OpenPortal/Dokumenti/doh_odm_div.n.sl.pdf)
- [FURS international taxation and treaty sources](https://www.fu.gov.si/davki_in_druge_dajatve/podrocja/mednarodno_obdavcenje)
- [eDavki other-interest return (Doh-Obr)](https://edavki.durs.si/EdavkiPortal/OpenPortal/CommonPages/Opdynp/PageD.aspx?category=odmera_dohodnine_od_drugih_obresti)
- [FURS private crypto-trading guidance](https://www.fu.gov.si/zivljenjski_dogodki_prebivalci/trgujem_z_virtualnimi_valutami)
