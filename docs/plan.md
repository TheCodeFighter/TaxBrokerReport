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
- capital-gains, dividend, and interest reports;
- standalone FURS-compatible XML generation;
- structured diagnostics returned to the frontend;
- a local backend and browser frontend; and
- downloading each successfully generated XML file.

IBKR parsing and performance optimizations are not required to complete this MVP.

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
- the source CSV row;
- the broker transaction ID; and
- a stable input sequence.

The timestamp may be optional because not every future broker is guaranteed to provide one. Stable
ordering must therefore have a documented fallback based on source provenance and input sequence.
Timezone parsing and normalization must not change the broker-provided tax date.

Monetary values, quantities, exchange rates, and corporate-action ratios remain fixed-point values.
Rounding is performed only at the processing or output boundary defined by the applicable rule.

## Diagnostics

The existing parser diagnostics remain a stable frontend contract. Introduce a broader application
result for diagnostics produced by later pipeline stages.

Diagnostics must identify their stage, such as:

- parsing;
- merging and duplicate detection;
- historical-data validation;
- corporate-action processing;
- FIFO and tax processing; or
- XML generation.

Where available, diagnostics include broker, source filename, source row, transaction ID, and field
name. They must not expose absolute paths or duplicate sensitive raw financial values.

Warnings allow processing to continue. An error blocks only the affected output XML file. For
example, incomplete capital-gains history blocks the capital-gains XML but does not prevent valid
dividend or interest XML files from being generated. The API returns both successful outputs and
all diagnostics so the frontend can explain partial success.

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
- preservation of source provenance; and
- deterministic diagnostic and output ordering regardless of parser completion order.

The merger creates a combined chronological ledger but does not calculate tax or resolve the effect
of corporate actions on FIFO inventory. Those operations require historical position state and
belong to the following processing stage.

## Reporting-year preparation

The user can select any reporting tax year. Processing must not assume a particular calendar year.

For selected year `Y`:

- ignore events after December 31 of `Y` for that report run;
- process all supplied trades before `Y`, including both acquisitions and disposals, because they
  establish the opening FIFO inventory;
- process all relevant corporate actions through December 31 of `Y`;
- process events during `Y` chronologically; and
- include only reportable outcomes belonging to `Y` in the generated report models.

Filtering must therefore occur at the ledger/output stages, not by discarding all rows outside the
selected year immediately after parsing.

If a disposal cannot be matched because acquisition history is missing, produce a structured
incomplete-history error. Do not silently invent a cost basis or generate an affected XML file from
known incomplete data.

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

Detailed, source-backed rules belong in `docs/fifo.md` and `docs/tax_rules.md`. Legacy behavior can
be used as a tested reference, but the new implementation and tests must state the rules explicitly.

## Final report models

Tax processing produces broker-neutral report structures rather than writing XML directly. Define
separate models for:

- capital gains;
- dividends;
- interest; and
- shared taxpayer and reporting metadata.

These structures form the boundary between tax calculations and XML serialization. They must not
contain CSV-specific or frontend-specific fields.

## XML generation

Port the established behavior from the legacy implementation into standalone generators in the new
backend. Do not call or link legacy code.

Verify each generated report with:

- golden XML fixtures derived from known-good behavior;
- the applicable FURS XSD;
- exact fixed-point formatting and rounding tests;
- XML escaping and non-ASCII text tests;
- optional and empty-value cases;
- multiple instruments and transactions; and
- isolated failures for capital-gains, dividend, and interest outputs.

A failure in one generator blocks only its affected XML file and is returned through structured
diagnostics. Other valid report files remain available.

## Backend application service and API

Before implementing the frontend, connect the full backend behind one application-level operation.
Its conceptual input is:

- input files;
- selected reporting year; and
- taxpayer and report metadata.

Its result contains:

- parse and processing diagnostics;
- status for each report type; and
- every successfully generated XML document.

Exercise this complete operation through unit and integration tests before exposing it through the
local HTTP API. The API serializes the application result but does not contain parsing, tax, or XML
business logic.

## Frontend

Build the minimum usable local frontend against the tested API contract. It must allow the user to:

- select one or more broker files;
- select the reporting tax year;
- enter required taxpayer and report metadata;
- start processing;
- see warnings and errors with useful source locations;
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
8. Produce selected-year capital-gains, dividend, and interest report models.
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
- incomplete history is visible in the frontend and blocks only affected XML files;
- capital-gains, dividend, and interest XML files match ported known-good behavior and validate
  against their schemas;
- the complete backend pipeline is covered by integration tests;
- the local frontend can run the pipeline and download successful files; and
- the new application has no dependency on the legacy project.
