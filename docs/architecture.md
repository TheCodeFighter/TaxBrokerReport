# Architecture

TaxBrokerReport is intended to run entirely on the user's machine. The C++20 backend owns broker
parsing, validation, tax processing, and XML generation. A browser frontend will call the local
backend once an HTTP framework and frontend stack are selected.

The repository does not currently define a frontend framework or an HTTP transport. Code should
therefore keep domain results independent of JSON and avoid designing transport-specific endpoint
classes prematurely.

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
`schemaVersion` for a breaking contract change.

For privacy, the response exposes only the uploaded filename, never an absolute host path. It also
does not duplicate raw financial field values. The CSV row, transaction ID, field name, and a
human-readable message provide enough context for the local UI.

### Local debug artifacts

Running `scripts/dump_tr_parse.sh` produces:

- `runtime/debug/tr_parsed_debug.txt` for the parsed C++ data;
- `runtime/diagnostics/tr_parse_diagnostics.json` for the frontend-shaped diagnostics.

The whole `runtime/` directory is ignored by Git. These files are inspection aids, not an API or a
source of application state.
