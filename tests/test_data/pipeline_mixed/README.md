# Mixed-broker pipeline fixtures

Synthetic files of a Trade Republic export and Interactive Brokers Flex Query exports, used by the
mixed-broker run tests in `tests/integration/server_mixed_run_test.cpp`. All amounts are in EUR, so
no rate file is needed. The ISINs use the prefix `XX`, which is not in the built-in payer file.
The taxpayer is the synthetic one of the server tests.

## `trade_republic_2021_2024.csv`

| ID | Date | Row |
| --- | --- | --- |
| `m-buy-1` | 2021-02-10 | buy 1.5 `XX0000000201` at EUR 10 (EUR -15) |
| `m-buy-4` | 2023-09-01 | buy 100 `XX0000000203` at EUR 10 (EUR -1000) |
| `m-buy-2` | 2023-09-10 | buy 0.75 `XX0000000201` at EUR 11 (EUR -8.25) |
| `m-buy-3` | 2024-03-01 | buy 10 `XX0000000202` at EUR 10 (EUR -100) |
| `m-split-1` | 2024-11-10 | split of `XX0000000202`, `shares` 10 |
| `m-sell-2` | 2024-12-02 | sell 1 `XX0000000202` at EUR 6 (EUR 6) |
| `m-sell-3` | 2024-12-20 | sell 4 `XX0000000203` at EUR 8 (EUR 32) |

## `ibkr_2023_2024.xml` (account `U0000000`, entity `IB-IE`)

| ID | Date | Row |
| --- | --- | --- |
| `ib-sell-1` | 2023-04-05 | sell 0.4 `XX0000000201` at EUR 12 |
| `ib-sell-2` | 2024-06-03 | sell 1.6 `XX0000000201` at EUR 14 |
| `ib-split-1` | 2024-11-14 | split of `XX0000000202`, action ID `S1`, quantity 10 |
| `ib-split-2` | 2024-11-20 | split of `XX0000000202`, action ID `S2`, quantity 20 |

## `ibkr_2025.xml`

| ID | Date | Row |
| --- | --- | --- |
| `ib-buy-5` | 2025-01-06 | buy 4 `XX0000000203` at EUR 8 (EUR -32) |

## Where the values come from

- `XX0000000201`: `docs/fifo.md`, "Cross-broker example". The purchases are at Trade Republic and
  the sales at Interactive Brokers.
- `XX0000000202`: `docs/fifo.md`, "Identity question". Three split rows of two brokers within 10
  days.
- `XX0000000203`: `docs/tax_rules.md`, the 30-day rule. The loss sale of 2024-12-20 has a
  replacement purchase on 2025-01-06 only in the third file.

## Expected result for 2024

Upload of the Trade Republic file and `ibkr_2023_2024.xml`:

- Two requests: `corporate_action_identity_required` for `XX0000000202` (the three split rows in
  date order) and `loss_eligibility_confirmation_required` for `XX0000000203` (the sale of 4 units
  on 2024-12-20, 4 open units). Doh-KDVP needs input; Doh-Div, Doh-DHO and Doh-Obr have no data.

Answers, one call each:

1. Grouping `[1, 1, 2]` and the loss answer `no_other_activity`: one ratio question for the action
   of 2024-11-10 with 10 units before it and the rows of both brokers.
2. Ratio 2 : 1 on 2024-11-10, 20 units after it: the next ratio question is for 2024-11-20 with 20
   units before it and the one Interactive Brokers row.
3. Ratio 2 : 1 on 2024-11-20, 40 units after it: no request is left and Doh-KDVP is generated.

`Doh-KDVP_2024.golden.xml` is the Doh-KDVP after the three answers (checked by hand):

- `XX0000000201`: purchase 2021-02-10 `F3` 1.1 `F4` 10; purchase 2023-09-10 `F3` 0.5 `F4` 11;
  sale 2024-06-03 `F7` 1.6 `F9` 14, `F10` true. Gain 22.40 - 16.50 = EUR 5.90. No row for the sale
  of 2023.
- `XX0000000202`: purchase 2024-03-01 `F3` 1 `F4` 2.5 (EUR 100 for 40 units after the two splits);
  sale 2024-12-02 `F7` 1 `F9` 6, `F10` true. Gain EUR 3.50. No row for the splits.
- `XX0000000203`: purchase 2023-09-01 `F3` 4 `F4` 10; sale 2024-12-20 `F7` 4 `F9` 8, `F10` true
  (answer `no_other_activity`). Loss EUR -8.

A new upload with `ibkr_2025.xml` added (the answers are kept): no request. The purchase of
2025-01-06 is 17 days after the sale and replaces all 4 units, so the sale of `XX0000000203` has
`F10` false and the 2025 purchase is no row. The loss answer is listed as recorded and not used;
the identity answer and both ratio answers are used.
