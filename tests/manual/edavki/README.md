# Manual eDavki draft-import tests (Doh-KDVP)

Synthetic Trade Republic CSVs for checking that eDavki imports the Doh-KDVP file of
`taxbroker report` and calculates the expected values. No build or CTest uses them; the same cases
are covered automatically by the unit and integration tests. The results go in
[`docs/verification.md`](../../../docs/verification.md).

All trades are in EUR, without fees. Every listed sale of S1 to S4 is a gain; the loss of S5 is
blocked in full by a purchase within 30 days, so no CLI gate blocks any of them.
The ISINs (`XX…`) and names are made up. Purchase value is F3 × F4, sale value F7 × F9.

## How to run a check

1. Put your own taxpayer file in `runtime/taxpayer.json` (ignored, never committed). The tax
   number must be the one you log in to eDavki with; the other keys are optional
   (`taxbroker report --help`).
2. Write the XML of a scenario, always as a self-report (an original can be imported only from
   1 January to 28 February):

   ```sh
   scripts/report.sh --year 2025 --taxpayer runtime/taxpayer.json --self-report \
       --out runtime/edavki/s1 tests/manual/edavki/kdvp_2025_s1_two_purchases.csv
   ```

   The summary on stdout lists the listed units and the units past the holding period. With
   `--taxpayer tests/test_data/cli/taxpayer.json` the same command writes the synthetic version.
3. eDavki → *Uvoz dokumenta* → the XML file.
4. Note whether it imported (or the error text), and for each row what the form shows: the values,
   the gain, and any holding-period mark or warning.
5. **Never submit.** Delete the draft afterwards.
6. Record the result in `docs/verification.md` without tax number, name, address or screenshots.

## S1: one sale uses two purchases, an earlier-year sale and unused units

File `kdvp_2025_s1_two_purchases.csv`, tax year 2025, ISIN `XX0000000101`.

| Date | Type | Units | Price |
| --- | --- | --- | --- |
| 2024-01-10 | BUY | 10 | 10 |
| 2024-06-10 | SELL | 4 | 12 |
| 2025-01-10 | BUY | 10 | 20 |
| 2025-03-10 | SELL | 10 | 25 |

FIFO: the 2024 sale uses 4 units of the first purchase (earlier year, no row). The 2025 sale uses
the other 6 of the first purchase and 4 of the second; 6 units stay unused (no row).

| Row | F1 / F6 | F3 / F7 | F4 / F9 | F8 |
| --- | --- | --- | --- | --- |
| Purchase | 2024-01-10 | 6 | 10 | 6 |
| Purchase | 2025-01-10 | 4 | 20 | 10 |
| Sale | 2025-03-10 | 10 | 25 | 0 |

Purchase value 60 + 80 = 140, sale value 250, gain **110**.

Checks: `F3` is only the units this year's sale uses, and `F8` is the stock of the listed units,
not the whole holding (`tax_rules.md`, "Project rule").

## S2: a sale mixes units past the holding period with newer units

File `kdvp_2025_s2_old_and_new_units.csv`, tax year 2025, ISIN `XX0000000202`.

| Date | Type | Units | Price |
| --- | --- | --- | --- |
| 2005-04-01 | BUY | 2 | 10 |
| 2020-04-01 | BUY | 2 | 15 |
| 2025-06-02 | SELL | 3 | 20 |

FIFO takes both 2005 units (past the 15 years, left out) and 1 unit from 2020.

| Row | F1 / F6 | F3 / F7 | F4 / F9 | F8 |
| --- | --- | --- | --- | --- |
| Purchase | 2020-04-01 | 1 | 15 | 1 |
| Sale | 2025-06-02 | 1 | 20 | 0 |

Summary: 2 units past the holding period, sale 2025-06-02. Purchase value 15, sale value 20,
gain **5**.

Checks: `F7` (1) is lower than the sold quantity on the broker confirmation (3).

## S3: the 15-year anniversary and the day after, in one sale

File `kdvp_2025_s3_anniversary.csv`, tax year 2025, ISIN `XX0000000303`.

| Date | Type | Units | Price |
| --- | --- | --- | --- |
| 2010-03-10 | BUY | 1 | 10 |
| 2010-03-11 | BUY | 1 | 12 |
| 2025-03-11 | SELL | 2 | 20 |

The 2010-03-10 unit's 15 years end on 2025-03-10, so the sale is the day after (past, left out).
The 2010-03-11 unit's anniversary is the sale day (still inside, listed).

| Row | F1 / F6 | F3 / F7 | F4 / F9 | F8 |
| --- | --- | --- | --- | --- |
| Purchase | 2010-03-11 | 1 | 12 | 1 |
| Sale | 2025-03-11 | 1 | 20 | 0 |

Summary: 1 unit past the holding period, sale 2025-03-11. Purchase value 12, sale value 20,
gain **8**.

