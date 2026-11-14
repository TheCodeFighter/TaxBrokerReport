# Synthetic full-pipeline fixture

Two Trade Republic exports that overlap in 2024, used by `FullPipelineIntegrationTest`. Every
value is made up. The rate file is `tests/test_data/cli/report_ecb_rates.csv` (USD 1.20 on
2024-02-14, 1.25 on 2024-03-14, 1.40 on 2024-08-14).

`export_2023_2024.csv` (2023-03-01 to 2024-03-28):

| id | date | row |
| --- | --- | --- |
| `p-buy-1` | 2023-03-01 | buy 10 `XX0000000101` @ 10, fee 1.00 |
| `p-buy-2` | 2023-09-01 | buy 10 `XX0000000101` @ 20 |
| `p-bond-buy` | 2024-01-10 | buy 1000 bond `XS0000000002` @ 0.9, fee 1.00 |
| `p-int-1` | 2024-01-31 | bank interest 0.33 |
| `p-sell-1` | 2024-02-01 | sell 15 `XX0000000101` @ 30, fee 1.00 |
| `p-coupon-1` | 2024-02-14 | bond coupon USD 20.00 |
| `p-int-2` | 2024-02-29 | bank interest 0.33 |
| `p-div-apple` | 2024-03-14 | dividend Apple USD 12.50 |
| `p-int-3` | 2024-03-28 | bank interest 0.34 |

`export_2024_2025.csv` (2024-02-01 to 2025-02-03) holds the rows from `p-sell-1` to `p-int-3`
again, byte for byte (the overlap), then:

| id | date | row |
| --- | --- | --- |
| `p-div-krka` | 2024-04-02 | dividend Krka EUR 3.00 (Slovenian payer, left out) |
| `p-saveback` | 2024-05-02 | saveback benefit EUR 2.50 |
| `p-coupon-2` | 2024-08-14 | bond coupon USD 20.00 |
| `p-sell-2` | 2025-02-03 | sell 5 `XX0000000101` @ 40 |

Expected 2024 (both files; the deposit question answered `above_limit`):

- Doh-KDVP: purchase 2023-03-01 `F3` 10 `F4` 10 (`F8` 10), purchase 2023-09-01 `F3` 5 `F4` 20
  (`F8` 15), sale 2024-02-01 `F6` 15 `F9` 30 (`F8` 0). Basis EUR 200 against EUR 450: no loss.
- Doh-Div: one row, Apple `Value` 10.00 (12.50 / 1.25). Krka is a `slovenian_tax_settled`
  excluded item, and the bond is a `debt_security` excluded item (not listed in Doh-KDVP).
- Doh-DHO: `InterestEarned` 1.00 (0.33 + 0.33 + 0.34; the overlap counts once).
- Doh-Obr: `Value` 16.67 (20.00 / 1.20) and 14.29 (20.00 / 1.40), each coupon once. The payer
  of `XS0000000002` is not in the reference files, so the payer facts are empty with a warning.
- Fees EUR 2.00 (bond purchase and `p-sell-1`; the 2023 fee is another year), benefits EUR 2.50.
- Two warnings (Doh-DHO holds only the imported interest after `above_limit`; the bond payer is
  not in the reference files). Overall `completed_with_warnings`, also before the answer.

Expected 2025: Doh-KDVP with purchase 2023-09-01 `F3` 5 `F4` 20 (`F8` 5) and sale 2025-02-03
`F6` 5 `F9` 40 (`F8` 0); Doh-Div, Doh-DHO and Doh-Obr have no data; overall `success`.

The `Doh-*_2024.golden.xml` and `Doh-KDVP_2025.golden.xml` files are the XML of those runs for the
synthetic taxpayer of the tests.
