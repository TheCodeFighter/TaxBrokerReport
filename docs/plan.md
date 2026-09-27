# TaxBrokerReport implementation plan

## Goal

Build a complete, local-first application that imports Trade Republic CSV exports and produces
FURS-compatible XML reports for a selected tax year. Interactive Brokers support will be added
after the Trade Republic workflow is complete.

The processing pipeline must remain broker-neutral after parsing so that adding another broker
requires a new parser and broker-specific validation, without changing merging, tax processing,
XML generation, the API, or the frontend.

## MVP scope

The first complete version supports:

- one or more Trade Republic CSV files;
- overlapping exports from the same broker;
- deterministic merging and duplicate detection;
- historical FIFO processing across all supplied data;
- corporate actions that affect FIFO inventory;
- selection of any requested reporting tax year;
- capital-gains, dividend, bank-deposit-interest, and other-interest reports, including ordinary
  bond coupons;
- standalone FURS-compatible XML generation;
- structured diagnostics returned to the frontend;
- a local backend and browser frontend; and
- downloading each successfully generated XML file.

IBKR parsing and performance optimizations are not required to complete this MVP. Plan the
broker-neutral other-interest model and Doh-Obr generator now, including ordinary bond coupons,
using synthetic fixtures when a live export is unavailable. Slovenian retail government bonds
and their special interest treatment are outside the current TR/IBKR import scope.

## Architectural principles

### Broker-neutral pipeline

Broker parsers convert source files into a common domain model. Every later stage operates only on
that common model:

```text
Broker CSV files
    -> broker parsers
    -> parsed statements
    -> deterministic merge and deduplication
    -> chronological ledger through the selected year
    -> corporate-action and FIFO processing
    -> selected-year report models
    -> FURS XML files
    -> local API and frontend
```

### Standalone replacement for the legacy project

The legacy project is a behavioral reference for established calculations and XML that has been
accepted by eDavki. All required behavior must be ported into the new implementation.

The new application must not have a runtime, build, packaging, or data dependency on the legacy
project. Tests and documentation in the new application must preserve the required behavior so the
legacy tree can eventually be deleted.

### Correctness before concurrency

Merging, tax processing, and XML generation are deterministic and single-threaded unless profiling
later proves that an additional optimization is necessary. Initial concurrency is limited to
parsing independent input files.

## Domain model changes

Each imported event must retain enough information for tax reporting, deterministic ordering,
duplicate handling, and user-facing diagnostics.

Retain:

- the calendar date used for tax reporting;
- the complete source timestamp when the broker supplies one;
- the broker;
- the source filename, without exposing an absolute path;
- a run-local source ID derived from file request order, so equal filenames remain distinguishable;
- the source CSV row;
- the broker transaction ID; and
- a stable input sequence.

The timestamp may be optional because not every future broker is guaranteed to provide one. Stable
ordering must therefore have a documented fallback based on source details and input sequence.
Timezone parsing and normalization must not change the broker-provided tax date.

Monetary values, quantities, exchange rates, and corporate-action ratios remain fixed-point values.
Rounding is performed only at the processing or output boundary defined by the applicable rule.

## Diagnostics

The existing parser diagnostics remain a stable frontend contract. Introduce a broader application
result for diagnostics produced by later pipeline stages. The complete contract is defined in
[`diagnostics.md`](diagnostics.md).

Diagnostics must identify their stage, such as:

- parsing;
- merging and duplicate detection;
- historical-data validation;
- corporate-action processing;
- FIFO and tax processing; or
- XML generation.

Where available, diagnostics include broker, source filename, source row, transaction ID, and field
name. They must not expose absolute paths or duplicate sensitive raw financial values.

Warnings allow processing to continue. Errors have an explicit scope. Each capital-gains, dividend,
bank-deposit-interest, and other-interest result is independently `generated`, `no_data`,
`needs_input`, or `failed`.
For safely isolatable capital errors, one request lists every affected ISIN. The user either
excludes all listed ISINs or fails the capital-gains report. The result records that choice and
lists every excluded ISIN, while valid dividend or interest XML files can still be generated. The
API returns both successful outputs and all diagnostics so the frontend can explain exclusions and
partial success. Doh-DHO and Doh-Obr have independent status and failure scope. Ordinary bond
coupons and broker cash interest have separate subtypes but share the Doh-Obr output.

