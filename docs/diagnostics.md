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
- informational income totals, fee totals, and recorded run-local decisions and corrections.

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
`interest` result changes its shape and API values. Parser JSON remains version 1. This is a
specification for future application implementation, not a change to the existing parser API.

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
  bank-interest `no_data` result.

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

`unsafe` is `false` unless the user completes the three developer-mode confirmations for
incomplete history. Excluding an unsafe ISIN in normal mode does not make the remaining XML
unsafe.

`failureResolution` is either `exclude_failed_instruments` or `fail_report`. It is present only
after the user answers that report's `capital_gains_failure_resolution_required` or
`other_interest_failure_resolution_required`; otherwise it is omitted. Each report owns its own
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
- a short user-facing message;
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
| A dividend calculation error | Dividends only. |
| A Doh-Obr error safely isolated to a complete ISIN | Other interest only, with the exclusion/failure choice below. |
| Another interest calculation error | Its established form; both interest forms when the subtype is unknown. |
| XML serialization or schema failure | Only the XML file being generated. |

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
report again. A choice applies only to the exact affected-ISIN list in the current run. Changed
input or a changed list requires a new answer.

When the user chooses exclusion, the processor removes every purchase, sale, corporate action,
and calculated capital match for every listed ISIN from report output. It preserves source evidence
and replacement allocations needed by retained losses in the separate loss-analysis view.
An unresolved dependency produces `loss_eligibility_confirmation_required` for the retained sale;
exclusion of its source ISIN does not resolve that request. Cancellation may add the dependent ISIN
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

### Choosing whether to exclude failed other-interest ISINs

Collect all Doh-Obr errors that can be isolated safely to complete ISINs, including bond-income
parsing, duplicate-conflict and calculation errors whose form and instrument are established.
Offer missing-data correction requests where the facts can be supplied. If the error remains or
the user cannot supply the facts, return one `other_interest_failure_resolution_required` request
with `affectedReports: [other_interest]`, the run revision, source references and all failed
investments sorted by ISIN and then name. Each entry has its name, ISIN, reason codes and diagnostic
IDs. The request has two choices, with neither preselected:

1. `exclude_failed_instruments`: exclude every listed ISIN's selected-year Doh-Obr income rows
   across all supplied brokers and accounts, including otherwise valid rows for those ISINs;
2. `fail_report`: produce no Doh-Obr XML for this run.

Before an answer, `otherInterest` is `needs_input` and has no XML. The user may instead correct
the input. Cancelling the choice is not exclusion or permission to generate a partial return.
Changed input or a changed failed-ISIN list invalidates the answer and requires a new choice.

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
exclusion, or vice versa. When a source problem also affects another form, retain a separate scoped
blocking diagnostic there; resolving Doh-Obr must not clear it.

This choice requires known instrument identity and a safely separable Doh-Obr scope. It cannot
resolve missing ISINs, unclassified rows, cash-interest errors without an ISIN, file-level or shared
metadata errors, or final XML serialization/schema/range failures. Such problems retain their
normal correction request or report-failure behavior. An unresolved non-isolatable blocking error
prevents offering an exclusion choice that could not produce a valid result. Developer mode cannot
bypass any other-interest error. Classification exclusions remain a separate documented flow.

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

| Type | Required question data and answer |
| --- | --- |
| `exchange_rates_required` | Missing currency/date pairs, usage and any verified event-specific fallback; answer with official pair rates or explicit fallback acknowledgements. |
| `corporate_action_identity_required` | Candidate actions with ISIN, dates, types, ratios and all sources; answer with confirmed grouping, effective dates and order where needed. |
| `corporate_action_ratio_required` | Combined action identity, complete source set, original source value, old position and examples; answer with positive whole-number new/old shares and confirmation of the previewed position. |
| `quantity_correction_required` | Source event, original/proposed quantity, evidence, difference, resulting position and EUR impact/budget; answer with confirmed proposed quantity or a validated correction and supporting assertion. |
| `loss_eligibility_confirmation_required` | Loss sales, inclusive window dates, coverage gaps, known replacement allocations, dependencies on excluded items and declarations needed; answer with corrected evidence, coverage confirmation where sufficient, or declared disqualifying activity and quantities. |
| `tax_classification_required` | Investment/event identity, current category, safe sources, permitted treatment choices and treatment-specific missing fields; answer with classification, required facts, or explicit exclusion and reason. |
| `income_details_required` | Income items and missing or conflicting payer/catalogue, country, gross amount, withholding jurisdiction/status, payment evidence or requested treaty-claim facts, including external deposit interest; answer with validated details or explicit withdrawal of an optional exemption claim. |
| `capital_gains_failure_resolution_required` | Exact complete unsafe-ISIN list, diagnostic IDs and the two existing exclusion/failure choices; answer with one choice for the whole list. |
| `other_interest_failure_resolution_required` | Exact complete failed Doh-Obr ISIN list, safe sources, reason codes and diagnostic IDs; answer with `exclude_failed_instruments` or `fail_report` for that form and the whole list. |

Confirmation controls have no default answer. Optional entry of other bank interest and acknowledgement
of a potential corporate-action mismatch are frontend actions supported by structured result fields;
a non-blocking reminder does not manufacture an unanswered blocking request.

