# Architecture

TaxBrokerReport is intended to run entirely on the user's machine. The C++20 backend owns broker
parsing, validation, tax processing, and XML generation. A browser frontend will call the local
backend once an HTTP framework and frontend stack are selected.

The repository does not currently define a frontend framework or an HTTP transport. Code should
therefore keep domain results independent of JSON and avoid designing transport-specific endpoint
classes prematurely.

## Imported event metadata

Every imported trade, corporate action, dividend, interest payment, benefit, and private-market
event owns the same `EventMetadata`:

- `mTaxDate` is the broker-provided calendar date used for tax reporting. It is not derived from
  the source timestamp.
- `mSourceTimestamp` is an optional UTC instant with millisecond precision. Fractions finer than
  milliseconds are truncated.
- `mSources` contains every contributing `SourceReference`, each with the broker, source filename,
  source row, optional transaction ID, and stable input sequence. A parsed event starts with one
  source. An exact duplicate adds its source to the retained event instead of replacing provenance.

These types depend only on the C++ standard library. `SourceFilename::fromPath` removes both POSIX
and Windows directory components and rejects empty, `.` and `..` basenames, so event metadata
cannot retain an absolute host path. Source rows are one-based logical rows; for CSV input, the
header is row 1 and the first data row is row 2.

Different selected files may have the same sanitized filename. The application therefore derives
a run-local source ID from the file's request position: `source-0001`, `source-0002`, and so on.
The frontend keeps the selected-file list in that same order. When filenames collide, it shows a
label such as `taxreport.csv (file 2)` instead of exposing either file's directory. The source ID
identifies an upload only within that report run and does not change event ordering or duplicate
detection.

When duplicate names would still be confusing, the frontend may let the user add a run-local
display label such as `2022` or `pension account`. The frontend keeps that label locally with the
source ID and may show `taxreport.csv (2022, file 1)`. It does not send the label as broker data,
use it for processing, or replace the source ID with it.

When a broker omits a timestamp, `mSourceTimestamp` is empty. When it supplies an invalid timestamp
for an otherwise valid event, the parser keeps the event, leaves the timestamp empty, and reports a
warning. The tax date remains unchanged in both cases.

### Stable input sequence

The stable input sequence is scoped to the complete input request. `mSourceIndex` is the zero-based
position of the source file in the request, and `mEventIndex` is the zero-based source-order
position assigned by that source's parser. The current CSV parsers use the data-row position as the
event index. Parser completion order is never used, so files can be parsed concurrently without
changing event order.

The pair must uniquely identify each imported event in a request. It is the final ordering key and
does not represent broker time. Application JSON may expose only the derived source ID, never the
raw source index.

### Equality and duplicate identity

`EventMetadata` equality compares every metadata field, including the complete source collection.
This exact value equality is separate from duplicate detection. Source references are presented in
stable input-sequence, filename and row order; the earliest source is the event's primary ordering
source.

A transaction identity consists of the broker and transaction ID. The same ID from different
brokers therefore identifies different transactions. Matching identities mark duplicate
candidates even when their filename, row, or stable sequence differs. A candidate is an exact
duplicate only when its event kind, tax date, timestamp, instrument, and event-specific values also
match; otherwise it is a conflict. Events without transaction IDs are not automatically
deduplicated.

### Deterministic ordering

Events are normally ordered by tax date, timestamp presence, timestamp value, and stable input
sequence, in that order. On the same tax date, timestamped events precede events without
timestamps. Broker, filename, row, and transaction ID are not ordering fallbacks. The placement of
untimestamped events is a deterministic policy and does not claim that they occurred after every
timestamped event.

Tax processing has one event-kind priority: a supported split or reverse split is applied before
every purchase or sale for the same ISIN on its effective tax date. Trades on that date use the
adjusted position. Several actions for the same ISIN and date use reliable source timestamps. If
their order cannot be established, stable input sequence keeps the diagnostics deterministic but
must not be used to guess the result. The processor reports the ambiguity and leaves that ISIN
unprocessed.

## Statement-merger contract

`StatementMerger` is a transport-independent operation over parsed domain results. Each
`StatementMergeInput` carries the source file's request index separately from the collection's
arrival order. Request indices must be unique and must agree with the stable source indices on the
input's events. The merger may therefore receive parse results in task-completion order while still
producing the same result. A repeated or inconsistent source index is an input error and must be
reported; it is never repaired from vector position, filename or broker.

