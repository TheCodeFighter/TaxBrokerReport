# Pipeline diagnostics and independent XML results

## What this document explains

This document defines how the application reports warnings, errors, questions that need a user
answer, and the result of each FURS XML file. It covers the complete run from parsing to XML
generation.

Parser details and event provenance are defined in [`architecture.md`](architecture.md). The rules
that can produce later diagnostics are defined in [`tax_rules.md`](tax_rules.md),
[`fifo.md`](fifo.md), and [`calculations.md`](calculations.md).

## Main rule

Capital-gains, dividend, bank-deposit-interest, and other-interest reports are independent outputs. A problem blocks only the
report that needs the affected data.

The application must continue processing unrelated data after an error when it can do so without
guessing. A successful XML file remains available even when another file needs user input or
fails.

No error may be handled by inventing a value, silently dropping an event, or showing a successful
status for an incomplete result. The documented capital and other-interest ISIN exclusion flows
below, and the capital-only developer-mode flow in [`fifo.md`](fifo.md), are explicit user decisions,
not silent recovery.

## Pipeline stages

Every application diagnostic has exactly one stage:

| Stage | What it covers |
| --- | --- |
| `parsing` | Reading one broker file and converting its rows into broker-neutral events. |
| `merging` | Combining files, detecting duplicate transactions, and rejecting conflicts. |
| `historical_validation` | Validating the reporting year and the supplied history needed to build the ledger. |
| `corporate_action` | Validating and applying splits, reverse splits, and other action rules. |
| `fifo_tax` | FIFO matching, tax rules, currency conversion, fixed-point arithmetic, and report-model creation. |
| `xml_generation` | Formatting, serializing, and validating one final XML document. |

The stage says where the problem was found. It does not replace the diagnostic code or its source
location.

## Application result

One application run returns one `ApplicationResult` with:

- an overall status;
- all diagnostics from every stage;
- all unresolved user-decision requests; and
- one result each for capital gains, dividends, bank-deposit interest, and other interest;
- explicit classification exclusions in `excludedItems`, distinct from per-report ISIN exclusions; and
- informational income totals, fee totals, corrections, and every answer the decision session
  keeps (`decisions`, see "Structured user-decision contract").

Income results also retain reference-catalogue provenance, confirmed withholding/relief treatment,
and a list of supporting documents to attach in eDavki. A generated XML is not a filed return or
proof that its supporting documents have been attached.

| Report object key | Report-type API value | XML form |
| --- | --- | --- |
| `capitalGains` | `capital_gains` | Doh-KDVP |
| `dividends` | `dividends` | Doh-Div |
| `bankInterest` | `bank_deposit_interest` | Doh-DHO |
| `otherInterest` | `other_interest` | Doh-Obr |

Report results and report-type values are always returned in that order. Bond coupons and broker
cash interest are separate income subtypes within `otherInterest`, not separate XML results.
A bond-income error safely isolated to a complete ISIN prompts the user to exclude the failed
ISINs from Doh-Obr or fail that form. Doh-Obr is `needs_input` until the user answers; valid Doh-DHO
and unrelated reports remain available. The other-interest exclusion flow is defined below.

The application JSON contract is version 2 because replacing the previously documented single
`interest` result changes its shape and API values. Parser diagnostics JSON is a developer format
with `schemaVersion` 1 and no compatibility promise before the beta.

The core of this contract is implemented in `ApplicationResultBuilder`
(`include/taxbroker/application_result.hpp`) and `serializeApplicationResultJson`: diagnostic
adapters, report scope, deterministic order and IDs, report and overall status, and the JSON. The
decision lifecycle (run revision, answers, recalculation) is in `DecisionSession`
(`include/taxbroker/decision_session.hpp`); of the eleven request types only the three
failure-resolution types, the loss question, the ratio question, the classification question of
a trade investment, the payer choice, the deposit interest question and the bond type question
have question data and answers so far. The capital failure choice is applied
(`FailureResolutionStage`, `include/processors/failure_resolution_stage.hpp`): see "Choosing
whether to exclude failed capital ISINs". The classification of trade investments is applied
(`TaxClassifier`, `include/processors/tax_classifier.hpp`): see "Classification and
`excludedItems`". The dividend and other-interest failure choices use the same stage, but the
report command does not run the dividend or interest stage yet, so `taxbroker report` asks
neither these choices nor the bond type.
Developer mode, the income summaries and XML failure isolation are not part of it yet; until they
exist, `unsafe` is `false`.

### Report result

Each report result contains:

- one status;
- the IDs of diagnostics that affect it;
- the IDs of unanswered requests that affect it;
- the generated XML, or an API reference from which it can be downloaded, when available;
- the user's `failureResolution` when the user chose how to handle failed ISINs;
- every excluded investment's name, ISIN, reason codes, and diagnostic IDs when exclusions apply;
  and
- an `unsafe` marker for developer-mode output; and
- an optional machine-readable `reason`, including `below_deposit_interest_threshold` for a
  bank-interest `no_data` result and `no_selected_year_disposals` for a capital-gains result
  without reportable selected-year sales.

The status is one of:

| Status | Meaning |
| --- | --- |
| `generated` | The XML was generated. It may contain documented, confirmed exclusions or be marked unsafe. |
| `no_data` | No reportable rows remain, or a documented tax rule requires no return. This is a successful result and has no XML. |
| `needs_input` | Processing can continue only after the user answers a structured request. No XML is available yet. |
| `failed` | The report cannot be generated from the current run and has at least one blocking error. |

An XML payload is present only for `generated`. It is absent for `no_data`, `needs_input`, and
`failed`. `needs_input` is not a failure and must identify at least one unanswered request.
`failed` must identify at least one blocking diagnostic.

When a report has both an open request and a blocking error, the report is `failed` unless every
blocking error that affects it is linked to an open request of that same report (for example
`incomplete_history` and its `capital_gains_failure_resolution_required`). A request that cannot
lead to a valid report is not worth answering. The request stays listed. A request of another
report does not resolve the error here: an error that affects capital gains and dividends, linked
only to a capital-gains request, still makes dividends `failed`.

A capital `no_data` result with reason `no_selected_year_disposals` tells the later GUI to explain
that no Doh-KDVP return is needed for the selected year based on the supplied data. This includes
valid empty years and years with only purchases or supported corporate actions. It does not suppress
unresolved history, rejected-row or classification problems, or change dividend/interest outcomes.
The boundary processor exposes source-sale presence; later report processing determines this status.

`unsafe` is `false` unless the user completes the three developer-mode confirmations for
incomplete history. Excluding an unsafe ISIN in normal mode does not make the remaining XML
unsafe.

`failureResolution` is either `exclude_failed_instruments` or `fail_report`. It is present only
after the user answers that report's `capital_gains_failure_resolution_required`,
`dividends_failure_resolution_required` or `other_interest_failure_resolution_required`; otherwise
it is omitted. Each report owns its own
`failureResolution` and `excludedInvestments`. A decision for one form never excludes an ISIN from
another form. Other-interest output always has `unsafe: false`; it has no developer override.

### Overall status

The overall status is derived after all four report results are known. Apply these rules in
order:

1. If any report is `needs_input`, use `needs_input`.
2. Otherwise, if all four reports are `failed`, use `failed`.
3. Otherwise, if at least one report is `failed`, use `partial_success`.
4. Otherwise, if there is a warning, a confirmed exclusion, an unsafe output, or a non-blocking
   error, use `completed_with_warnings`.
5. Otherwise, use `success`.

`generated` and `no_data` are both successful report results. Four `no_data` results with no warnings, exclusions or retained errors
produce overall `success`. A `no_data` report does not make a failure in another report disappear;
that combination produces `partial_success`.

## Application diagnostic

Every application diagnostic contains:

- a deterministic diagnostic ID;
- severity: `warning` or `error`;
- one pipeline stage;
- a stable machine-readable code;
- a message key with named parameters, from which the frontend builds the user-facing text (see
  "Message keys and translation");
- the English message, for logs, the CLI and developer tools;
- the affected report types, in the fixed report order;
- whether the diagnostic currently blocks those reports; and
- source and subject details when they are available.

Optional details are:

- one or more source references;
- tax date;
- field name;
- investment name and ISIN;
- the ID of a related user-decision request; and
- the minimum source value needed for the user to correct or confirm the problem.

Optional fields are omitted when unavailable. They are not sent as empty strings or `null`.

A diagnostic ID is assigned only after final deterministic sorting. IDs use the form
`diagnostic-0001`, `diagnostic-0002`, and so on. The ID is stable for identical input and choices;
it is not a permanent identity across different report runs.

### Source references

A source reference may contain:

- a run-local source ID;
- broker;
- uploaded filename with all directory parts removed;
- one-based logical source row;
- broker transaction ID; and
- internal stable input sequence used for ordering.

The stable input sequence and raw source index are not exposed in JSON. They are internal ordering
keys. Application JSON exposes the safe source ID derived from the source index, such as
`source-0002`.

For CSV, the header is row 1 and the first data row is row 2. Showing a filename and row number is
useful and safe: for example, `TransactionExport.csv, row 42`. A row number is only a location. It
is not the text or financial contents of that CSV row.

A merge conflict normally has two or more source references so the user can find every conflicting
record. A diagnostic about one event normally has one. A file-level problem can have a filename
without a row number.

### Files with the same name

A filename alone is not a unique source identity. If several selected files are named
`taxreport.csv`, the frontend lists them in request order and shows:

```text
File 1: taxreport.csv
File 2: taxreport.csv
File 3: taxreport.csv
```

Diagnostics use the matching source ID and display label, for example
`taxreport.csv (file 2), row 42`. The user can then find file 2 in the unchanged input list.

