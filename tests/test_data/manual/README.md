# Manual test files (Trade Republic, 2022 to 2025)

Four synthetic Trade Republic exports, one for each year, for trying the application by hand in the
browser and for checking the XML in eDavki. Every name, amount and date is made up. No test reads
these files: they are for people. Each file has the rows of its own year, so together they are
one history, the way consecutive exports from the broker are.

Rules for these files:

- **Load all four files and pick the year.** A year needs the purchases of the earlier years (a
  sale without its purchase is an incomplete history, which is its own scenario below). To see one
  scenario alone, load only the files it needs.
- **Every supported or handled case has a row here.** A new feature adds its rows to the file of a
  year where it fits (see the skill `taxbrokerreport-development`, "Manual test files"), and a line
  to the tables below.
- **A case that would block every form is not here.** A row type the parser does not know blocks
  all forms of every year by design. Try that with a copy of a file.
- Foreign amounts use the real built-in ECB rates of the day, so the broker rate (`fx_rate`) is
  about the same as the official one. Use the taxpayer file `tests/test_data/cli/taxpayer.json`
  (all values made up, tax number `12345679`) or type any made-up details in the browser.

Command line, with the answers of 2023 (the order is the order of the questions):

```sh
printf 'capital\n2 : 1\n\nyes\n1\ndividends\nordinary\nyes\nexclude\nexclude\n' | scripts/report.sh --year 2023 \
  --taxpayer tests/test_data/cli/taxpayer.json --trade-republic \
  tests/test_data/manual/trade_republic_2022.csv tests/test_data/manual/trade_republic_2023.csv \
  tests/test_data/manual/trade_republic_2024.csv tests/test_data/manual/trade_republic_2025.csv
```

Every year asks again the questions that its history needs (the app keeps no answers between
runs), so 2024 and 2025 also ask the classification of the unknown-class share and the split ratio.

## Instruments

| Name | ISIN | Rows | What it shows |
| --- | --- | --- | --- |
| Synthetic Basic Share | `XX0000000101` | buys 2022, sales 2022 and 2024 | FIFO over two lots, fees; the 2024 sale uses a lot that an earlier sale used in part |
| Synthetic Index Fund | `XX0000000102` | savings plan 2022, sale and distribution 2023 | savings-plan buys (`SAVINGS_PLAN_EXECUTED`), a sale over two lots, a distribution whose payer is not in the reference files |
| Synthetic Loss Share | `XX0000000103` | 2022 | loss with no purchase near it: the 30-day loss question |
| Synthetic Partly Blocked Share | `XX0000000104` | 2022, sale 2025 | loss with a purchase inside the 30 days that blocks 4 of 10 units: the question for the other 6 |
| Synthetic Blocked Share | `XX0000000105` | 2022 | loss fully blocked by a purchase in the files: no question, `F10` false |
| Synthetic December Share | `XX0000000106` | sale 2022-12-20, purchase 2023-01-05 | loss whose window reaches into January: with the 2023 file the purchase blocks it without a question; with the 2022 file alone the app asks |
| Synthetic Split Share | `XX0000000107` | buy 2022, split 2023, sales 2023 and 2025 | split without a ratio: the ratio question (answer 2 : 1) |
| Synthetic Reverse Split Share | `XX0000000108` | buy 2022, split 2024, sale 2024 | reverse split (answer 1 : 10) |
| Synthetic Quantity Share | `XX0000000109` | 2024 | 0.99999999 bought, 1 sold, no proof of rounding: the quantity question |
| Synthetic Rounding Share | `XX0000000110` | 2023 | three purchases of 0.333333334 units: the discarded digits prove rounding, no question, a warning |
| Synthetic Missing History Share | `XX0000000111` | 2025 | 1 bought, 2 sold: incomplete history, the exclusion question |
| Synthetic Dollar Share | `XX0000000112` | USD buys 2022 (one on a Saturday), sales 2022 and 2024 | trades in USD converted with the ECB rate of the day; the Saturday takes the rate of the Friday with a note; the 2024 sale is a loss with the loss question |
| Synthetic Token | `XX0000000114` | crypto 2023 | crypto: left out of Doh-KDVP by rule, listed in the excluded items |
| Synthetic Unknown Class Share | `XX0000000118` | blank asset class 2023 | classification question (answer `capital`), and its dividend asks the income type (`dividends`) |
| Synthetic Index Certificate | `XX0000000119` | derivative 2024 | derivative: not supported, the exclusion question |
| Synthetic Ruble Share | `XX0000000121` | RUB 2023 (buy, sale and a dividend of 25.00 RUB on 2023-07-03) | the ECB has no ruble rate in 2023: `missing_exchange_rate`, the exclusion question of Doh-KDVP and a second one of Doh-Div (leave out: Doh-Div keeps its other rows and lists the ruble share; make no XML: no Doh-Div file; cancel: Doh-Div failed) |
| Synthetic Two Row Share | `XX0000000122` | buy 2022, one sale in two rows 2023 | two sale rows on one day |
| Synthetic Accumulating ETF | `XX0000000123` | savings plan and sale 2025 | sale over three lots |
| Synthetic Spare Change Share, Synthetic Saveback Share | `XX0000000127`, `XX0000000128` | 2025 | `BENEFITS_SPARE_CHANGE_EXECUTION` and `BENEFITS_SAVEBACK_EXECUTION` buys |
| Synthetic Private Fund | `XX0000000130` | 2024 | private-market buy and sell: skipped, only a cash amount |
| Synthetic Bond A 2025 | `XS0000000201` | USD bond, buy 2022, coupons 2022 to 2025, sale 2023, maturity 2025 | bond left out of Doh-KDVP, coupons in Doh-Obr, the bond-type question on a sale or maturity (`ordinary`), the maturity rows |
| Synthetic Bond B 2030 | `XS0000000202` | 2024 | a bond sold in the year; answer `discount` to see Doh-Obr blocked for it and the exclusion question |
| Synthetic Slovenian Bond | `SI0031102153` | 2024, 2025 | a bond of a Slovenian issuer (the ISIN is borrowed from the reference files): its coupon is a normal Doh-Obr row; the 2024 coupon has tax in the export and gets the `slovenian_coupon_tax` warning |

