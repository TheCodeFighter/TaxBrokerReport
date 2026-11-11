# Manual eDavki draft-import tests (Doh-KDVP and income forms)

Synthetic Trade Republic CSVs for checking that eDavki imports the Doh-KDVP file of
`taxbroker report` and calculates the expected values (S1 to S8), and the checks of the income
forms (E1 to E5 and E7, at the end; E6 imports S5 to S8). No build or CTest uses them; the same cases are covered automatically
by the unit and integration tests. The results go in
[`docs/verification.md`](../../../docs/verification.md).

All trades are in EUR, without fees. Every listed sale of S1 to S4 is a gain; the loss of S5 is
blocked in full by a purchase within 30 days, so no CLI gate blocks any of them. S6 and S8 contain
a split: the command asks for its ratio (answer `2 : 1`, an empty line for the broker's date and
`yes`). S7 has a sale whose loss is blocked in part and needs no answer.
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

## S6: a split before the sale

File `kdvp_2025_s6_split_lot.csv`, tax year **2025**, ISIN `XX0000000606`. Answer the ratio
question with `2 : 1`, an empty line (the broker's date, 2024-06-03) and `yes`.

| Date | Type | Units | Price |
| --- | --- | --- | --- |
| 2024-03-01 | BUY | 10 | 100 |
| 2024-06-03 | SPLIT | | answered 2 : 1 |
| 2025-09-02 | SELL | 20 | 60 |

The 10 units are 20 after the split, EUR 50 each. The purchase row lists the 20 units after the
split with the original date.

| Row | F1 / F6 | F3 / F7 | F4 / F9 | F8 |
| --- | --- | --- | --- | --- |
| Purchase | 2024-03-01 | 20 | 50 | 20 |
| Sale | 2025-09-02 | 20 | 60 | 0 |

Purchase value 1,000, sale value 1,200, gain **200**.

Checks: eDavki imports `F3` 20 and `F4` 50 for a purchase of 10 units at EUR 100 (the units and
price of the broker confirmation are not the row's).

## S7: a loss blocked in part (two sale rows)

File `kdvp_2025_s7_loss_in_two_rows.csv`, tax year **2025**, ISIN `XX0000000707`.

| Date | Type | Units | Price |
| --- | --- | --- | --- |
| 2024-06-03 | BUY | 10 | 10 |
| 2024-07-03 | BUY | 10 | 20 |
| 2025-03-11 | SELL | 15 | 18 |
| 2025-03-20 | BUY | 8 | 18 |

The sale uses 10 units at EUR 10 (+80) and 5 at EUR 20 (−10). The purchase of 8 units 9 days later
blocks the loss of the 5 units, so the sale has two rows: the 10 profitable units with `F10` true,
then the 5 loss units with `F10` false. The 2025-03-20 purchase has no row.

| Row | F1 / F6 | F3 / F7 | F4 / F9 | F8 | F10 |
| --- | --- | --- | --- | --- | --- |
| Purchase | 2024-06-03 | 10 | 10 | 10 | |
| Purchase | 2024-07-03 | 5 | 20 | 15 | |
| Sale | 2025-03-11 | 10 | 18 | 5 | true |
| Sale | 2025-03-11 | 5 | 18 | 0 | false |

Purchase value 100 + 100 = 200, sale value 270, gain 70: **+80** on the first row and **−10**
(not deductible) on the second.

Checks: eDavki imports two sale rows with the same date and price, and does not let the −10 reduce
the gains.

## S8: a purchase sold before and after a split

File `kdvp_2024_s8_split_between_sales.csv`, tax year **2024**, ISIN `XX0000000808`. Answer the
ratio question with `2 : 1`, an empty line (the broker's date, 2024-06-03) and `yes`.

| Date | Type | Units | Price |
| --- | --- | --- | --- |
| 2023-05-02 | BUY | 10 | 100 |
| 2024-02-01 | SELL | 4 | 120 |
| 2024-06-03 | SPLIT | | answered 2 : 1 |
| 2024-09-02 | SELL | 12 | 55 |

The 4 units sold in February are old units; the 6 left become 12 after the split, EUR 50 each. The
purchase has one row for each state, both at its date.

| Row | F1 / F6 | F3 / F7 | F4 / F9 | F8 |
| --- | --- | --- | --- | --- |
| Purchase | 2023-05-02 | 4 | 100 | 4 |
| Purchase | 2023-05-02 | 12 | 50 | 16 |
| Sale | 2024-02-01 | 4 | 120 | 12 |
| Sale | 2024-09-02 | 12 | 55 | 0 |

Purchase value 400 + 600 = 1,000, sale value 480 + 660 = 1,140, gain **140** (80 + 60).

Checks: eDavki imports two purchase rows with the same date, and `F8` (old and new units added) is
accepted.

## Income forms (E1 to E5)

Draft imports of the Doh-Div, Doh-DHO and Doh-Obr files. The files of `taxbroker report` are
originals: eDavki accepts the original workflow `O` of these forms as a draft at any time of the
year. A tax number that passes eDavki's check goes in the taxpayer file, as above. A return for a
year may already exist: change `<Period>` to an earlier year in the file, as the earlier
imports did. **Never submit**, delete the draft afterwards.

### E1: the four files of `scripts/report.sh --synthetic`

```sh
scripts/report.sh --synthetic
```

Tax year 2024, Doh-KDVP plus the income fixture (`tests/test_data/cli/README.md`). Answer yes to
the one Doh-DHO question. The command writes `Doh-KDVP_2024.xml`, `Doh-Div_2024.xml`,
`Doh-DHO_2024.xml` and `Doh-Obr_2024.xml` to `runtime/report/synthetic_2024/`. Import each one.

| Form | Expected rows |
| --- | --- |
| Doh-Div | `Value` 10.00 (USD 12.50 / 1.25), 15.38 (USD 20.00 / 1.30; `ForeignTax` 2.40), 5.00 (fund, no payer facts), 4.00 (Brazilian payer); no row for the Slovenian payer |
| Doh-DHO | one row, `InterestEarned` 1.00 |
| Doh-Obr | two coupons, `Type` 2, `Value` 16.67 (USD 20.00 / 1.20) and 14.29 (USD 20.00 / 1.40) |

Checks: every file imports; the only critical content errors are the empty payer facts and the
foreign-tax proof of payment.

### E2: a dividend on a Saturday (rate of the day before)

Files `div_2025_e2_weekend_dividend.csv` and `ecb_2025_e2.csv` (USD 1.25 on Friday 2025-03-14,
no rate on Saturday 2025-03-15).

```sh
scripts/report.sh --year 2025 --taxpayer runtime/taxpayer.json --no-input \
    --out runtime/edavki/e2 \
    --trade-republic tests/manual/edavki/div_2025_e2_weekend_dividend.csv \
    --ecb-rates tests/manual/edavki/ecb_2025_e2.csv
```

Doh-Div row: USD 12.50 / 1.25 = `Value` **10.00**, `ForeignTax` 0.00, date 2025-03-15. The summary
has the warning `official_rate_from_nearby_date` and lists `USD 2025-03-15 … (rate of 2025-03-14)`.
The share has an imaginary ISIN (`XX0000000010`), so the row has no payer facts and a second
warning says to complete them in eDavki.

Checks: eDavki imports a dividend dated on a Saturday; the empty payer facts are the only critical
content error. (Whether FURS accepts the rate of the day
before is a separate question to FURS, not tested by a draft.)

### E3: Doh-Obr with `ReliefStatement` `DA`

File `tests/test_data/income/Doh-Obr_2023_treaty.golden.xml`, with `<Period>` changed to 2022.

| Row | Expected |
| --- | --- |
| Coupon 2023-03-14 | `Value` 25.00, `ForeignTax` 3.00, `ReliefStatement` `2. odstavek 11. člena` |
| Coupon 2023-06-16 | `Value` 40.00, `ForeignTax` 6.00, `ReliefStatement` `DA` |
| Cash interest | `Type` 4, `Value` 3.45 |

Checks: eDavki accepts `DA` (the earlier import had `true`) and, for the 6.00 row, shows the claim
the user completes with the paragraph and article. The foreign-tax proof of payment is a critical
content error for both taxed rows, as before.

### E4: a coupon of a Slovenian issuer

Files `obr_2025_e4_slovenian_issuer.csv` and `e4_reference/payers.csv` (the file keeps the name
`payers.csv`, because a supplied file replaces the built-in file of the same name). The issuer's
tax number `12345679` is made up and passes the check digit.

```sh
scripts/report.sh --year 2025 --taxpayer runtime/taxpayer.json --no-input \
    --out runtime/edavki/e4 \
    --trade-republic tests/manual/edavki/obr_2025_e4_slovenian_issuer.csv \
    --reference-files tests/manual/edavki/e4_reference/payers.csv
```

Doh-Obr row: EUR 30.00 coupon of 2025-06-16, `Type` 2, `Value` **30.00**, `IdentificationNumber`
`12345679`, `Country` `SI`, `Country2` `SI`, no `ForeignTax` (the form leaves it out for a Slovenian
payer) and no `ReliefStatement`. No warning (no tax was paid).

Checks: eDavki accepts an eight-digit `IdentificationNumber` and a Slovenian payer country on a
coupon row, and shows no foreign tax.

### E5: `Doh-Obr_2023.golden.xml`

File `tests/test_data/income/Doh-Obr_2023.golden.xml`, with `<Period>` changed to 2022.

| Row | Expected |
| --- | --- |
| Coupon | `Type` 2, `Value` 25.00, no payer facts |
| Cash interest | `Type` 4, `Value` 3.45, the broker's payer facts (`DE`) |

Checks: the file imports; the only critical content errors are the empty payer facts of the coupon.

### E6: S5 to S8

The Doh-KDVP files S5 to S8 above, each as a self-report (see "How to run a check").

### E7: a taxed coupon with a payer and a treaty text, through the CLI

The coupon rows of E1 and E5 have no payer, and E3 is a golden file, not CLI output. This check
runs the CLI on a coupon whose issuer is in a supplied reference file, with a treaty text.
Files `obr_2025_e7_treaty_coupon.csv`, `e7_reference/payers.csv` (issuer `XX0000000036`, `US`,
`TAX-U`, made up) and `e7_reference/relief_statements.csv` (a `bond_coupon` text for `US`).

```sh
scripts/report.sh --year 2025 --taxpayer runtime/taxpayer.json --no-input \
    --out runtime/edavki/e7 \
    --trade-republic tests/manual/edavki/obr_2025_e7_treaty_coupon.csv \
    --reference-files tests/manual/edavki/e7_reference/payers.csv \
                      tests/manual/edavki/e7_reference/relief_statements.csv
```

Doh-Obr row: EUR 25.00 coupon of 2025-06-16 with EUR 3.00 tax, `Type` 2, `Value` **25.00**,
`IdentificationNumber` `TAX-U`, `Name` `Synthetic Issuer US Inc.`, `Country` `US`, `ForeignTax`
**3.00**, `Country2` `US`, `ReliefStatement` `2. odstavek 11. člena`. The summary has one warning,
`foreign_tax_evidence_required`.

Checks: eDavki shows the payer facts and the treaty claim; the foreign-tax proof of payment is the
only critical content error.