The frontend may also let the user add a run-local label such as `2022` or `2023` beside duplicate
filenames. It keeps the label locally and maps it by source ID, so it can show
`taxreport.csv (2023, file 2), row 42`. The label is never used for ordering, merging, or tax
processing and is not a substitute for the source ID. Without a label, the file number remains the
required fallback.

A source name that cannot be reduced to a plain filename (for example `exports/`) is left out of
the JSON; the source ID and the row remain.

The backend must not send `2022/taxreport.csv`, `2023/taxreport.csv`, an absolute path, or a parent
folder name to distinguish the files. A browser may not provide a real path, and a directory name
can itself be private. Request order and source ID work even when parsing fails before any dates or
transactions are available.

If the user changes the selected files or their order, it is a new report run with newly assigned
source IDs. Diagnostics, requests, and report results from different runs must not be combined by
source ID.

## Warnings, errors, and user decisions

A warning never blocks a report. It tells the user about a limitation or a choice that was already
accepted, such as using a broker exchange rate. Processing continues and the warning remains
visible with the generated output.

An error means that some input or result is not reliable. It starts as blocking for every report
listed in `affectedReports`. It stops blocking only through a documented resolution:

- the user supplies valid missing information and the run is recalculated;
- the user confirms exclusion of the whole unsafe ISIN under the normal FIFO flow;
- the user confirms exclusion of every Doh-Obr income row for the failed ISIN under the
  other-interest flow below;
- the user explicitly excludes a parsed item through the classification flow, where its scope can
  be identified safely; or
- the user completes the three developer-mode confirmations allowed only for incomplete history.

An error that is resolved by exclusion or developer mode remains in the result with
`blocking: false`. This preserves an honest record of what happened. It is not changed into a
warning.

When processing can continue after an answer, the result contains a structured request and marks
the affected report `needs_input`. The frontend must render the request directly. It must not
discover required questions by parsing a diagnostic message or a log.

Examples include `exchange_rates_required`, `corporate_action_ratio_required`, and confirmation to
exclude unsafe ISINs or fail the affected capital-gains or other-interest report. When a valid answer is supplied, the backend
reruns the affected calculation and returns a new complete result. A missing-rate diagnostic
disappears after a valid rate is supplied. An error for an excluded ISIN remains after confirmation
because the underlying data did not become valid.

## Failure scope

The processor determines scope from the data that the failed operation could affect:

| Problem | Reports affected |
| --- | --- |
| A rejected trade or corporate-action row | Capital gains only. |
| A rejected dividend row | Dividends only. |
| A rejected interest row | Its established form; both interest forms when the subtype is unknown. |
| A row or file that cannot be classified safely | All report types that the source may contain. |
| A conflicting duplicate with a known event kind | The report for that event kind. |
| Invalid shared taxpayer or filing metadata | Every non-empty report that needs that field. |
| Missing capital acquisition history | Capital gains only, using the ISIN flow in `fifo.md`. |
| A capital calculation error limited to one ISIN | Capital gains only, using the confirmed-exclusion flow when the rule allows it. |
| A Doh-Div error safely isolated to a complete ISIN (a missing rate or an amount that does not fit, of one payment) | Dividends only, with the exclusion/failure choice below. |
| Another dividend calculation error | Dividends only. |
| A Doh-Obr error safely isolated to a complete ISIN | Other interest only, with the exclusion/failure choice below. |
| Another interest calculation error | Its established form; both interest forms when the subtype is unknown. |
| XML serialization or schema failure | Only the XML file being generated. |

The parser records the kind of the row on each row diagnostic, and the application result takes
the scope from it:

| Row kind | Reports |
| --- | --- |
| trade, corporate action | Capital gains. |
| dividend | Dividends. |
| bond interest | Other interest. |
| Trade Republic broker interest | Bank-deposit interest (doh-dho); also other interest when the selected year is 2023 or earlier, because interest before 6 December 2023 belongs to Doh-Obr and the rejected row has no trusted date. |
| Interactive Brokers cash interest | Other interest. |
| interest of unknown subtype | Both interest forms. |
| benefit | No report. |
| bond maturity | No report: a maturity is information only, and bond sales and maturities are in no form. The same row with an asset class other than a bond has the capital scope (corporate action), because it can be a taxable sale. |
| private market | No report: private-market events are skipped (no units or price), so a rejected one changes no form. It gets the capital scope back when private markets are supported. |
| no kind (file, header, record with the wrong field count, unknown row type) | All four reports. |

A recognized unsupported row type (`unsupported_row_type`) without a row kind affects no report: it
stays visible as a non-blocking diagnostic. When the row has a kind, the kind decides: an interest
row with an unsupported subtype still affects both interest forms. A merge conflict takes its scope
from the kinds of the conflicting events. An instrument name conflict takes the kind of the
instrument's collection (trades and corporate actions, dividends, or interest). The merger does not
say whether an interest event is a bond coupon or deposit interest, so a conflicting interest event
affects both interest forms unless every source is Interactive Brokers. An error without any
affected report is never `blocking`.

A row positively known to belong to a category for which this application produces no XML is still
reported, but it does not block an unrelated XML file. An unknown row is broader: because its
meaning is unknown, the app must not assume that it is unrelated.

After a scoped error, the pipeline still validates and generates every unaffected report. It also
collects later independent diagnostics. It stops only work that depends on unreliable state. For
example, after incomplete history makes one ISIN unsafe, later FIFO results for that ISIN are not
calculated, but another ISIN and both income reports continue.

### Choosing whether to exclude failed capital ISINs

The processor collects every capital-gains error that can be isolated safely to a complete ISIN
before asking the user what to do. It returns one
`capital_gains_failure_resolution_required` request containing every affected investment, sorted
by ISIN and then name. Each entry contains its name, ISIN, reason codes, and diagnostic IDs.

The request offers two explicit choices:

1. `exclude_failed_instruments`: exclude every listed unsafe ISIN and generate capital-gains XML
   from the remaining processable ISINs;
2. `fail_report`: generate no capital-gains XML for this run.