The operation is single-threaded. Its result owns one `BrokerStatement` presentation structure and
a separate chronological sequence of `StatementEventReference` values into that structure. Event
payloads and provenance are stored only in the presentation structure. A reference contains the
event kind, event index and an instrument index when that collection is instrument-based. Benefit
and private-market references have no instrument index.

Presentation order is independent of economic processing order:

1. collections appear as trades, dividends, interest, benefits and private-market events;
2. instrument collections use their broker-neutral identity, with deterministic identity and name
   ordering; and
3. events within an instrument use the normal event chronology.

The chronological reference sequence crosses every presentation collection and uses tax date,
timestamp presence, timestamp value and stable input sequence. It does not process one broker or
one presentation collection to completion first. The merger does not apply the processing-only
same-day corporate-action priority, calculate FIFO, or resolve economic corporate-action identity.

Parser and merger diagnostics share the result without changing parser diagnostics. A
`SourcedParseDiagnostic` contains the original `ParseDiagnostic` plus its broker and source request
index. Merger diagnostics have their own typed codes and retain every relevant source reference.
Parsing diagnostics precede merging diagnostics; within those stages, the deterministic ordering
from [`diagnostics.md`](diagnostics.md) applies. Final application diagnostic IDs are assigned only
after diagnostics from all pipeline stages are combined.

Input outcomes are defined as follows:

- no inputs produce empty presentation and chronological collections with no diagnostics;
- a row-level parse error preserves the parser's valid events and diagnostic;
- a file-level parse failure contributes its diagnostics and no events, while other inputs remain
  available;
- when every file fails, merged domain data is empty and every parser error remains visible; and
- inputs from different known brokers are accepted and combined through the same broker-neutral
  result.

The merger result has no report status. The later application service derives report and overall
statuses after applying each diagnostic's scope. Empty merged data is therefore not itself an error,
and parser errors cannot disappear merely because another input supplied valid data.

Broker values whose meaning is not verified remain source data, not calculated tax inputs. In
particular, a Trade Republic split row's decimal `shares` value does not establish the split ratio.
The application result asks the frontend for the action's new-shares-to-old-shares ratio. Only a
validated user confirmation turns that action into a processable split or reverse split.

After transaction deduplication, economic corporate-action identity is resolved separately across
all sources. One confirmed action changes all open lots of its ISIN across brokers exactly once.
Grouping by ISIN and calendar year is used only for
`corporate_action_sources_mismatch_possible`: always warn when action records for that ISIN/year
come from multiple source files or brokers, and show every date, type, ratio and source. The year
is never an action identity or a reason to combine two distinct actions. Ambiguous grouping returns
`corporate_action_identity_required`; a ratio prompt covers the combined action's complete source
set. The detailed identity, ordering and conflict rules are in `fifo.md`.

The processing request also retains a separate analysis view for 30-day loss eligibility, including
required previous-December and next-January events. It does not mutate the selected-year closing
FIFO position. Partially eligible losses become separate report rows, without duplicating their
inventory consumption. User confirmations of missing coverage and external activity are explicit
run-local inputs, as defined in `tax_rules.md`.

Report exclusions preserve source facts and replacement allocations needed by retained losses.
Unresolved dependencies on excluded data require a loss decision for the dependent sale; they
cannot be cleared by removing the source instrument's report rows. Relevant following-January
actions use the same identity, ratio and multiple-source warning rules in the analysis view.

Exact FIFO lot basis retains the complete product of the acquisition-date converted Money unit
price and imported Units. Proportional allocations remain exact rational values until their
specified output boundary. Controlled quantity adjustments retain their original source values,
apply through a run-local overlay and rebuild affected history. The backend tracks the cumulative
absolute EUR impact of inventory reconciliation per ISIN/run; details are in `calculations.md`
and `fifo.md`. This additional processor state does not change parser diagnostic version 1.

Foreign-currency processing follows [`calculations.md`](calculations.md). The application result
contains one structured request for every missing official `(currency, tax date)` rate. A
user-entered official rate is shared across brokers and instruments only for that exact pair. A
broker rate is an event-specific fallback, requires an explicit frontend warning, and remains
marked as non-official in the result.

The application result also contains the selected-year informational fee summary. It groups
preserved fees by broker and original currency. It provides a broker-rate EUR equivalent only
when every required fee rate is available from that broker. A fee never creates an official-rate
request. Missing fee rates leave the affected EUR totals unavailable and do not block FURS XML
output.

