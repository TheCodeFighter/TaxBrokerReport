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
- `mSource` is a `SourceReference` containing the broker, source filename, source row, optional
  transaction ID, and stable input sequence.

These types depend only on the C++ standard library. `SourceFilename::fromPath` removes both POSIX
and Windows directory components and rejects empty, `.` and `..` basenames, so event metadata
cannot retain an absolute host path. Source rows are one-based logical rows; for CSV input, the
header is row 1 and the first data row is row 2.

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
does not represent broker time.

### Equality and duplicate identity

`EventMetadata` equality compares every metadata field, including filename, row, and stable input
sequence. This exact value equality is separate from duplicate detection.

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

Broker values whose meaning is not verified remain source data, not calculated tax inputs. In
particular, a Trade Republic split row's decimal `shares` value does not establish the split ratio.
The application result asks the frontend for the action's new-shares-to-old-shares ratio. Only a
validated user confirmation turns that action into a processable split or reverse split.

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
does not duplicate raw financial field values. The CSV row, transaction ID, field name, and a
human-readable message provide enough context for the local UI.

### Local debug artifacts

Running `scripts/dump_tr_parse.sh` produces:

- `runtime/debug/tr_parsed_debug.txt` for the parsed C++ data;
- `runtime/diagnostics/tr_parse_diagnostics.json` for the frontend-shaped diagnostics.

The tool shows what the parser retains from a local export, including parsed financial data and
complete event metadata. Only the source filename is stored, never its path. The `runtime/`
directory is ignored by Git; its potentially private artifacts must remain local.

Parser logs may later be shared for support, so they follow a stricter boundary: source index, CSV
row, and diagnostic reason are allowed; paths, transaction IDs, and raw financial values are not.
