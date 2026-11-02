# Synthetic parser fixtures

All values here are made up. None comes from a real export.

## `traderepublic_bond_maturity_fixture.csv`

One USD bond on Trade Republic, from purchase to maturity, with the correction Trade Republic
made after a maturity was paid out in the wrong currency. The values are synthetic. The columns
that are filled or blank, the signs, the `category`/`type`/`asset_class` values, the decimal places
and the row order copy a real export (2026-11-02, #281).

| Row | `category` `type` | What it shows |
| --- | --- | --- |
| 2–3 | `TRADING` `BUY` | One purchase split into two rows a few milliseconds apart; `shares` is the nominal (932.5 + 67.5 = 1,000, a fraction), `price` is EUR per 1 nominal of the bond's currency (0.90, below 1 because of the exchange rate, not because the bond is under par), the fee is only on the first row. `amount` is higher than `shares` × `price` (848.58 against 839.25, 61.43 against 60.75): it also has the interest built up since the last coupon (*accrued interest*), so each row gets a `trade_amount_mismatch` warning. |
| 4–5 | `CASH` `INTEREST_PAYMENT` | Coupons: `shares`, `price` and `tax` blank; `amount` in EUR, `original_amount` 20.00 USD, `fx_rate` EUR per USD (0.90 and 0.92). The last coupon is on the maturity day. |
| 6 | `CORPORATE_ACTION` `FINAL_MATURITY` | Units out: `shares` −1,000; `price`, `amount` and `currency` blank. |
| 7 | `CASH` `FINAL_MATURITY` | First payout: `shares` **blank**, `amount` 1,000.00 EUR with no original amount (the wrong currency). |
| 8 | `CORPORATE_ACTION` `FINAL_MATURITY_CANCELLED` | Units back: `shares` +1,000. `date` stays the maturity day (2025-08-14), `datetime` is the correction day (2025-08-21). |
| 9 | `CORPORATE_ACTION` `FINAL_MATURITY` | Units out again, same `date`/`datetime` pattern as row 8. |
| 10 | `CASH` `FINAL_MATURITY` | Payout reversed: `amount` −1,000.00, on the correction day. |
| 11 | `CASH` `FINAL_MATURITY` | Corrected payout: `amount` 919.99 EUR, `original_amount` 999.99 USD (one cent below the nominal), `fx_rate` 0.92, on the correction day. |

Net: 1,000 units out once (rows 6 − 8 + 9), one payout of 999.99 USD (rows 7 − 10 + 11), which is not exactly the nominal.
Rows are sorted by `date`, then `datetime`, as in the real export, so row 9 comes before row 10
although its `datetime` is later. Every row has its own transaction ID.
