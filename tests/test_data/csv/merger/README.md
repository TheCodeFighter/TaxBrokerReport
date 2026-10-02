# Synthetic merger fixtures

All rows, names, identifiers, dates and values are hand-authored test data. No real broker export
was used or copied. The CSV column layout matches the existing Trade Republic parser contract.

- `history.csv` contains all six event kinds, deliberately unsorted dates, an ignored row, equal
  and missing timestamps, a timestamp that disagrees with its tax year, and a trade without an ID.
- `conflict.csv` changes only the timestamp of `synthetic-tied` by one second.
- `partial.csv` contains one invalid-price row and one valid row with a conflicting instrument
  name and asset class.
- `invalid.csv` has an invalid header and produces a file-level diagnostic.

Integration tests create disjoint, overlapping and header-only files from `history.csv` inside
an owned temporary directory. Their provenance differs intentionally from the whole-file input.