Neither choice is preselected. The user may instead go back, add or correct files, and run the
report again. A choice applies only to the exact affected-ISIN list with its reasons; a changed list
requires a new answer. While the app is open, a later run with the same list uses the recorded
choice, shown in `decisions`, and the user can delete it (see "Structured user-decision
contract").

When the user chooses exclusion, the processor removes every purchase, sale, corporate action,
and calculated capital match for every listed ISIN from report output. It preserves source evidence
and replacement allocations needed by retained losses in the separate loss-analysis view.
Only the same ISIN is a replacement ([`tax_rules.md`](tax_rules.md)), so excluding one ISIN
never changes the loss of another. An open `loss_eligibility_confirmation_required` request of a
retained sale stays open; exclusion does not resolve it. Cancellation may add the dependent ISIN
to the failure list, requiring a new exclusion choice. These rules also apply to classification
exclusions. Only after remaining dependencies are resolved can capital gains be generated.
The processor deduplicates `excludedInvestments` by ISIN and lists every excluded investment in
the same deterministic order as the request. Errors resolved by exclusion remain visible with
`blocking: false`; unresolved dependent-loss errors remain blocking. Once no blocking errors or
requests remain, capital gains becomes `generated` when rows remain or `no_data` when none remain.

When the user chooses `fail_report`, capital gains becomes `failed`, contains no XML, and keeps the
errors blocking. Dividend and interest results are unchanged by either choice.

The exclusion choice is available only when the processor knows the complete ISIN and can remove
all capital report rows for it safely while preserving needed loss evidence. A file-level error,
missing ISIN, unknown row type, shared metadata error, or final XML-generation error cannot be
repaired by ISIN exclusion and follows its normal
report failure scope. If any unresolved capital-gains error cannot be isolated this way, the app
does not offer a pointless exclusion choice: capital gains fails and shows all of its diagnostics.

**Implementation.** The stage runs after every other capital stage and before the XML. It lists
the errors that block capital gains and are not linked to an open capital question. An error
belongs to one investment when it affects only capital gains, names an ISIN and is not an XML
error; a parse or merge error never does, even when it names an ISIN. One error that no
investment owns suppresses the question. The question lists the ISINs of the errors found; the
errors of an ISIN with an open loss or ratio question are not found, so they join the list only
when that question is closed with a cancel. The CLI asks this question after the loss and ratio
questions of the investments it does not list (a cancel there can add one) and before those of the
investments it lists, so `exclude` never leaves a useless answer. After `exclude`, the errors of
the listed ISINs stay with `blocking: false`, and the open loss and ratio questions of those ISINs
are dropped, because no answer can change the report any more; a question whose linked error is
not resolved (it also affects another form) stays open. Each entry of
`excludedInvestments` keeps the reason codes of the question that was answered, and its
diagnostic IDs are every error the exclusion resolved, also the error of a dropped question. A
`cancel` closes the question: the errors stay blocking, capital gains is `failed` and no
`failureResolution` is recorded.

### Choosing whether to exclude failed dividend ISINs

The same choice and the same stage as for other interest, for Doh-Div. Every dividend has an ISIN,
so every error of one dividend payment belongs to it: the error of a missing official rate (one
error for each payment that needs the rate, with its ISIN, not one for each currency and day) and
an amount that does not fit. A parse or merge error never counts, even when it names an ISIN; one
that blocks Doh-Div suppresses the question. The request is
`dividends_failure_resolution_required` with `affectedReports: [dividends]`; its question data, the
two choices (`exclude_failed_instruments`, `fail_report`, neither preselected), the cancel (the
question is closed, the errors stay blocking, `dividends` is `failed`, no `failureResolution`) and
the recorded answer for an unchanged list are the same as for other interest. On exclusion every
dividend row of the listed ISINs is removed from Doh-Div across brokers, the other rows stay, the
errors stay visible with `blocking: false`, and `dividends.excludedInvestments` lists the ISINs;
`generated` when rows remain, otherwise `no_data`. The open payer choice of an excluded ISIN is
dropped. The choice never resolves an error of another form, and another form's choice never
resolves a Doh-Div error. The warnings of Doh-Div (payer not found, foreign tax evidence) are not
errors and are not part of the choice.

### Choosing whether to exclude failed other-interest ISINs

Collect all Doh-Obr errors that can be isolated safely to complete ISINs: the error of a coupon
payment (a missing official rate, an amount that does not fit) and the bond type errors below. A
parse or merge error never counts, even when it names an ISIN, because its ISIN can differ between
duplicates; one that blocks Doh-Obr suppresses the question. Offer missing-data correction
requests where the facts can be supplied. If the error remains or
the user cannot supply the facts, return one `other_interest_failure_resolution_required` request
with `affectedReports: [other_interest]`, the run revision, source references and all failed
investments sorted by ISIN and then name. Each entry has its name, ISIN, reason codes and diagnostic
IDs. The request has two choices, with neither preselected:

1. `exclude_failed_instruments`: exclude every listed ISIN's selected-year Doh-Obr income rows
   across all supplied brokers and accounts, including otherwise valid rows for those ISINs;
2. `fail_report`: produce no Doh-Obr XML for this run.

Before an answer, `otherInterest` is `needs_input` and has no XML. The user may instead correct
the input. Cancelling the choice is not exclusion or permission to generate a partial return: as
for capital gains, the question is closed, the errors stay blocking, `otherInterest` is `failed` and
no `failureResolution` is recorded. A changed failed-ISIN list requires a new choice; the same list
in a later run of the open app uses the recorded choice, as for capital gains. The open payer
choice of an excluded ISIN is dropped, as no answer to it can change the report any more.

On exclusion, rebuild Doh-Obr from the remaining valid income, including unaffected broker cash
interest and bond coupons. Record `failureResolution: exclude_failed_instruments` and a deduplicated
`otherInterest.excludedInvestments` list with names, ISINs, reason codes and diagnostic IDs in request
order. Errors resolved for this form remain visible with `blocking: false`. Once no other blocking
errors or requests remain, use `generated` when rows remain, otherwise `no_data` with no XML.
Show all excluded investments and the incomplete coverage beside the download. Never label the
excluded investments successfully processed. Overall status is at least `completed_with_warnings`
unless another report's status takes precedence.

On `fail_report`, keep the errors blocking, set `otherInterest` to `failed`, record that choice,
and leave its `excludedInvestments` empty. Both choices preserve source evidence and leave capital
gains, dividends and bank interest unchanged. A capital exclusion does not authorize an income
exclusion, or vice versa. An error that affects several forms is resolved only for the form whose
choice was made; the entry of `excludedInvestments` of a form lists only the resolved errors that
affect that form. When a source problem also affects another form, retain a separate scoped
blocking diagnostic there; resolving Doh-Obr must not clear it.

This choice requires known instrument identity and a safely separable Doh-Obr scope. It cannot
resolve missing ISINs, unclassified rows, cash-interest errors without an ISIN, file-level or shared
metadata errors, or final XML serialization/schema/range failures. Such problems retain their
normal correction request or report-failure behavior. An unresolved non-isolatable blocking error
prevents offering an exclusion choice that could not produce a valid result. Developer mode cannot
bypass any other-interest error. Classification exclusions remain a separate documented flow.

**Implementation.** The interest stage makes the error of a missing official rate for each payment
that needs it (with its form, rows, name and, for a coupon, its ISIN), not one error per currency
and day, so a coupon's error belongs to its ISIN and a cash or deposit interest error never does.
The failure stage is the same code as for capital gains with the form and the request type as
parameters. After `exclude`, the coupon rows of the listed ISINs are removed from the Doh-Obr
model; broker cash interest and other coupons stay.

**Bond type.** A bond (a debt security) with a sale or a maturity in the reporting year asks one
`income_details_required` question, because the interest of a discount or zero-coupon bond arises
then and nothing calculates it yet. The question data has the ISIN, the name, the units sold, the
units matured (units out minus units back) and the first maturity date. Answers: `ordinary` (the
coupons go to Doh-Obr as usual) or `discount_or_zero_coupon`; neither is preselected. Without an
answer Doh-Obr is `needs_input` and the coupons of the bond are not rows. `discount_or_zero_coupon`
is a blocking `unsupported_tax_treatment` error of Doh-Obr with the bond's ISIN, and so is a
cancel (with a message that the type was not given); the failure question above can then leave
the bond out. A bond whose unit totals of the year do not fit is a blocking `arithmetic_overflow`
error of Doh-Obr with its ISIN instead of a question, and its coupons are not rows either. A bond
with coupons only and a bond sold or matured in another year ask nothing. The issuer does not
matter: a Slovenian issuer's listed bond is asked and reported like any other, because its payer
does not withhold the tax (`tax_rules.md`, "Interest forms and the annual deposit allowance").

### Normal incomplete-history result

Before the user answers, capital gains is `needs_input`; dividend and interest processing
continues. If the user chooses `exclude_failed_instruments`, every listed unsafe ISIN is excluded
and capital gains becomes `generated` or `no_data` once all remaining loss dependencies and other
requests are resolved. Otherwise it remains `needs_input` for the dependent question. Each excluded
ISIN's `incomplete_history` diagnostic remains non-blocking, and all excluded names and ISINs remain
visible in the report result.

If the user chooses `fail_report`, capital gains is `failed` and the error remains blocking. No
cost basis is invented and the unmatched sale is not silently removed. Dividend and interest
results remain available.

### Developer-mode result

Developer mode can bypass only incomplete history and only after the three confirmations defined
in [`fifo.md`](fifo.md). Capital gains becomes `generated`, `unsafe` is `true`, and the blocking
flag on the retained `incomplete_history` diagnostic becomes `false`. The output and download
screen must list the affected investments.

`generated` is reached only if the separate known-source-event export passes all remaining
validation and XML checks. An unanswered loss question remains `needs_input`; an unrepresentable
stock field or action error still fails or uses normal confirmed exclusion. The override does not
restart guessed FIFO matching when a raw running quantity becomes positive.

No other error becomes safe merely because developer mode is enabled.

## Structured user-decision contract

Every request has a deterministic ID, `type`, `affectedReports`, safe source references and a
`runRevision` identifying the exact input and current decisions that produced it. Answers include
that revision and the request ID. Reject stale answers after files, event corrections, action
sources, referenced catalogue revisions or affected-ISIN lists change; rerunning must not apply an answer to a new question that
happens to reuse the same sorted request ID. Request IDs use `request-0001` and so on after sorting.

`DecisionSession` (`include/taxbroker/decision_session.hpp`) implements this without any transport.
It holds the calculation of the current run and the answers, in memory only, and follows these
rules:

- **Revision.** The first result of a session is revision 1. Every recalculation, after accepted
  answers, deleted answers or a new run, takes the next number. A number is never used twice while
  the session lives, so an answer from an earlier calculation or run never matches.
- **Question identity.** A question is its `type` and its question data: for the two
  failure-resolution types, the investments with ISIN, name and reason codes, in a normalized
  order; for the loss question, the ISIN, its name and the facts of each open sale (below); for
  the ratio question, the ISIN, its name, the effective date, every source record (broker,
  transaction ID and the broker's own quantity field, sorted by broker and transaction ID) and
  the open units just before the action on its row date, and every range of effective dates it
  may have with the open units just before it on them (`fifo.md`, "User confirmation of the
  ratio"). Sources, rows, source IDs, the tax date and diagnostic IDs only say where to look; they
  are run-local and not part of it, so another file order asks the same question. Each type puts
  into its question data exactly the facts its answer depends on, such as a currency and date, or
  an action with the broker and transaction ID of every source. A different value is a different
  question.
- **Answering.** One call takes the answers to any open requests of the current revision. It is
  rejected as a whole, and nothing changes, when one answer names an older revision
  (`answer_rejected.stale_revision`), a request that is not open (`answer_rejected.unknown_request`),
  a request answered twice in the call (`answer_rejected.duplicate_answer`), or an answer that does
  not fit the type or a request without question data (`answer_rejected.answer_does_not_fit`).
  A ratio answer is also rejected when a number is 0 or less
  (`answer_rejected.ratio_not_positive`, with the parameter `field`: `newShares` or `oldShares`),
  when both numbers are equal (`answer_rejected.ratio_unchanged`), when the position after the
  action does not fit the units (`answer_rejected.ratio_out_of_range`) or when the confirmed
  position differs from the preview of the ratio (`answer_rejected.preview_mismatch`), and when its
  effective date is before the question's first range or after its last one
  (`answer_rejected.effective_date_out_of_range`, with `earliestDate` and `latestDate`) or lies
  between two ranges, where no unit is held just before the action
  (`answer_rejected.effective_date_without_units`), and when the units after the action do not
  cover a later sale of its range (`answer_rejected.later_sale_too_large`: the preview plus the
  range's lowest change after the action is below 0). The preview is the one of the units on the
  answer's effective date. A quantity answer is rejected when its units are 0 or less
  (`answer_rejected.correction_not_positive`), above the units left
  (`answer_rejected.correction_above_open_units`), `0.00000100` units or more below the sale
  (`answer_rejected.correction_too_large`), or when the EUR value of the removed units, with the
  roundings of the ISIN before it, would reach EUR 0.01 (`answer_rejected.correction_over_budget`).
  An accepted
  ratio is recorded reduced: `20 / 10` as `2 / 1`. The key tells the frontend which input to
  mark: `ratio_not_positive` the one named by `field` (the only key that can mean either
  number), `ratio_unchanged` both numbers, `ratio_out_of_range` the ratio, `preview_mismatch`
  the confirmed position, `effective_date_out_of_range` and `effective_date_without_units` the
  date, and `later_sale_too_large` both the ratio and the date. Each rejection has a message key
  with
  the `requestId` parameter. An accepted answer is bound to the request's question, never to its
  ID, and the run is recalculated.
- **Recalculation.** The calculation rebuilds the whole run from its full input and gets only the
  recorded answers; no ledger or report state is kept between calculations. A stage looks up the
  answer to its own question. Unrelated reports come out the same because the calculation is
  deterministic. The session changes only after a calculation succeeds: when a calculation fails
  with an exception, the answers given or deleted in that call are dropped, every other recorded
  answer stays, and the result and its revision stay the previous ones.
- **Recorded answers.** An answer stays recorded until the app closes or the user deletes it, also
  across runs with new input. It applies wherever a stage asks the same question again. Every
  result lists all recorded answers in `decisions` (`decision-0001` and so on, valid for that
  revision), each with its question data, its answer, `fromEarlierRun` and `used` (asked and
  applied in this calculation), so a recorded answer is a visible default, never a silent one.
  A decision has no sources, because they are run-local.
- **Deleting answers.** The user deletes some recorded answers by ID, or all of them, naming the
  current revision. A call with an older revision (`answer_rejected.stale_revision_delete`), an ID
  that is not listed (`answer_rejected.unknown_decision`) or the same ID twice
  (`answer_rejected.duplicate_delete`) is rejected as a whole; these keys have the `decisionId`
  parameter (`all` when everything was to be deleted). Otherwise the run is recalculated and the
  questions are asked again.
- **Cancellation and missing answers.** Cancel is an answer for every type with question data and
  is recorded on the question; the stage applies the documented rule of its type. For the
  capital failure question the question is closed, its errors stay blocking, capital gains is
  `failed` and no XML is written. For the other-interest failure question the question stays open,
  the report stays `needs_input` and no XML is written. For the loss
  question, each open sale gets a blocking `loss_eligibility_unresolved` error with the key
  `loss_eligibility_unresolved.cancelled` and no request; the error belongs to its investment, so
  the capital failure question then lists it. For the ratio question, the action gets a blocking
  `corporate_action_ambiguous` error with the key `corporate_action_ambiguous.cancelled` and no
  request, with the same result; for a question before a sale up to 31 December with too few
  units (`earlierSaleDate`), the sale gets the `incomplete_history` error instead, because a
  cancel there means that purchases are missing. For the classification question, the investment
  gets a blocking `tax_classification_unknown` error with the key
  `tax_classification_unknown.cancelled` and no request; the error belongs to its investment, so the
  capital failure question then lists it, and a cancel is never an exclusion. An unanswered
  request stays open: the session never answers anything by itself.
- **Memory only.** Answers are never written to disk or browser storage and are gone when the app
  closes. Only the taxpayer profile may be saved (#278).

In the JSON, a loss request has `"options":["no_other_activity","other_activity"]` and
`"lossCheck"` with `name`, `isin` and `sales`; each sale has `saleDate`, `transaction` (`broker`,
`transactionId`; left out without an ID), `saleUnits`, `lossUnits`, `windowStart`, `windowEnd`,
`knownReplacements` (`date`, `units` in the units of the sale: a purchase on the other side of a
confirmed split is converted, `tax_rules.md` rule 7), `knownIneligibleUnits` and `openUnits`.
Units are exact decimal strings with eight decimals (`"4.00000000"`). A recorded loss answer lists
the same `lossCheck` and `"answer":"no_other_activity"`, `"other_activity"` or `"cancel"`.

A ratio request has no `options` and has `"ratioCheck"` with `name`, `isin`, `effectiveDate`,
`sources` (each with `broker`, `date` (the row's date), and `transactionId` and `sourceValue`
when the record has them; `sourceValue` is the broker's quantity field as an exact decimal, shown
read-only) and
`positionBefore` (units, eight decimals, just before the action on the broker's row date
`effectiveDate`), or instead `earlierSaleDate` when a sale before the row needs more units than
were held and the action must come on or before it, and `dateRanges`: in date order, each with
`earliestDate`, `latestDate`, `positionBefore`, the units just before the action on every date
of the range, and `lowestChangeAfter`, the lowest running total of the trades after the action
up to 31 December (30 January for an action with a row after 31 December), or up to 11 days
before the next action's row; a later trade date is a range of its own with the trades of that
day only (eight decimals, 0 or below). `ratioCheck` also has `askSameEvent`: `true` when the rows
are on different dates (split rows of two brokers at most 10 days apart). Such a question also
takes the answer `"separate_events"` (the rows are two splits), which blocks the investment with
`corporate_action_ambiguous.different_dates`; a ratio answer to it says the rows are one split.
It is answered with `newShares`, `oldShares` (whole numbers), `effectiveDate` (from the
announcement, within a range) and `confirmedUnits` (the previewed position for that date's units,
eight decimals). A
recorded ratio answer lists the same `ratioCheck` and
`"answer":{"newShares":2,"oldShares":1,"effectiveDate":"2024-06-01","confirmedUnits":"6.00000000"}`
with the reduced numbers, `"answer":"separate_events"` or `"answer":"cancel"`.

A classification request has `"options":["capital_gains","exclude_from_supported_reports"]` and
`"classificationCheck"` with `name`, `isin` and `classLabels` (the class label of each row as the
files gave it, `""` for a blank class, sorted without repeats). A recorded classification answer
lists the same `classificationCheck` and `"answer":"capital_gains"`,
`"exclude_from_supported_reports"` or `"cancel"`.

An income classification request (the same type) has `"options"` (`"dividends"` left out without an
ISIN), `"otherInterestSubtypes"` (`["broker_cash","bond_coupon"]`, `"bond_coupon"` left out without
an ISIN) and `"incomeCheck"` with `eventKind` (`"interest"` or `"dividend"`), `receiptDate`, `name`,
`isin` and `transaction` (`broker` and `transactionId`; each only when known), `classLabels`,
`grossAmount` (four decimals) and `currency`. A recorded answer lists the same `incomeCheck` and
`"answer"`: `"dividends"`, `"bank_deposit_interest"`, `{"choice":"other_interest","subtype":
"broker_cash"}` or `{"choice":"other_interest","subtype":"bond_coupon"}`,
`"exclude_from_supported_reports"` or `"cancel"`. The CLI has no prompt for it until the income
forms are part of its report.

A quantity request has no `options` and `"quantityCheck"` with `name`, `isin`, `saleDate`,
`transaction` (`broker` and `transactionId`, only when the sale has one), `originalUnits` (the
units of the sale), `proposedUnits` (the units the earlier purchases left, eight decimals),
`unitValueEur` (EUR per unit that values the removed units: the higher of the sale price and the
highest exact value per unit of the open lots) and `budgetUsedEur` (the EUR impact that other
roundings of the ISIN have used before this sale). The two EUR values are exact fractions with
`numerator` and `denominator` as strings, because the values do not fit a JSON number. It is
answered with `{"correctedUnits":"0.99999998"}` (eight decimals) or cancelled; a recorded answer
lists the same `quantityCheck`.

A payer request (`income_details_required` for a dividend payer whose reference rows disagree) has
no `options` and `"payerCheck"` with `name`, `isin` and `options`: the reference rows, sorted by
name, address, country, tax number and source row, each with `name`, `address`, `country`, `taxNumber`
(only when the row has one) and `source` (`source`, `sourceRevision`, `file`, `row` and `origin`,
`built_in` or `supplied`). It is answered with `{"option":1}`, the position of the picked row in
`payerCheck.options` counted from 0, or `"skip"`; a position outside the options and a cancel are
rejected with `answer_does_not_fit`. A skip, like no answer, gives empty payer facts and the same
warning as a payer that is not found. The request is listed with its form (`requestIds`), but it
never makes the form `needs_input` and never blocks. A recorded answer lists the same `payerCheck`.
The question changes (a new question, a new answer) when a reference row changes. The same question
is asked for the issuer of a bond coupon; it then affects Doh-Obr.

A deposit interest request (`deposit_interest_total_required`) has no `options` and `"depositCheck"`
with `year`, `importedInterestEur` (the exact total of the imported qualifying interest, four
decimals) and `limitEur` (`"1000.0000"`). It is asked only when that total is above 0 and at most
the limit, and it affects only Doh-DHO: unanswered, Doh-DHO is `needs_input` and the other forms are
not affected. It is answered with `"above_limit"` (the total with the interest of the user's other
Slovenian and EU banks is above EUR 1,000) or `"not_above_limit"`; a cancel is rejected with
`answer_does_not_fit`. `above_limit` makes the Doh-DHO rows from the imported interest and adds the
warning `deposit_interest_add_other_banks`; `not_above_limit` makes no rows and gives the form the
reason `below_deposit_interest_threshold`. A different total is a different question. A recorded
answer lists the same `depositCheck`.

A request type whose processor does not define its question data yet cannot be answered, not even
cancelled: without question data, all its questions would be the same question, and one answer
would reach questions the user never saw. The builder rejects question data that does not fit the
request type, and a failure-resolution question without an investment.

| Type | Required question data and answer |
| --- | --- |
| `exchange_rates_required` | Missing currency/date pairs, usage and any verified event-specific fallback; answer with official pair rates or explicit fallback acknowledgements. |
| `corporate_action_identity_required` | Candidate actions with ISIN, dates, types, ratios and all sources; answer with confirmed grouping, effective dates and order where needed. |
| `corporate_action_ratio_required` | One request per ISIN, for the first action its pool reached: the ISIN, its name, the effective date, every source record of the action (broker, transaction ID, read-only quantity field; rows of different brokers on one date are one action) the open units of all brokers just before it, and the range of effective dates that keep them. Also asked for a split row after 31 December that a December loss window reaches, and for split rows of two brokers at most 10 days apart (`askSameEvent`, answer `separate_events` when they are two splits). Answer with positive, different whole-number new/old shares, the effective date from the announcement within the range and the confirmed previewed position, or cancel. The GUI and the CLI also accept the shares before and after and turn them into the two numbers; they warn about a reduced number above 1000. |
| `quantity_correction_required` | One request for the first sale of an ISIN whose pool stopped for a shortage below `0.00000100` units that the discarded source digits do not prove to be rounding: the ISIN, its name, the sale (date, broker and transaction ID), its units, the units left (the proposed quantity), the unit value in EUR and the budget used. Answer with the corrected sale units (above 0, at most the units left, less than `0.00000100` below the sale, and with the roundings before it below EUR 0.01 in total) or cancel. |
| `loss_eligibility_confirmation_required` | One request per ISIN: the ISIN, its name and every sale of the year whose loss the files cannot decide, each with its date, broker and transaction ID, sale units, loss units, inclusive window, known replacements (date and units in the units of the sale) and known and open units. Answer `no_other_activity` (the investment was not bought or sold at another broker or account: the open units keep their loss) or `other_activity` (every open unit is blocked, the safe side; the user corrects it in eDavki). |
| `tax_classification_required` | One request per trade investment whose asset class is missing, not recognized or conflicting: the ISIN, its name and the class labels of its rows (question data), with the options `capital_gains` and `exclude_from_supported_reports` and no preselected choice; answer with one of them or cancel. A second kind of question is for one interest or dividend payment of unknown type (`tax_rules.md`, "Supported tax categories and user classification"): the question data is the receipt date, name, ISIN, transaction, class labels, gross amount and currency; the options are `dividends`, `bank_deposit_interest`, `other_interest` (subtype `broker_cash` or `bond_coupon`) and `exclude_from_supported_reports`, without `dividends` and `bond_coupon` when there is no ISIN. |
| `deposit_interest_total_required` | The year and the exact imported qualifying interest in EUR (above 0, at most 1,000); answer yes (`above_limit`) or no (`not_above_limit`). It is the only request with no default and no cancel. |
| `income_details_required` | Income items and a missing gross amount, requested treaty-claim facts, the bond type of a bond with a sale or maturity in the year (`ordinary` or `discount_or_zero_coupon`, or a cancel), and the choice among several reference rows of one payer (the options are the rows with their facts and a skip for each payer; a skip or no answer never blocks and gives empty payer facts, so it creates no unanswered blocking request); a payer that is not found, a blank tax and other banks' deposit interest do not use this request; answer with validated details or explicit withdrawal of an optional exemption claim. |
| `capital_gains_failure_resolution_required` | Exact complete unsafe-ISIN list, diagnostic IDs and the two existing exclusion/failure choices; answer with one choice for the whole list. |
| `dividends_failure_resolution_required` | Exact complete failed Doh-Div ISIN list, safe sources, reason codes and diagnostic IDs; answer with `exclude_failed_instruments` or `fail_report` for that form and the whole list. |
| `other_interest_failure_resolution_required` | Exact complete failed Doh-Obr ISIN list, safe sources, reason codes and diagnostic IDs; answer with `exclude_failed_instruments` or `fail_report` for that form and the whole list. |

Confirmation controls have no default answer. Acknowledgement of a potential corporate-action
mismatch is a frontend action supported by structured result fields; a non-blocking reminder does
not manufacture an unanswered blocking request. The app takes no amount of other banks' interest.

`excludedItems` entries contain the parsed item/instrument identity, established category, explicit
exclusion reason, safe sources and diagnostic IDs. Record whether the exclusion follows a verified
tax rule or an explicit user choice. Reason `slovenian_tax_settled` follows from a dividend payer with the
residence country `SI` (a Slovenian company deducts the tax itself); an ISIN prefix alone cannot establish it. Capital and other-interest failure
exclusions use each report's own `excludedInvestments`. A classification request does not override malformed source rows, missing
instrument identity or undocumented tax calculations. A known unsupported category can be shown
without blocking unrelated reports; an unknown category cannot be silently assigned no scope.

## Classification and `excludedItems`

The classification stage runs before FIFO, from the asset class of each trade investment; it never
reads a name or a symbol. Shares and fund units stay in capital gains. A bond and a crypto
instrument are left out by a tax rule; an unknown class asks the question above; a derivative and
a private fund are not changed by it (the capital gates give their `unsupported_tax_treatment`
error, and the capital failure question can leave them out into `excludedInvestments`).

Every instrument that is left out has one entry in the top-level `excludedItems` list, sorted by
ISIN and always present (`[]` when empty). It is distinct from the `excludedInvestments` of a
report, which lists failed investments the user chose to leave out of one form. An entry has:

- `name`, `isin`, and `classLabels` (the class labels of its rows as the files gave them). An
  interest or dividend payment of unknown type that the user left out is one entry per payment
  (category `unknown`, reason `user_choice`), and `isin` is left out when the file gave none. If
  its class is a share, fund or bond, which should have an ISIN, the warning has the key
  `.user_choice_income_missing_isin` and tells the user to check the export; for any other class
  no ISIN is expected and the key is `.user_choice_income`;
- `category`: `bond`, `crypto`, `unknown` (an unknown class the user left out), `dividend` (the
  dividends of a Slovenian payer) or `interest` (kept for the contract; no stage makes it today);
- `reason`: `debt_security`, `crypto_not_reported`, `slovenian_tax_settled` or `user_choice`, and
  `basis`: `tax_rule` or `user_choice`;
- for a bond only, `bond` with `boughtUnits`, `soldUnits` and `maturedUnits` in the selected year
  (units out minus units back, never added up by order) and the `maturityDate` of the first
  maturity row, when there is one. Units are exact decimal strings with eight decimals; amounts
  are not listed;
- safe `sources` and the `diagnosticIds` of its `tax_category_not_reported` warning. An entry that
  follows a tax rule (`basis` `tax_rule`: a bond, crypto, a Slovenian dividend payer) has no
  warning, because the exclusion is on purpose, and its `diagnosticIds` is `[]`. An entry of a user
  choice has its warning.

A bond with no trade or maturity row, or a crypto instrument with no trade or corporate-action
row, in the selected year is left out of capital gains without an entry or a warning. For a bond whose units overflow, `bond` is left out. A bond that only has maturity rows (no file gave its class) has an empty `classLabels`.

```json
{
  "excludedItems": [
    {
      "name": "Example Bond",
      "isin": "XS0000000001",
      "category": "bond",
      "reason": "debt_security",
      "basis": "tax_rule",
      "classLabels": ["BOND"],
      "bond": {
        "boughtUnits": "1000.00000000",
        "soldUnits": "0.00000000",
        "maturedUnits": "1000.00000000",
        "maturityDate": "2025-08-14"
      },
      "sources": [],
      "diagnosticIds": []
    }
  ]
}
```

## Diagnostic codes

Codes are stable API values. New behavior gets a new code; an existing code must not be reused with
a different meaning.

The parsing stage keeps the existing codes `unknown_row_type`, `unsupported_row_type`,
`unsupported_asset_class` (a warning for a private-fund trade row only; a crypto row has none, a tax rule leaves it out), `missing_field`, `invalid_value`, `inconsistent_value`, and
`parse_error`. It adds `field_count_mismatch`: a CSV record has a different number of fields than
the header, so the row is skipped and no field of it is trusted. The message gives only the
expected and found counts. It also adds `localized_number_format`, a file-level warning without row
or field. The Trade Republic parser reports it when at least 3 rows of one file were rejected
because a numeric field has a comma or parentheses, which Trade Republic does not export. It means
the file was probably saved again in a spreadsheet program. The message gives only the row count.
Each of those rows still has its own `invalid_value` error, and the count is a number of rows,
not fields.

It also adds `trade_amount_mismatch`, a row-level warning on the field `amount` (or
`original_amount` for a trade with `original_currency`). The Trade Republic parser reports it
for a valid trade when `|amount|` differs from `|unit price × units|` by more than EUR 0.01. The
comparison is exact at the stored scales, never rounded, and the fee is not part of it (the export
amount is gross of fees). A Trade Republic export rounds the amount to cents, so a healthy file
differs by at most EUR 0.005; a larger difference means a corrupted file or a changed export
format. The row is kept with its values, the amount is not used for any tax value, and nothing is
repaired. The warning never blocks a report, and its message gives only the field name, no
amounts. A trade with an empty `amount` or a rejected row gets no such warning.

It also adds `invalid_header`, a file-level error without row or field. The parser reports it when
the header misses a column the parser reads (the list is in [`architecture.md`](architecture.md)),
which includes a file saved with semicolons or tabs instead of commas. No rows are read. The
message gives only our own column names, never text from the file. A file of 0 bytes is still a
`parse_error`.

The rate-file reader (see [`calculations.md`](calculations.md), "Official ECB rate files") uses
`invalid_header`, `field_count_mismatch`, `missing_field`, `invalid_value` and `parse_error` with
their own keys, and adds `duplicate_rate`, `mixed_rate_years`, `duplicate_rate_year` and
`no_rates`. A rate file is rejected whole at its first fault, so a file has one diagnostic, with
its row and column where the fault is in a row. `duplicate_rate_year` is a file-level error on each
of the files that cover the same year. These diagnostics have no row kind and name the file by
its display name only. Their keys are `invalid_header.rate_file_columns`, `invalid_header.rate_file_duplicate_column`,
`field_count_mismatch.rate_file`, `parse_error.unreadable_rate_file`,
`invalid_value.rate_field_expected`, `invalid_value.rate_currency`, `invalid_value.rate_date`,
`invalid_value.rate_value`, `missing_field.rate_value`, `duplicate_rate`, `mixed_rate_years`,
`duplicate_rate_year` and `no_rates`. A message never holds a rate value.

The reference-file loader (see [`reference_data.md`](reference_data.md), "How the app loads the
files") reports the first fault of a supplied file with one `reference_file.*` key, the file name
(the last path part only) and the logical row when the fault is in a row (header = 1). The keys
are `reference_file.unknown_name` and `reference_file.repeated_name` (parameter `name`; checked
before any file is read), `reference_file.unreadable`, `reference_file.unclosed_quote`,
`reference_file.line_break`, `reference_file.missing_columns` (`count`, `columns`),
`reference_file.repeated_column` (`column`), `reference_file.field_count` (`expected`, `found`),
`reference_file.empty_field` (`field`), `reference_file.invalid_isin`,
`reference_file.invalid_isin_source`, `reference_file.invalid_country`,
`reference_file.invalid_instrument_kind`, `reference_file.invalid_verified_on`,
`reference_file.invalid_source`, `reference_file.missing_source_revision` and
`reference_file.duplicate_broker_code`. A message never holds a value of the file.

A skipped row reports every invalid field, so one row can have several diagnostics. They follow
the column order of the export; the sign checks (`inconsistent_value`) come last and only use
valid fields. The JSON `errorCount` counts diagnostics, not rows.

For otherwise valid trades/actions, missing classification is a `missing_field` error and an
unrecognized class is an `unsupported_asset_class` error on `asset_class`. The event is retained
as unresolved; messages must not say it was skipped. A same-file class disagreement produces
`inconsistent_value` while retaining both events. The merger preserves all original class evidence
and reports its instrument conflict after rejecting any conflicting transaction identity groups.
Classification requests and report blocking remain the processor's responsibility.

A private-market row with shares or a price is kept as a cash-only event and gets an
`unsupported_asset_class` warning (key `unsupported_asset_class.private_market_units`, field
`shares`, or `price` when only the price is filled). It has the row kind private market, so it
affects no report.

A Trade Republic bond maturity is several rows, and each is one event of its own (kind `Maturity`,
`MaturityCancelled`, `Payment` or `PaymentReversal`); nothing is linked or added up at parsing.
They have the row kind bond maturity, so they affect no report. Units with the wrong sign for the
kind are `inconsistent_value` (key `inconsistent_value.redemption_sign`, field `shares`); a row of
another asset class is `unsupported_asset_class` (key `unsupported_asset_class.redemption_not_bond`)
with the capital scope. The two correction rows of a maturity (a cancelled maturity and the
maturity booked again) keep the maturity `date` but have a later `datetime`, so each also gets the
usual `inconsistent_value.datetime_date` warning. A value in a column the row kind does not use
(for example `amount` on a corporate-action row, or `price` on a cash row) is not stored and gets a
warning with key `inconsistent_value.redemption_unused_field` and the column as `field`. See
"Trade Republic bond rows" in `architecture.md`.

A missing display name with a known ISIN is a `missing_field` warning on `name`, not rejection.
The original blank remains source evidence even if another supplied name completes presentation.
The merger uses the first non-empty supplied name for that same ISIN under the documented source
ordering. If all names are blank, the name remains empty; no ISIN is substituted into a name field.
Required missing ISINs and malformed numeric facts keep their existing rejection errors. Original
names and class labels belong to domain evidence and local debug dumps; this change adds no raw
row fields, paths or source indices to the parser JSON contract.

The later stages use the codes defined by their rule documents. These include:

- `conflicting_duplicate` for two records with the same broker transaction identity but different
  event data;
- `duplicate_transaction_in_file` for exact copies of one transaction inside one file: a merging
  warning that blocks no report, one per file and transaction ID, with that file's rows in its
  source references. The copies are merged into one event, as for copies across files, which add
  no warning;
- `invalid_reporting_year` and `incomplete_history` for historical validation;
- the `corporate_action_*` and `unsupported_corporate_action` codes in
  [`fifo.md`](fifo.md);
- `invalid_exchange_rate`, `broker_exchange_rate_used`,
  `unrepresentable_value`, and `arithmetic_overflow` in [`calculations.md`](calculations.md); and
- `xml_value_out_of_range`, `xml_serialization_failed`, and `xml_schema_validation_failed` for the
  final XML boundary.

Additional codes and their default scope are:

| Code | Severity and scope |
| --- | --- |
| `corporate_action_sources_mismatch_possible` | Warning for capital gains; one per ISIN/year with action records from multiple sources used by the ledger or loss analysis, including relevant following-January actions. |
| `corporate_action_quantity_rounded` | Warning for capital gains on one applied action: its units were rounded to eight decimals, and the rounding of the ISIN stays below EUR 0.01. No units or amounts in the message. |
| `quantity_rounding_reconciled` | Warning for capital gains on one sale: its units were reduced to the units the earlier purchases left, because the discarded source digits show a rounding difference of less than `0.00000100` units. The original units stay in the unit evidence; no units or amounts in the message. |
| `source_quantity_corrected` | Warning for capital gains on one sale: its units were reduced to a quantity the user confirmed, because the files do not prove rounding. The original units stay in the unit evidence; no units or amounts in the message. |
| `incomplete_history` keys | Without a key: a sale needs more units than the purchases left. Key `.correction_required` for a sale short by less than `0.00000100` units while its quantity question is open (linked to it); key `.correction_cancelled` after a cancel (no request, so the exclusion question follows). |
| `xml_basis_rounding_difference` | Warning for capital gains, one per purchase row of a Doh-KDVP list whose `F4` times `F3`, rounded to four decimals, differs from the exact basis of its units rounded the same way. Output only: no value changes and no budget is used. Its sources and date are those of the purchase. |
| `xml_value_out_of_range` | Error for the one form whose writer rejects the value, now Doh-KDVP (capital gains): one per field that the form does not accept, and no XML for that form. The parameter `field` is the XML field name (for example `taxNumber`, `ISIN` or `F8`), never the value; the context has the ISIN and name of the list when they are valid text. |
| `loss_eligibility_unresolved` | Error for capital gains on one sale, isolated to its complete ISIN where possible: key `.confirmation_required` while the loss question is open (linked to it), `.cancelled` after a cancel (no request). A warning per ISIN with key `.assumed_blocked` when a caller that does not ask (the debug tools) treats the open units as blocked; no request. Key `.units_mismatch` (stage `fifo_tax`, on the sale) is a safety check: a decided sale whose rule blocks more units than the sale has loss units within the holding period cannot be written as rows; it should never happen. |
| `corporate_action_not_applied` | Error for capital gains: a corporate action whose result cannot be reported yet. Key `.waiting_for_earlier_action` (parameter `earlierDate`) for a later action of the ISIN, linked to the open ratio question of the earlier one; `.loss_window` for a date after 31 December with a merger row inside a December loss window (a split row there has its ratio question or its own error instead). An action of a class that never reaches Doh-KDVP gets none: that class has its own error. |
| `corporate_action_ambiguous` | Error for capital gains on one action (`fifo.md`): key `.ratio_required` while its ratio question is open (linked to it), `.ratio_required_before_sale` (parameters `saleDate` and `days`, 10) for the same question when a sale before the row needs more units than were held and the row may be a late booking, `.cancelled` after a cancel (no request), `.same_day` for two or more rows of one broker for the ISIN on one date, and `.different_dates` (parameter `days`, 10) for split rows of different brokers on dates at most that far apart that the user says are two splits, or that cannot be asked as one split (more than one such neighbour date, a merger row, or a broker on both dates; no request for these until the identity question exists). |
| `corporate_action_applied` | Warning for capital gains on one applied action, key `.user_ratio` (parameters `newShares`, `oldShares` and `effectiveDate`): the user, not the broker, supplied the ratio and the effective date. |
| `corporate_action_missing_position` | Error for capital gains on one action: the files show no units just before it on its row date. No question is asked. |
| `corporate_action_unrepresentable` | Error for capital gains on one action that cannot be applied: an overflow, a lot that would become 0 units, or, with key `.rounding_budget`, rounding that reaches EUR 0.01 for the ISIN. The pool stops at the action. With key `.unit_value` (stage `fifo_tax`, on the first sale that uses it) the value of one unit after a split does not fit the `F4` of a Doh-KDVP row; no pool stops, but the ISIN gets no list. |
| `unsupported_corporate_action` | Error for capital gains on one action that needs tax rules the app does not have; key `.merger` for a merger row. No question is asked. |
| `tax_classification_unknown` | Error from the classification stage (never from the capital gates) for capital gains on an investment with an unknown asset class: key `.confirmation_required` while its classification request is open (linked to it), `.cancelled` after a cancel (no request). The parse and merge errors about the class of that investment stay errors but no longer block. The same code, with the keys `.income_confirmation_required` and `.income_cancelled`, is the error for one interest or dividend payment of unknown type; it affects Doh-Div (only with an ISIN), Doh-DHO and Doh-Obr. |
| `unsupported_tax_treatment` | Warning for a positively identified outside-scope category; error for a needed but unimplemented calculation in an otherwise selected report. |
| `tax_category_not_reported` | Warning for an explicit classification exclusion by the user, never for a tax-rule exclusion (a bond, crypto, a Slovenian dividend payer: those are listed in `excludedItems` without a warning). Key `.user_choice`: the user left an unknown-class investment out, with the sources of the trade and action rows up to 31 December. Key `.user_choice_income`: the user left an interest or dividend payment of unknown type out of every form; key `.user_choice_income_missing_isin`: the same for a payment of a share, fund or bond that has no ISIN. |
| `dividend_payer_not_found` | Warning for Doh-Div, once per ISIN: no reference row gave the payer, or several rows disagree and the user skipped them or has not answered. The rows of the ISIN are made without payer facts and the user completes them in eDavki. Its sources are those of all the ISIN's payments in the year; with an open payer request it is linked to it. |
| `foreign_tax_evidence_required` | Warning for Doh-Div, once per run when a row has a foreign tax above 0: the user attaches the evidence of the payment and the tax in eDavki; the app attaches nothing. The sources are those of the payments with tax. |
| `official_rate_files_required` | Warning for the capital gains report: the years of the ECB rate files that the missing rates of investments sold in the year need. Key `.not_loaded` (`years`: the files are not loaded) or `.ends_early` (`years`: the files end before a trade day); `years` is a text such as `2023, 2024`. No ISIN or date; the sources are those of the trades behind the years. Given once per key after the per-ISIN `missing_exchange_rate` errors, and not given for a rate that no file can fix. |
| `missing_exchange_rate` | Error for every report that needs one currency and tax date for which no official rate was found, one error per pair with all its sources: key `.no_rate_file` (parameters `currency`, `date`, `year`: the rate file of that year is not loaded), `.rate_file_ends_early` (`currency`, `date`, `lastDate`: the file of that year ends before the date), `.no_rate_on_date` (`currency`, `date`: the loaded files have no rate for it within 4 days before the date, or the currency was stopped) or `.replaced_by_eur` (`currency`, `date`, `euroDate`). For the capital gains report the error is given per ISIN, not per pair: the first foreign trade of an ISIN that has no rate names its date and sources, and the ISIN can be excluded. A trade with no known currency has the key `.trade` (no parameters). |
| `official_rate_from_nearby_date` | Warning for every report that needs one currency and tax date with no ECB rate of its own, one per pair: the last earlier ECB rate, at most 4 days before, was used. Parameters `currency`, `date` and `rateDate`. The rate is still official; the rule is unconfirmed by FURS. |
| `official_rate_file_incomplete` | Warning, once per used rate file of a tax year that is not a whole year: key `.starts_late` (parameters `year`, `firstDate`: the file starts more than 4 days after 1 January) or `.ends_early` (`year`, `lastDate`: the file ends within 4 days before 31 December and a need after its end was accepted). Its reports and sources are those of the needs in that file's year (for `.ends_early`, those after its end). |
| `income_details_missing` | Error for the affected income form, with a details request while resolvable. |
| `interest_payer_not_found` | Warning for Doh-Obr (and Doh-DHO when the payments are deposit interest), once per ISIN of a coupon or once per broker of cash interest: no reference row gave the payer, several rows disagree and the user skipped them or has not answered, or the broker has no single company in `broker_payers.csv`. The rows are made without payer facts and the user completes them in eDavki. |
| `interest_payment_not_positive` | Warning for the form of the payment, one per payment: an interest payment of 0 or less, or one that rounds to 0.00, is not a row, is not netted against other payments and is not in the deposit total. |
| `slovenian_coupon_tax` | Warning for Doh-Obr, one per coupon row whose payer has the residence country `SI` and whose export has a tax above 0. A Slovenian withholding is not foreign tax and gives no credit; if the payer already taxed the coupon, the user leaves it out in eDavki. The row is still made. |
| `deposit_interest_add_other_banks` | Warning for Doh-DHO, once per run, after the answer `above_limit`: the form holds only the imported interest, and the user adds the interest of the other banks in eDavki. |

`broker_exchange_rate_used` is also a warning. Other previously
listed codes are errors unless their rule explicitly says otherwise. Adapted parser diagnostics
always keep their original severity. New application rules do not change the meaning of existing
parser codes.

The CLI report command already emits `unsupported_tax_treatment`,
`tax_classification_unknown` (with its request), `tax_category_not_reported`,
`missing_exchange_rate` and `corporate_action_not_applied` for capital gains, without requests
except the classification request (the others come with exchange-rate requests). It also runs the 30-day loss rule and the corporate-action stage in a
`DecisionSession` and asks their questions in the terminal; with `--no-input`, or when the input
ends, they stay open and the summary lists them. `unsupported_tax_treatment` is also a non-blocking warning for
each skipped private-market event of the selected year; a rejected private-market row or a merger
error on a private-market event affects no report. The cases and their scope are in
[`architecture.md`](architecture.md), "CLI report command (walking skeleton)".

## Message keys and translation

The app is Slovenian by default, with English as a second language. The backend therefore never
sends a sentence that the frontend has to read. Every diagnostic has:

- `messageKey`: the stable id of one situation, for example `field_count_mismatch`;
- `messageParams`: the named values the text needs, such as counts, field names, and expected and
  found values; and
- `message`: the English sentence, for logs, the CLI and developer tools. The frontend never parses
  it and never shows it when it has a translation for the key.

The code stays; the key is an addition. One code can cover several situations: `parse_error` is
used for an unclosed quote, for a line break inside a field and for an unreadable file. A key is
therefore the code alone when the code has one situation (`field_count_mismatch`), and
`<code>.<sub_code>` when it has several (`parse_error.unclosed_quote`,
`parse_error.line_break_in_field`, `parse_error.unreadable_file`). A new situation gets a new key,
even under an existing code. A key is an API value: it is never renamed, and an old key is never
reused for another meaning.

A parameter is one of three kinds:

| Kind | Meaning |
| --- | --- |
| number | A whole number. The frontend formats it. |
| text | Shown as it is and never translated, such as one of our column names. |
| term | A term id such as `row_kind.trade`. The frontend translates it from the same table. |

Terms are the next level below a key. They let one sentence say "in a dividend row" or "in a trade
row" without a sentence per row kind. Slovenian has cases, so a sentence puts a term where its base
form fits, for example after a colon. There is no plural syntax: a sentence is worded so that a
number stands alone, for example `Required columns are missing: 3 (price, amount, fx_rate)`.

The texts live in `resources/i18n/diagnostic_messages.csv`, with the header `key,en,sl`. It has
one row per message key and per term, and `{name}` marks a parameter in a text. The backend embeds
the table when it is built and uses the English column; the frontend loads the same file. The list
of every key with its parameters is in `include/taxbroker/diagnostic_messages.hpp`. Tests fail when:

- a key is missing or used twice;
- a key or a term has no row, or a row has no key;
- a text is empty; or
- the `{name}` placeholders of a text differ from the parameters of its key.

A parameter holds only what the English sentence already showed: counts, our own names and, where
the user needs it to find the problem, a source label such as a transaction type. It follows the
privacy rules below and never holds a raw row, a path or an unrelated financial value.

Every new user-facing diagnostic needs a key, parameters, and an English and a Slovenian text in
the table, in the same change. Decision requests use keys and parameters for their questions in
the same way, so the frontend builds each question from the request type and its fields, never
from a sentence.

## Deterministic ordering

The intermediate statement-merger result retains parser diagnostic creation order within each
source and deterministically orders merger diagnostics using their available fields, as described
in [`architecture.md`](architecture.md). The following full ordering applies to application
diagnostics after report scopes and diagnostics from the later stages are available.

Diagnostics are collected without relying on parser thread completion, container iteration order,
filesystem order, or log timing. Before serialization, sort them by these keys in order:

1. pipeline stage in the order listed in this document;
2. earliest source file request index, with no source last;
3. earliest source event index, with no source last;
4. tax date, with no date last;
5. affected report order: capital gains, dividends, bank-deposit interest, other interest, then no report;
6. ISIN, with no ISIN last;
7. severity, with errors before warnings;
8. diagnostic code, in the order in which the code list of the stage declares the codes (not
   alphabetical); and
9. deterministic stage-local creation sequence.

For a diagnostic with several sources, sort its source references by request index, event index,
filename, and row before using the earliest source in the diagnostic key.

The stage-local sequence must come from deterministic processing order. It must not be an atomic
counter shared by concurrent parsers. Diagnostic IDs are assigned after this sort.

User-decision requests use the same applicable source, date, report, and ISIN ordering. Report
results and `affectedReports` always use the fixed report order. Human-readable or translated
messages are never sorting keys.

## Parser API compatibility

The parser diagnostics JSON (`serializeDiagnosticsJson`, `schemaVersion` 1) is a developer format
until the beta, and no compatibility promise covers it. It keeps the existing enum values and
status values (`success`, `completed_with_warnings`, `completed_with_errors`), omits unavailable
optional fields, and gained `messageKey` and `messageParams` for each diagnostic.

The application result is a separate, versioned contract. An adapter copies parser diagnostics
into the application result, adds the `parsing` stage and report scope, and keeps their original
severity, code, message key, parameters, safe message, and source details. The adapter must not
mutate the `ParseResult`. It adds the run-local source ID from the application request.

From the first released frontend on, a breaking change to either contract requires a version change
to that contract and compatibility tests.

## JSON privacy and safety

Application JSON may expose only the information needed to identify, correct, or download a
result. A diagnostic may include:

- a run-local source ID;
- a sanitized filename;
- a source row number;
- broker and transaction ID when useful;
- field name and tax date;
- investment name and ISIN; and
- a specific value only when the user must see it to make the documented correction or decision.

It must never include:

- an absolute or parent directory path, including POSIX and Windows paths;
- the contents of a raw CSV row or a copy of all fields from that row;
- unrelated prices, quantities, balances, fees, income, tax, or calculated totals;
- taxpayer identifiers inside diagnostic messages;
- generated XML inside a diagnostic; or
- raw exception text, stack traces, library errors, or operating-system errors.

Generated XML is financial output and may appear only in the successful report payload or behind
its download reference. It is necessary output, not diagnostic context. Logs and error messages
must not copy it.

Messages are built from the controlled templates in the message table. An external exception is
mapped to a stable code, a message key and a safe message. Source values are omitted by default and
included only when a documented frontend decision cannot be made without that exact value.

Before serialization, every filename is passed through `SourceFilename::fromPath`. This rule also
applies to filenames returned by a future broker parser or application-stage error; trusting that a
string was already sanitized is not enough.

### Partial-result JSON example

This shortened example shows safe provenance and independent report status. The source row number
is present; the raw CSV row is not.

```json
{
  "schemaVersion": 2,
  "status": "needs_input",
  "runRevision": 1,
  "reports": {
    "capitalGains": {
      "status": "needs_input",
      "diagnosticIds": ["diagnostic-0001"],
      "requestIds": ["request-0001"],
      "excludedInvestments": [],
      "unsafe": false
    },
    "dividends": {
      "status": "generated",
      "diagnosticIds": [],
      "requestIds": [],
      "unsafe": false,
      "downloadAvailable": true
    },
    "bankInterest": {
      "status": "no_data",
      "diagnosticIds": [],
      "requestIds": [],
      "unsafe": false
    },
    "otherInterest": {
      "status": "no_data",
      "diagnosticIds": [],
      "requestIds": [],
      "excludedInvestments": [],
      "unsafe": false
    }
  },
  "diagnostics": [
    {
      "id": "diagnostic-0001",
      "severity": "error",
      "stage": "historical_validation",
      "code": "incomplete_history",
      "message": "Earlier purchase history is missing: this sale needs more units than the earlier purchases left. Add the older or missing statements.",
      "messageKey": "incomplete_history",
      "messageParams": {},
      "affectedReports": ["capital_gains"],
      "blocking": true,
      "sources": [
        {
          "broker": "trade_republic",
          "sourceId": "source-0002",
          "file": "TransactionExport.csv",
          "row": 42
        }
      ],
      "instrument": {
        "name": "Example Investment",
        "isin": "XX0000000001"
      },
      "requestId": "request-0001"
    }
  ],
  "requests": [
    {
      "id": "request-0001",
      "type": "capital_gains_failure_resolution_required",
      "runRevision": 1,
      "affectedReports": ["capital_gains"],
      "options": ["exclude_failed_instruments", "fail_report"],
      "investments": [
        {
          "name": "Example Investment",
          "isin": "XX0000000001",
          "reasonCodes": ["incomplete_history"],
          "diagnosticIds": ["diagnostic-0001"]
        }
      ]
    }
  ],
  "decisions": []
}
```

The generated dividend XML is still downloadable while the capital-gains question is open.

### Excluded-instrument result example

After the user chooses to continue without every failed ISIN, the capital-gains result records the
choice and the complete sorted list. For example:

```json
{
  "status": "generated",
  "failureResolution": "exclude_failed_instruments",
  "excludedInvestments": [
    {
      "name": "Example Fund",
      "isin": "XX0000000001",
      "reasonCodes": ["incomplete_history"],
      "diagnosticIds": ["diagnostic-0001"]
    },
    {
      "name": "Example Shares",
      "isin": "XX0000000002",
      "reasonCodes": ["corporate_action_ambiguous"],
      "diagnosticIds": ["diagnostic-0002"]
    }
  ],
  "unsafe": false,
  "downloadAvailable": true
}
```

If the user instead chooses to stop the capital-gains report, its status is `failed`, its
`failureResolution` is `fail_report`, `excludedInvestments` is empty, and no capital-gains XML is
available.

## Frontend behavior

The frontend must:

- show each generated download even when another report needs input or failed;
- show `no_data` as a normal empty result, not an error;
- group each diagnostic under its affected report and also allow a run-wide summary;
- show a sanitized filename and row number when available;
- show the matching file number whenever sanitized filenames collide and keep the ordered input
  list available to the user;
- allow an optional frontend-only label for same-name files while keeping the file number as the
  fallback;
- keep warnings and non-blocking errors visible beside the result they affect;
- show separate choices for capital gains and other interest between excluding every listed
  failed ISIN from that form and generating no XML for that form;
- preserve the selected choice as `failureResolution` in the report result;
- list every excluded name and ISIN before confirmation and beside the final download;
- mark developer output as unsafe;
- use structured request types and fields for user decisions;
- build the text of every diagnostic from its message key and parameters, in Slovenian by default
  or in English; and
- use a clear general message only when a more specific diagnostic is unavailable.

The frontend must not parse logs, diagnostic message wording, or exception text to decide what to
show or which action to offer.

## Result examples

### Complete success

Capital gains is `generated`, dividends is `generated`, and both interest forms are `no_data`.
There are no diagnostics. Overall status is `success`.

### Waiting for one decision

Capital gains has incomplete history and is `needs_input`. Dividends is `generated`, and both
interest forms are `no_data`. Overall status is `needs_input`; the dividend download remains available.

Assuming no unresolved dependent loss checks or other errors, after the user chooses exclusion,
capital gains is generated without every ISIN listed in the request. The final result lists them
all, their errors remain non-blocking, and overall status is
`completed_with_warnings`. If the user chooses `fail_report`, capital gains is `failed`, but the
dividend download remains available.

### Partial success

A failed bond ISIN initially leaves Doh-Obr `needs_input`, while valid Doh-DHO is downloadable.
If the user excludes it, unaffected bond coupons and broker cash interest produce Doh-Obr with a
visible exclusion list. If nothing remains, Doh-Obr is `no_data` with the same exclusion record.
Choosing to block the form instead gives Doh-Obr `failed`; Doh-DHO remains available in either case.

Capital gains is `generated`, dividend XML schema validation fails, and both interest forms are `no_data`.
Only dividends is `failed`; the capital-gains download remains available. Overall status is
`partial_success`.

### Complete failure

A file-level parse failure prevents the app from knowing which report types the file contained.
All four report results are `failed`, each refers to the parsing diagnostic, and overall status is
`failed`.

## Required tests

Application-service, API, and integration tests based on this document must prove that:

- all-generated, all-empty, complete-failure, and partial-success results are representable;
- every combination of one failed report and three valid reports keeps the valid results available;
- one open request produces `needs_input` without hiding already generated XML;
- warnings never block a report;
- a blocking error cannot produce an ordinary generated result;
- normal incomplete-history exclusion keeps the error, makes it non-blocking, and removes the
  whole affected ISIN from report output while retaining evidence needed by other losses;
- one resolution request lists every safely isolatable failed ISIN and offers only exclusion of
  all listed ISINs or failure of the capital-gains report;
- choosing exclusion lists every excluded name, ISIN, reason code, and diagnostic ID in the final
  report result;
- choosing report failure creates no capital-gains XML and does not block valid income reports;
- both choices are recorded exactly in `failureResolution`, which is absent when no choice was
  needed;
- errors without a complete, safely isolatable ISIN do not offer the exclusion choice;
- one non-isolatable blocking capital error suppresses the exclusion choice and fails capital
  gains even when other errors have ISINs;
- the three-confirmation override is the only way to generate unsafe incomplete-history output;
- a capital error does not block valid dividend or interest XML;
- dividend and interest errors do not block each other or valid capital XML;
- Doh-DHO and Doh-Obr have independent statuses and downloads;
- broker cash interest and bond coupons share the Doh-Obr result but retain separate subtypes;
- a safely isolated bond-income error requests the Doh-Obr exclusion/failure choice with no
  preselection; cancellation generates no XML and changed inputs invalidate the choice;
- Doh-Obr exclusion removes all selected-year income rows for each failed ISIN across brokers,
  preserves unaffected income, records non-blocking errors and yields `no_data` if nothing remains;
- choosing to fail Doh-Obr keeps errors blocking and records no exclusions;
- capital and other-interest exclusions are independent, preserve sources, and do not clear
  shared errors in other reports;
- non-isolatable income errors and final XML failures cannot be bypassed by ISIN exclusion;
- below-threshold bank interest records its reason, total and external-interest reminder;
- missing known external income details prevent a falsely complete bank-interest XML;
- every documented request type has structured question data and validates revision-bound answers;
- stale, replayed, repeated and unfitting answers are rejected as a whole without a recalculation,
  and a reused request ID never answers a different question;
- a recorded answer applies again only to the same question, is listed in `decisions` with
  `fromEarlierRun` and `used`, and can be deleted;
- classification exclusions remain visible separately from each form's failure exclusions;
- action-source mismatch warnings show every relevant source without blocking on their own;
- each XML generator can fail without changing another report result;
- an empty selected year returns `no_data` and is successful;
- diagnostics and IDs are identical across different parser completion orders;
- excluding an ISIN preserves replacement evidence needed by retained losses, and unresolved
  dependencies keep capital gains `needs_input` until resolved or explicitly excluded in turn;
- a dividend of a payer with the residence country `SI` is visibly excluded, several payer rows give a
  skippable pick request that never blocks, and requested treaty claims produce an income-details request for the
  affected form;
- supporting-document requirements remain visible alongside generated income XML;
- conflicts retain all source references in deterministic order;
- POSIX and Windows input paths serialize only their filename;
- equal sanitized filenames receive different deterministic source IDs and display file numbers;
- reordering input files deterministically changes their run-local source IDs;
- no directory or parent-folder name is used to distinguish equal filenames;
- an optional frontend-only label maps to the correct source ID without entering diagnostic JSON
  or processing logic;
- the source row number is preserved and raw CSV row contents are absent;
- diagnostics contain no unnecessary financial values, generated XML, stack traces, or raw
  exception text;
- optional fields are omitted when unavailable;
- the overall status follows the stated precedence;
- every diagnostic the code can emit has a message key, and a missing or duplicate key fails a test;
- each key has an English and a Slovenian text, and the placeholders match its parameters;
- each diagnostic reports the expected key and parameters, for example a wrong field count gives
  `field_count_mismatch` with `expected` and `found`; and
- the parser diagnostics JSON sends the key and parameters.