Safely isolated Doh-Obr ISIN errors use a separate
`other_interest_failure_resolution_required` request: exclude all listed ISINs' income from
Doh-Obr or fail that form. Before the answer it is `needs_input`; exclusion keeps valid remaining
income and records the full exclusion list, or returns `no_data` if empty. The choice and
`excludedInvestments` belong to `otherInterest` and never remove rows from another form.
Non-isolatable errors and final XML failures retain their normal scope.

The application result also carries structured requests for missing user decisions. When a Trade
Republic split row has no verified ratio, the frontend asks for the number of new shares and old
shares from the broker's announcement. The backend validates the ratio and never infers it from an
undocumented decimal value.

The same result carries revision-bound corporate-action identity, quantity-correction,
loss-eligibility, tax-classification and missing-income-detail requests. Action records for one
ISIN from different sources within one calendar year always produce a potential-mismatch warning.
One identified action adjusts all brokers' lots once; grouping by year never deduplicates actions.
Explicit category exclusions remain visible separately from per-report failed-ISIN exclusions.

Exclusions affect report rows while preserving evidence needed for other loss checks. Unresolved
dependencies remain structured loss requests, including dependencies on an excluded instrument.
Apply corporate-action source warnings to relevant following-January loss-analysis events too.

The same result carries missing exchange-rate requests grouped by currency and tax date. Official
Banka Slovenije input is the default and is reused for that exact pair across the current run. A
broker-rate fallback requires a prominent explicit warning and remains event-specific.

## Multi-file merging

Implement merging as a deterministic, single-threaded operation.

The merge contract must define and test:

- instrument grouping by ISIN;
- aggregation of events from multiple files;
- exact duplicates from overlapping exports;
- conflicting events that share the same broker and transaction ID;
- transaction-ID scope by broker;
- conflicting instrument names or asset classes;
- stable ordering of events with equal dates or timestamps;
- preservation of source references; and
- deterministic diagnostic and output ordering regardless of parser completion order.

The merger creates a combined chronological ledger but does not calculate tax or resolve the effect
of corporate actions on FIFO inventory. Those operations require historical position state and
belong to the following processing stage.

## Reporting-year preparation

The user can select any reporting tax year. Processing must not assume a particular calendar year.

For selected year `Y`:

- exclude events after December 31 of `Y` from its FIFO ledger and report output;
- retain a separate analysis view for loss checks spanning preceding December and following
  January, without changing the year-end FIFO position;
- process all supplied trades before `Y`, including both acquisitions and disposals, because they
  establish the opening FIFO inventory;
- process all relevant corporate actions through December 31 of `Y`;
- process events during `Y` chronologically; and
- include only reportable outcomes belonging to `Y` in the generated report models.

Filtering must therefore occur at the ledger/output stages, not by discarding all rows outside the
selected year immediately after parsing.

If a disposal cannot be matched because acquisition history is missing, produce a structured
incomplete-history error. In normal mode, identify the investment by name and ISIN and require user
confirmation before excluding every listed unsafe ISIN; the other normal choice is to generate no
capital-gains XML. Developer mode may include known incomplete data only after three separate
warnings. Neither mode may invent a cost basis.

## Tax processing rules

Document and implement the following rules before considering the processor complete:

- FIFO inventory spans all brokers and accounts for the same instrument;
- corporate actions are applied at their chronological position while historical inventory is
  built;
- conversion into the required reporting currency uses explicit fixed-point rules;
- rounding precision, stage, and direction are explicit and tested;
- broker fees remain preserved in imported data but are not added as separately claimed costs in
  FURS calculations because the applicable FURS deduction is handled by FURS; and
- incomplete or contradictory history produces structured errors.

Loss eligibility must allocate replacement quantities chronologically, distinguish partially
eligible losses, and populate the XML eligibility field explicitly. Missing coverage or external
activity requires a structured decision. Preserve exact lot basis through partial sales and splits;
accept bounded, documented rounding adjustments with a cumulative absolute impact below EUR 0.01
per ISIN/run. Corrections are run-local overlays that rebuild affected history. Developer output
has a separate, deterministic known-source-event export and cannot bypass other errors.

Detailed, source-backed rules belong in `docs/fifo.md`, `docs/tax_rules.md`, and
`docs/calculations.md`. Legacy behavior can be used as a tested reference, but the new
implementation and tests must state the rules explicitly.

### Payer reference data and income-tax relief