Final models and result statuses are split by form: capital gains (Doh-KDVP), dividends (Doh-Div),
bank-deposit interest (Doh-DHO), and other interest (Doh-Obr). Broker cash interest and ordinary bond
coupons remain separate subtypes inside the common other-interest report. A broker parser supplies
facts such as product category and payer identity; the tax processor applies date-specific rules.
Unknown classifications and missing income fields produce structured requests. Explicit
classification exclusions remain visible in `excludedItems`, separate from each report's failed ISINs.

A Doh-Obr error safely isolated to an ISIN produces
`other_interest_failure_resolution_required`. The user excludes all listed failed ISINs' Doh-Obr
income or fails that form. Each report owns its exclusion list and failure choice; capital and
other-interest choices never apply to each other. Preserve original evidence and unaffected income.
Non-isolatable errors and final XML failures retain their normal scope, as defined in `diagnostics.md`.

A standalone reference catalogue supplies verified Slovenian-company and imported company/ETF
metadata with source, revision and effective-date provenance. It can prefill payer and treaty
facts; payment evidence determines settled Slovenian withholding and actual foreign tax.
Treaty exemptions require a separate applicable legal basis. The income result retains treatment
decisions and filing-evidence requirements. Catalogue conflicts and missing facts use
`income_details_required`; changing a referenced catalogue revision invalidates affected decisions.

The deposit allowance considers all imported and user-supplied qualifying bank interest. A
below-threshold TR total retains its reminder about other banks. Slovenian retail government bonds
are outside the current TR/IBKR import scope; their special treatment is not part of this workflow.

The complete application diagnostic model, revision-bound decisions, deterministic ordering,
privacy boundary, and four independent form statuses are defined in [`diagnostics.md`](diagnostics.md).
Application JSON uses version 2 for this expanded result; existing parser JSON remains version 1.

## Parser diagnostics

Parser diagnostics have three separate responsibilities:

1. `include/taxbroker/diagnostics.hpp` defines the in-memory domain model. A `ParseResult` owns its
   diagnostics, so parsing separate CSV files concurrently does not require shared mutable state.
2. `include/taxbroker/api/diagnostics_json.hpp` and its implementation under `src/server/api/`
   convert that model into the versioned frontend contract. The parsing and tax-processing core
   does not depend on a JSON library.
3. The optional `taxbroker_tr_dump` developer tool writes a JSON artifact. Production parsing does
   not write diagnostics to disk; the future local API should return them directly in its response.

Application logs remain useful for developers and operational troubleshooting, but the frontend
must never parse log messages.

### JSON contract version 1

```json
{
  "schemaVersion": 1,
  "broker": "trade_republic",
  "status": "completed_with_errors",
  "summary": {
    "warningCount": 1,
    "errorCount": 1,
    "totalCount": 2
  },
  "diagnostics": [
    {
      "severity": "warning",
      "code": "unsupported_asset_class",
      "message": "Asset class 'CRYPTO' was preserved, but its tax treatment is not supported yet.",
      "source": {
        "file": "TransactionExport.csv",
        "row": 275
      },
      "transactionId": "example-transaction-id",
      "field": "asset_class"
    }
  ]
}
```

The `status` value is derived from diagnostics:

- `success`: no diagnostics;
- `completed_with_warnings`: one or more warnings and no errors;
- `completed_with_errors`: at least one error.

Warnings describe preserved data that needs attention later. Errors describe rejected rows or a
file-level parsing failure. Optional location fields are omitted when unavailable rather than sent
as `null`.

The strings used by `severity`, `code`, `broker`, and `status` are API values. Existing values must
not be renamed within a schema version. Add a new code for new behavior, and increment
`schemaVersion` for a breaking contract change. This versioning protects the current backend and
future frontend contract; it does not cover the legacy project.

For privacy, the response exposes only the uploaded filename, never an absolute host path. It also
does not duplicate raw financial field values. The CSV row number, transaction ID, field name, and
a human-readable message provide enough context for the local UI. The parser version 1 contract is
unchanged when its diagnostics are adapted into the separate application result defined in
[`diagnostics.md`](diagnostics.md).

### Local debug artifacts

Running `scripts/dump_tr_parse.sh` produces:

- `runtime/debug/tr_parsed_debug.txt` for the parsed C++ data;
- `runtime/diagnostics/tr_parse_diagnostics.json` for the frontend-shaped diagnostics.

The tool shows what the parser retains from a local export, including parsed financial data and
complete event metadata. Only the source filename is stored, never its path. The `runtime/`
directory is ignored by Git; its potentially private artifacts must remain local.

Parser logs may later be shared for support, so they follow a stricter boundary: source index, CSV
row, and diagnostic reason are allowed; paths, transaction IDs, and raw financial values are not.
