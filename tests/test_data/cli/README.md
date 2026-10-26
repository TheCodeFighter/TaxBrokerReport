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