Maintain a Slovenian-company reference list and import the company/ETF catalogue from the other
project designated by the user. Before importing, identify its URL/path, revision and reuse licence;
the current legacy tree has no identified matching catalogue. Package the reusable entries as
standalone offline resources with provenance and effective dates. Missing entries use manual
income details, so catalogue coverage never decides whether an income is taxable.

Use exact identifiers to prefill verified payer metadata and relevant treaty references. Verify
payment-level Slovenian final withholding before excluding an income with `slovenian_tax_settled`.
For foreign-tax relief, preserve gross income and actual foreign tax, distinguish a credit from
a treaty exemption, and list the required filing evidence. A list match does not grant relief.
Implement the missing/conflicting-information flow, source-revision invalidation and synthetic
scenarios specified in `tax_rules.md`; catalogue import remains a tracked implementation task.

## Final report models

Tax processing produces broker-neutral report structures rather than writing XML directly. Define
separate models for:

- capital gains;
- dividends;
- bank-deposit interest;
- other interest, with broker-cash and ordinary-bond-coupon subtypes; and
- shared taxpayer and reporting metadata.

These structures form the boundary between tax calculations and XML serialization. They must not
contain CSV-specific or frontend-specific fields. Income models retain payer and source countries,
gross income, foreign tax and form-specific grouping. Date-specific classification and the shared
bank-interest allowance are applied before XML generation. Below-threshold TR totals retain the
reminder about other banks; known external interest above the threshold requires complete details.

Income models also retain withholding jurisdiction/status, applicable confirmed treaty claims,
catalogue provenance and evidence references. Verified settled Slovenian income is visibly excluded
without removing capital trades in that issuer. XML exports include the required foreign-tax and
claim fields; the download view explains which supporting evidence still needs attaching in eDavki.

## XML generation

Port the established behavior from the legacy implementation into standalone generators in the new
backend. Do not call or link legacy code. Doh-DHO is the bank-deposit form available in the legacy
reference. Doh-Obr is new work for broker cash interest and ordinary bond coupons; confirm its
schema and interest-type codes from official instructions. Use one result per form, not one per
broker or income subtype. Copy necessary official schemas into standalone resources so checks do
not depend on the legacy tree. Track real bond-export and eDavki verification separately from
synthetic tests when no live example exists.

Verify each generated report with:

- golden XML fixtures derived from known-good behavior;
- the applicable FURS XSD;
- exact fixed-point formatting and rounding tests;
- XML escaping and non-ASCII text tests;
- optional and empty-value cases;
- multiple instruments and transactions; and
- isolated failures for all four form outputs, including independent Doh-DHO and Doh-Obr failures.

Verify Doh-Obr's explicit ISIN exclusion/failure flow, including preservation of unaffected bond
coupons and broker cash interest, all-excluded `no_data`, stale answers, visible excluded ISINs and
independence from capital-gains exclusions. Final XML failures cannot use this exclusion flow.

A failure in one generator blocks only its affected XML file and is returned through the
structured result defined in [`diagnostics.md`](diagnostics.md). Other valid report files remain
available.

## Backend application service and API

Before implementing the frontend, connect the full backend behind one application-level operation.
Its conceptual input is:

- input files;
- selected reporting year; and
- taxpayer and report metadata.

Its result contains:

- parse and processing diagnostics;
- structured, revision-bound requests for missing official rates, action identity/ratios,
  quantity correction, loss eligibility, classification and income details;
- explicit classification exclusions, recorded corrections and reconciliation totals;
- qualifying bank-interest totals and the other-bank reminder;
- informational fee totals by broker and original currency, with complete broker-rate EUR totals
  where available;
- status for each report type; and
- every successfully generated XML document.

The result and its JSON boundary follow [`diagnostics.md`](diagnostics.md), including deterministic
diagnostic ordering, safe source-row locations, independent report status, and partial success.

Exercise this complete operation through unit and integration tests before exposing it through the
local HTTP API. The API serializes the application result but does not contain parsing, tax, or XML
business logic.

## Frontend

Build the minimum usable local frontend against the tested API contract. It must allow the user to:

- select one or more broker files;
- select the reporting tax year;
- enter required taxpayer and report metadata;
- resolve action identity and ratios for the complete ISIN pool;
- see potential corporate-action mismatches across different sources in the same year;
- confirm source corrections and see their recorded quantity and EUR impact;
- confirm loss-check coverage across reporting-year boundaries and declare external activity;
- classify unknown instruments or explicitly exclude them, with a list of missing fields;
- independently choose exclusion of all listed failed ISINs or failure for Doh-KDVP and Doh-Obr,
  and see each form's exclusions beside its download;