## Income and other rows

| Rows | Years | What it shows |
| --- | --- | --- |
| Apple (US), Johnson & Johnson (US), Novartis (CH), TotalEnergies (FR) dividends | most years | foreign dividends in USD, CHF and EUR with foreign tax; the treaty text of the country is claimed; no tax (J&J) is a blank tax |
| Krka (SI) dividend | 2022 to 2025 | a Slovenian payer deducts the tax itself: an excluded item, not in Doh-Div |
| Banco Bradesco (BR) dividend in BRL, with tax | 2022 | no treaty text for the country: the treaty question (`other` and a text, or `no`) |
| NVIDIA (US) dividend with tax | 2022, 2024 | two reference rows that differ: the payer question (pick a row or skip) |
| British American Tobacco (GB) dividend in GBP | 2023, 2025 | two reference rows that differ: the payer question |
| Distribution of a fund and a dividend of a yen share | 2022, 2023, 2024, 2025 | payer not in the reference files: the row is made, with a warning |
| A dividend without a name | 2025 | the row is made, the name is left empty |
| Interest on cash, monthly | 2022, 2023 (up to 2023-12-05) | Trade Republic interest before 2023-12-06 is Doh-Obr |
| One negative interest payment (-1.25, debit interest) | 2022 | not a row of Doh-Obr and not netted against the other payments; a warning `interest_payment_not_positive` |
| Interest on cash from 2023-12-06 (6.00 on the 6th, 6.50 on the 29th) | 2023 | from 2023-12-06 it is deposit interest (Doh-DHO); the total is EUR 12.50, so the app asks whether other banks bring it above EUR 1,000 |
| Interest on cash, monthly, with one payment of 0.00 and one of -2.40 | 2024 | total EUR 427.80, below EUR 1,000: the same question; the 0.00 and the -2.40 payment are not rows (not netted) and each gets a warning |
| Interest on cash, 12 payments of 95.00 | 2025 | EUR 1,140, above EUR 1,000: Doh-DHO is made without a question |
| Saveback, stock perk, referral, bonus | most years | benefits: information only, shown in the summary |
| Card payment, deposit, transfer, card fee | every year | ignored rows |
| `TAX_REFUND`, `WARRANT_EXERCISE`, `FIXED_INCOME` | 2022, 2023, 2025 | recognised but not supported: an error that is about no form, nothing else blocks |
| The last interest row of 2022 repeated at the top of 2023 | 2022, 2023 | an exact duplicate across two files is merged: counted once |
| Fees on trades (EUR and USD) | 2022, 2023, 2024 | fee totals in the summary, never in a form |

## Questions and answers

The answers below make every year succeed. The order is the order the command line asks them.

| Year | Questions in order | Answers | Result |
| --- | --- | --- | --- |
| 2022 | 30-day loss (Partly Blocked), 30-day loss (Loss Share), treaty text for BR, payer of NVIDIA | `yes`, `no`, `other` and a text such as `Article 10`, `1` | Doh-KDVP; Doh-Div 7 rows; Doh-DHO no data; Doh-Obr 14 rows (12 interest payments and 2 coupons) |
| 2023 | class of the unknown share, split ratio, payer of BAT, type of the unknown dividend, bond type, deposit total, exclusion (ruble share) | `capital`; `2 : 1`, an empty line (the row date), `yes`; `1`; `dividends`; `ordinary`; `yes`; `exclude` | Doh-KDVP without the ruble share; Doh-Div 6 rows; Doh-DHO 1 row; Doh-Obr 14 rows |
| 2024 | class of the unknown share, bond type (Bond B), deposit total, reverse split ratio, payer of NVIDIA, 30-day loss (Dollar Share), quantity, exclusion (Doh-Obr, Bond B), exclusion (Doh-KDVP, certificate) | `capital`; `discount`; `no`; `1 : 10`, an empty line, `yes`; `1`; `yes`; an empty line (the units left); `exclude`; `exclude` | Doh-KDVP without the certificate; Doh-Div 5 rows; Doh-DHO no data; Doh-Obr 3 rows without Bond B |
| 2025 | class of the unknown share, split ratio, quantity (2024 sale), payer of BAT, bond type (maturity), exclusion (missing history) | `capital`; `2 : 1`, an empty line, `yes`; an empty line; `1`; `ordinary`; `exclude` | Doh-KDVP without the missing-history share; Doh-Div 7 rows; Doh-DHO 1 row; Doh-Obr 3 rows |