Checks: eDavki treats the unit sold on its anniversary as taxed.

### Probe P1 (hand-made, not app output)

eDavki cannot show what it does with a unit the app leaves out. P1 adds the left-out unit back:
in your S3 file, replace every `<Row>` inside `<Securities>` with these rows.

```xml
          <Row>
            <ID>0</ID>
            <Purchase>
              <F1>2010-03-10</F1>
              <F2>B</F2>
              <F3>1.0000</F3>
              <F4>10.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>1.0000</F8>
          </Row>
          <Row>
            <ID>1</ID>
            <Purchase>
              <F1>2010-03-11</F1>
              <F2>B</F2>
              <F3>1.0000</F3>
              <F4>12.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>2.0000</F8>
          </Row>
          <Row>
            <ID>2</ID>
            <Sale>
              <F6>2025-03-11</F6>
              <F7>2.0000</F7>
              <F9>20.0000</F9>
              <F10>true</F10>
            </Sale>
            <F8>0.0000</F8>
          </Row>
```

Purchase value 10 + 12 = 22, sale value 40, gain 18 if eDavki taxes both units. Look for: does
eDavki treat the 2010-03-10 unit as past the 15 years (tax free, a holding-period mark, or a gain
of only 8 counted) and the 2010-03-11 unit as taxed?

## S4: a purchase on 29 February

File `kdvp_2023_s4_leap_day.csv`, tax year **2023**, ISIN `XX0000000404`. A 29 February purchase
with the 15 years needs a sale in 2023 (purchase 2008) or 2027 (purchase 2012).

| Date | Type | Units | Price |
| --- | --- | --- | --- |
| 2008-02-29 | BUY | 2 | 10 |
| 2023-02-28 | SELL | 1 | 20 |
| 2023-03-01 | SELL | 1 | 20 |

2023 has no 29 February, so the 15 years end on 2023-02-28: the first sale is inside (listed),
the second is past (no row).

| Row | F1 / F6 | F3 / F7 | F4 / F9 | F8 |
| --- | --- | --- | --- | --- |
| Purchase | 2008-02-29 | 1 | 10 | 1 |
| Sale | 2023-02-28 | 1 | 20 | 0 |

Summary: 1 unit past the holding period, sale 2023-03-01. Purchase value 10, sale value 20,
gain **10**.

Checks: eDavki accepts the 29 February date and treats the 2023-02-28 sale as taxed.

### Probe P2 (hand-made, not app output)

In your S4 file, replace every `<Row>` inside `<Securities>` with these rows.

```xml
          <Row>
            <ID>0</ID>
            <Purchase>
              <F1>2008-02-29</F1>
              <F2>B</F2>
              <F3>2.0000</F3>
              <F4>10.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>2.0000</F8>
          </Row>
          <Row>
            <ID>1</ID>
            <Sale>
              <F6>2023-02-28</F6>
              <F7>1.0000</F7>
              <F9>20.0000</F9>
              <F10>true</F10>
            </Sale>
            <F8>1.0000</F8>
          </Row>
          <Row>
            <ID>2</ID>
            <Sale>
              <F6>2023-03-01</F6>
              <F7>1.0000</F7>
              <F9>20.0000</F9>
              <F10>true</F10>
            </Sale>
            <F8>0.0000</F8>
          </Row>
```

Purchase value 20, sale value 40, gain 20 if eDavki taxes both sales. Look for: does eDavki treat
the 2023-03-01 sale as past the 15 years and the 2023-02-28 sale as taxed?

Both probe row sets, put into the synthetic S3 and S4 files, pass the `Doh_KDVP_9.xsd` check.

## S5: a loss blocked by the 30-day rule (`F10` false)

File `kdvp_2024_s5_loss_blocked.csv`, tax year **2024**, ISIN `XX0000000505`.

| Date | Type | Units | Price |
| --- | --- | --- | --- |
| 2023-09-01 | BUY | 100 | 10 |
| 2024-12-20 | SELL | 4 | 8 |
| 2025-01-05 | BUY | 4 | 8 |

The sale is a loss of 4 × (8 − 10) = −8. The 2025-01-05 purchase, 16 days after it, blocks the
loss of all 4 units (*pravilo navidezne odsvojitve*), so the sale row has `F10` false and the app
asks nothing. The 2025 purchase is not in the 2024 ledger and gets no row.

| Row | F1 / F6 | F3 / F7 | F4 / F9 | F8 | F10 |
| --- | --- | --- | --- | --- | --- |
| Purchase | 2023-09-01 | 4 | 10 | 4 | |
| Sale | 2024-12-20 | 4 | 8 | 0 | false |

Purchase value 40, sale value 32, loss **−8**, not deductible.

Checks: eDavki imports `F10` false, and, if it shows anything, does not let the loss reduce the
gains.