- review company/ETF payer suggestions, resolve withholding and treaty-claim facts, and see the
  evidence required for foreign-tax relief and any settled-Slovenian-tax exclusions;
- see separate bank-deposit, broker-cash and ordinary-bond-interest summaries;
- see the bank-interest allowance reminder and optionally add other qualifying bank interest;
- enter each missing official Banka Slovenije rate once per currency and date;
- choose a broker-rate fallback only after a prominent warning;
- start processing;
- see warnings and errors with useful source locations;
- see selected-year fees by broker and original currency, with broker-rate EUR totals only when
  complete and without any rate prompt for fees;
- understand which report files succeeded or failed; and
- download every successfully generated XML file.

Additional visual polish and convenience features follow only after this end-to-end workflow works.

## Concurrency

Add bounded concurrency only for parsing multiple independent files, after the single-threaded full
pipeline is correct and tested.

- Submit independent files to a bounded worker pool.
- Each parser returns an owned result without shared mutable state.
- Collect results in stable input-file order rather than task-completion order.
- Perform merging deterministically on one thread.
- Keep diagnostics and final output ordering identical between sequential and concurrent parsing.
- Measure performance and avoid creating more work than the number of files or configured workers.

Do not divide merging into concurrent jobs by trades, dividends, corporate actions, and interest.
Trades and corporate actions are state-dependent, while the expected workload for independent
income collections does not justify synchronization and nondeterministic error handling.

## IBKR extension

Add Interactive Brokers only after the Trade Republic application is complete.

The IBKR work consists of:

1. obtaining privacy-safe examples that define the actual export contract;
2. creating comprehensive synthetic fixtures;
3. implementing broker-specific parsing and validation;
4. mapping IBKR data into the existing common domain model; and
5. running the unchanged merge, tax, XML, API, and frontend pipeline against mixed-broker tests.

If downstream components require broker-specific changes during this phase, treat that as an
architecture problem and first determine whether the common domain model is missing a genuine
broker-neutral concept.

## Implementation sequence

1. Define the detailed Trade Republic MVP completion criteria.
2. Extend the event model with source timestamp, broker, filename, row, transaction ID, and stable
   sequence while retaining the tax date.
3. Document FIFO, corporate-action, conversion, rounding, fee, and incomplete-history rules.
4. Define pipeline-level diagnostics and partial-success behavior.
5. Implement and test deterministic multi-file merging and deduplication.
6. Implement the chronological ledger limited by the selected reporting year's end.
7. Apply historical corporate actions and build cross-broker FIFO state.
8. Produce selected-year capital-gains, dividend, bank-deposit and other-interest report models.
9. Port standalone XML generation from verified legacy behavior.
10. Add golden-output, XSD-validation, boundary, and failure-isolation tests.
11. Connect the complete backend through one application-service operation.
12. Add full-pipeline integration tests without a frontend.
13. Expose the application service through the local API.
14. Build the minimum usable frontend and connect it to the API.
15. Package and test the complete local application.
16. Add bounded concurrent parsing while preserving deterministic results.
17. Define and implement the IBKR parser from privacy-safe fixtures.
18. Verify mixed-broker FIFO and reports through the unchanged downstream pipeline.

## MVP completion criteria

The Trade Republic MVP is complete when:

- multiple overlapping TR exports can be imported without double-counting;
- results are deterministic across repeated runs;
- selected-year processing uses all supplied relevant history and excludes later events;
- corporate actions and FIFO inventory produce tested outcomes;
- incomplete history is visible in the frontend, and affected ISINs can be explicitly excluded
  without blocking unrelated reports;
- Doh-KDVP, Doh-Div and Doh-DHO match verified reference behavior and their applicable schemas;
- Doh-Obr, including ordinary bond coupons, follows official instructions and validates using
  synthetic fixtures, with unavailable live-example verification explicitly tracked;
- isolated Doh-Obr ISIN errors require an explicit exclusion/failure choice, preserve unrelated
  income and reports, and expose the excluded ISINs and reasons;
- loss eligibility includes adjacent-year and quantitative scenarios;
- corporate actions apply once across brokers and show same-year source-mismatch warnings;
- rounding corrections, classifications, income details and external bank interest have explicit
  structured decisions, with recorded exclusions and adjustment budgets;
- the complete backend pipeline is covered by integration tests;
- the local frontend can run the pipeline and download successful files; and
- the new application has no dependency on the legacy project.
