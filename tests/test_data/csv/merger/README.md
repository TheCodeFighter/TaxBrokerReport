# Synthetic merger fixtures

All rows, names, identifiers, dates and values are hand-authored test data. No real broker export
was used or copied. The CSV column layout matches the existing Trade Republic parser contract.

- `history.csv` contains all six event kinds, deliberately unsorted dates, an ignored row, equal
  and missing timestamps, a timestamp that disagrees with its tax year, and a trade without an ID.
- `conflict.csv` changes only the timestamp of `synthetic-tied` by one second.
- `partial.csv` contains one invalid-price row and one valid row with a conflicting instrument
  name and asset class.
- `invalid.csv` has an invalid header and produces a file-level diagnostic.
- `unknown_income.csv` contains a foreign dividend, cash interest and bond coupon with blank tax
  and broker rate, plus a healthy trade and a separate dividend with explicit zero tax.
- `confirmed_tax_income.csv` repeats only the three foreign-income rows with explicit zero tax.
- `broker_rate_income.csv` repeats only those rows with a broker rate of `0.90` and blank tax.

The income fixtures exercise the parser's current import policy. They do not supply independent
broker payment evidence or establish the direction of the broker rate.

Integration tests create disjoint, overlapping and header-only files from `history.csv` inside
an owned temporary directory. Their provenance differs intentionally from the whole-file input.