`excludedItems` entries contain the parsed item/instrument identity, established category, explicit
exclusion reason, safe sources and diagnostic IDs. Record whether the exclusion follows a verified
tax rule or an explicit user choice. Reason `slovenian_tax_settled` requires payment-specific
evidence; company-list membership alone cannot establish it. Capital and other-interest failure
exclusions use each report's own `excludedInvestments`. A classification request does not override malformed source rows, missing
instrument identity or undocumented tax calculations. A known unsupported category can be shown
without blocking unrelated reports; an unknown category cannot be silently assigned no scope.

## Diagnostic codes

Codes are stable API values. New behavior gets a new code; an existing code must not be reused with
a different meaning.

The parsing stage keeps the existing codes `unknown_row_type`, `unsupported_row_type`,
`unsupported_asset_class`, `missing_field`, `invalid_value`, `inconsistent_value`, and
`parse_error`.

The later stages use the codes defined by their rule documents. These include:

- `conflicting_duplicate` for two records with the same broker transaction identity but different
  event data;
- `invalid_reporting_year`, `quantity_rounding_reconciled`, and `incomplete_history` for
  historical validation;
- the `corporate_action_*` and `unsupported_corporate_action` codes in
  [`fifo.md`](fifo.md);
- `missing_exchange_rate`, `invalid_exchange_rate`, `broker_exchange_rate_used`,
  `unrepresentable_value`, and `arithmetic_overflow` in [`calculations.md`](calculations.md); and
- `xml_value_out_of_range`, `xml_serialization_failed`, and `xml_schema_validation_failed` for the
  final XML boundary.

Additional codes and their default scope are:

| Code | Severity and scope |
| --- | --- |
| `corporate_action_sources_mismatch_possible` | Warning for capital gains; one per ISIN/year with action records from multiple sources used by the ledger or loss analysis, including relevant following-January actions. |
| `corporate_action_quantity_rounded` | Warning for capital gains; bounded split-quantity rounding with its recorded impact. |
| `source_quantity_corrected` | Warning for reports using the corrected source; retain the original and corrected quantities. |
| `xml_basis_rounding_difference` | Warning for capital gains; exact basis differs from reconstructed rounded XML value. |
| `loss_eligibility_unresolved` | Error for capital gains, isolated to its complete ISIN where possible. |
| `tax_classification_unknown` | Error for the potentially affected forms, with a classification request while resolvable. |
| `unsupported_tax_treatment` | Warning for a positively identified outside-scope category; error for a needed but unimplemented calculation in an otherwise selected report. |
| `tax_category_not_reported` | Warning explaining an applicable documented exemption or explicit classification exclusion. |
| `income_details_missing` | Error for the affected income form, with a details request while resolvable. |
| `deposit_interest_external_total_unchecked` | Warning for bank interest; remind the user of qualifying interest at other banks. |

`quantity_rounding_reconciled` and `broker_exchange_rate_used` are also warnings. Other previously
listed codes are errors unless their rule explicitly says otherwise. Adapted parser diagnostics
always keep their original severity. New application rules do not mutate parser version 1 codes.

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
8. diagnostic code; and
9. deterministic stage-local creation sequence.

For a diagnostic with several sources, sort its source references by request index, event index,
filename, and row before using the earliest source in the diagnostic key.

The stage-local sequence must come from deterministic processing order. It must not be an atomic
counter shared by concurrent parsers. Diagnostic IDs are assigned after this sort.

User-decision requests use the same applicable source, date, report, and ISIN ordering. Report
results and `affectedReports` always use the fixed report order. Human-readable or translated
messages are never sorting keys.

## Parser API compatibility

The existing parser diagnostics API remains version 1. This issue does not change:

- `ParseDiagnostic` or its existing enum values;
- `serializeDiagnosticsJson`;
- parser JSON field names;
- the parser status values `success`, `completed_with_warnings`, and `completed_with_errors`; or
- how optional parser fields are omitted.

The application result is a separate, versioned contract. An adapter copies parser diagnostics
into the application result, adds the `parsing` stage and report scope, and keeps their original
severity, code, safe message, and source details. The adapter must not mutate the `ParseResult`.
It adds the run-local source ID from the application request; parser JSON version 1 does not gain a
new field.

Changing the application result does not silently change parser JSON version 1. A breaking change
to either contract requires a version change to that contract and compatibility tests.

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

Messages are built from controlled templates. An external exception is mapped to a stable code and
a safe message. Source values are omitted by default and included only when a documented frontend
decision cannot be made without that exact value.

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
      "unsafe": false
    }
  },
  "diagnostics": [
    {
      "id": "diagnostic-0001",
      "severity": "error",
      "stage": "historical_validation",
      "code": "incomplete_history",
      "message": "Cannot process Example Investment because earlier purchase history is missing.",
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
  ]
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
- use structured request types and fields for user decisions; and
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
- classification exclusions remain visible separately from each form's failure exclusions;
- action-source mismatch warnings show every relevant source without blocking on their own;
- each XML generator can fail without changing another report result;
- an empty selected year returns `no_data` and is successful;
- diagnostics and IDs are identical across different parser completion orders;
- excluding an ISIN preserves replacement evidence needed by retained losses, and unresolved
  dependencies keep capital gains `needs_input` until resolved or explicitly excluded in turn;
- settled Slovenian income is visibly excluded only with evidence, and unresolved withholding or
  requested treaty claims produce an income-details request for the affected form;
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
- the overall status follows the stated precedence; and
- the existing parser diagnostics JSON remains byte-for-byte compatible for the same
  `ParseResult`.