Other answers to try: `no` to a loss question writes `F10` true and `yes` writes `F10` false with a
warning; `cancel` blocks; a deposit answer `yes` in 2024 makes Doh-DHO from EUR 427.80; `skip` on a
payer question leaves the payer empty with a warning; `exclude` on the unknown-class share removes it
(a warning says so).

## What the browser shows

Every question above has a screen in the browser, except the exclusion (failure) questions of the
ruble share, the certificate, the missing-history share and Bond B: the page still says it cannot
take them. Nothing is preselected on any screen.

| Year | Question screens | Result blocks |
| --- | --- | --- |
| 2022 | 30-day loss (2), treaty text (BR: text or no claim), payer (NVIDIA: two rows or skip) | Doh-Div rows with the treaty text of each country, the Krka dividend in the excluded items, Doh-Obr rows grouped into interest on cash and bond coupons, the fee block, benefits |
| 2023 | class of the unknown share, split ratio, payer (BAT), income type of the unknown dividend, bond type, deposit total (EUR 12.50, no cancel button) | the reminder to add other banks in the Doh-DHO card after `yes`, a payer that is not found in the rows, fees and benefits |
| 2024 | class, bond type (`discount`), deposit total (EUR 427.80), reverse split ratio, payer, 30-day loss, quantity | Doh-DHO no data after `no`, from EUR 427.80 and the reminder after `yes`; a coupon with tax in the export (evidence mark) |
| 2025 | class, split ratio, quantity, payer, bond type | Doh-DHO above EUR 1,000 with no reminder, the dividend without a name, benefits |

Not reachable from a Trade Republic export, covered by the frontend tests: an interest payment of
an unknown type (the choice `other_interest` and its kinds) and fees with a missing exchange rate.

## Doh-KDVP rows to check in eDavki

| Year | ISIN | Rows (`F1`/`F6` date, `F3`/`F7` units, `F4`/`F9` EUR per unit, `F10`) |
| --- | --- | --- |
| 2022 | `XX0000000101` | purchase 2022-01-12 10 @ 10; purchase 2022-03-15 5 @ 20; sale 2022-06-14 15 @ 30, `F10` true |
| 2022 | `XX0000000104` | purchase 2022-01-20 10 @ 10; sale 2022-04-04 10 @ 8, `F10` false after the answer `yes` |
| 2022 | `XX0000000105` | purchase 2022-07-01 4 @ 10; sale 2022-09-01 4 @ 8, `F10` false (blocked by the purchase of 2022-09-15) |
| 2022 | `XX0000000112` | purchase 2022-03-10 6 @ 9.02201371 (10 USD at 1.1084); sale 2022-09-14 6 @ 14.01401401 (14 USD at 0.999) |
| 2023 | `XX0000000107` | purchase 2022-02-02 4 @ 50 (5 units at 100 become 10 units at 50); sale 2023-06-14 4 @ 70 |
| 2023 | `XX0000000110` | three purchases of 0.33333333; sale 2023-03-01 0.99999999 @ 36 |
| 2023 | `XX0000000122` | purchase 2022-10-04 7 @ 10; two sale rows on 2023-12-12: 4 @ 13 and 3 @ 13.5 |
| 2024 | `XX0000000101` | purchase 2022-03-15 5 @ 20 (the earlier 2022 sale used the other lot); sale 2024-02-20 5 @ 35 |
| 2024 | `XX0000000108` | purchase 2022-04-12 1 @ 50 (10 units at 5 become 1 unit at 50); sale 2024-10-08 1 @ 60 |
| 2024 | `XX0000000109` | purchase 2024-02-06 0.99999999 @ 30; sale 2024-09-10 0.99999999 @ 36 (corrected from 1) |
| 2025 | `XX0000000104` | purchases 2022-01-20 10 @ 10 and 2022-04-20 4 @ 9; sale 2025-06-10 14 @ 12 |
| 2025 | `XX0000000123` | purchases 0.5, 0.5 and 0.2 units; sale 2025-09-02 1.2 @ 45 |

## Not in these files

- Units held more than 15 years (tax free): Trade Republic data cannot be that old. The unit tests
  cover the boundaries (`tests/unit`, the long-held cases).
- Several brokers and the second broker's split rows: Interactive Brokers is not supported yet.
- A conflicting duplicate (the same transaction ID with different facts) rejects the whole group;
  see `tests/test_data/csv/merger/conflict.csv`.
- A row type the parser does not know, a CSV with a wrong header or
  a broken record: they block forms, so they are in `tests/test_data/csv/` and the parser tests.
- The developer export (incomplete history written as an unsafe file): `tests/test_data/cli/developer_export.csv`.
