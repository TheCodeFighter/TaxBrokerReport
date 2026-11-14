# Synthetic CLI fixtures

- `report_fixture.csv` is the `tax_rules.md` example "one sale uses two purchases" in 2024: 10 @ 10
  and 20 @ 20 bought, 15 @ 30 sold.
- `taxpayer.json` has every key of the taxpayer file, all made up: tax number `12345678`, the
  residence, and a synthetic name, address, birth date, email (`example.com`) and phone number.
- `Doh-KDVP_2024.golden.xml` is the expected `taxbroker report --year 2024` output for them. Its
  rows are those of the golden literal of
  `KdvpXmlIntegrationTest.TwoFilesWithASaleUsingTwoPurchasesWriteTheWholeDocument`; the header
  also has every taxpayer field, in the order of
  `KdvpXmlWriterTest.AllOptionalTaxpayerFieldsAreWrittenInSchemaOrder`.

- `report_income_fixture.csv` is 2024 income for `scripts/report.sh --synthetic` only (no test reads
  it): five dividends (Apple USD 12.50 and Johnson & Johnson USD 20.00 with EUR 2.40 foreign tax,
  a fund and a Brazilian payer without conversion, a Slovenian payer that is left out), three bank
  interest payments of EUR 1.00 in all, and a bond kept all year with two coupons of USD 20.00.
- `report_ecb_rates.csv` is a made-up 2024 USD rate file (1.20 on 2024-02-14, 1.25 on 2024-03-14,
  1.30 on 2024-05-15, 1.40 on 2024-08-14; they replace the built-in 2024 year). With it the run
  gives Doh-Div `Value` 10.00 (12.50 / 1.25) and 15.38 (20.00 / 1.30), and Doh-Obr `Value` 16.67
  (20.00 / 1.20) and 14.29 (20.00 / 1.40), Doh-DHO `InterestEarned` 1.00. Answer yes to the one
  Doh-DHO question.

`scripts/report.sh --synthetic` uses all of these files; the `CliReport.Binary` CTest uses only
`report_fixture.csv`, `taxpayer.json` and the golden file.

For trying the questions of `taxbroker report` by hand (no test reads them):

- `loss_question.csv`: 10 @ 30 bought and 10 @ 20 sold in 2025 with no purchase within 30 days,
  so the command asks the 30-day loss question. no writes the loss with F10 true, yes with F10
  false; cancel blocks the report.
- `split_question.csv`: 1 @ 10 bought in 2022, a split on 2023-01-15 and 2 @ 30 sold in 2025, so
  the command asks for the ratio. A confirmed ratio is applied (1 unit becomes 2 with 2 : 1), and
  the report lists the 2 units after the split at EUR 5 (`F3` 2, `F4` 5) and the sale of 2 at
  EUR 30.
- `exclusion_question.csv`: two investments in 2024. One is fine (1 @ 10 bought, 1 @ 30 sold); the
  other sells 2 after buying 1, so its purchase history is incomplete. Run it with `--year 2024`:
  the command asks the exclusion question. exclude writes the XML with the first investment only
  and lists the other in the summary; fail and cancel block the report.
- `developer_export.csv` and `Doh-KDVP_2024_developer.golden.xml`: 0.6 @ 100 bought in 2022, 0.25 @
  110 sold in 2023 (0.35 left), 0.5 @ 120 sold in 2024 (0.15 short, so the purchase history is
  incomplete), 1 @ 90 bought and 0.2 @ 95 sold. With `--developer-export`, the answer `developer`
  and `confirm` write `Doh-KDVP_2024_UNSAFE.xml`, which equals the golden file: the purchase of
  0.35 units open at the short sale (F8 0.35), the short sale in full without F10 (F8 0.35 - 0.5 =
  -0.15), the purchase of 1 (F8 0.85) and the last sale of 0.2 (F8 0.65). The golden file uses the
  minimal taxpayer of the CLI tests (`{"taxNumber": "12345678", "isResident": true}`).

