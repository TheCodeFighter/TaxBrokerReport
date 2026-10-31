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

`scripts/report.sh --synthetic` and the `CliReport.Binary` CTest use these files.

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
