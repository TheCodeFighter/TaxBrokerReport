# Synthetic income golden files

All values here are made up. None comes from a real export. Each file is the whole XML document
that `tests/integration/income_xml_integration_test.cpp` gets from a synthetic Trade Republic CSV
(parse, merge, income stages, writer) and compares byte for byte. The taxpayer is the one of
`tests/test_data/cli/taxpayer.json`. Every file is also checked against its FURS schema.

Status: values checked by hand and XSD-valid. eDavki draft import accepted on 2026-11-10 for
`Doh-Div_2025` (tax year moved to 2024, because a 2025 return already exists), `Doh-DHO_2025` and
`Doh-Obr_2025`, each with a tax number that passes eDavki's check. The only critical content
errors were the expected ones: the empty payer facts of the rows without a reference row, and the
foreign-tax evidence that is attached by hand (`docs/verification.md`). `Doh-Obr_2023_treaty`
was accepted on 2026-11-10 too (tax year moved to 2022), with its text, `true` and `Type` 4 rows.
`Doh-Obr_2023` is not imported; its rows have the shapes that were. On 2026-11-11 the CLI output for the
synthetic income fixture (four forms) and a taxed coupon with payer facts were imported too (`docs/verification.md`).

| File | Year, workflow in the file | Input and rates | Expected rows |
| --- | --- | --- | --- |
| `Doh-Div_2025.golden.xml` | 2025, `O` | Five dividends. USD per 1 EUR: 1.25 on 2025-03-14, 1.28 on 2025-05-15. | Apple USD 12.53 / 1.25 = 10.024: `Value` 10.02, `ForeignTax` 0.00. Johnson & Johnson USD 20.00 / 1.28 = 15.625: `Value` 15.63, `ForeignTax` 2.40 (EUR in the export), `ReliefStatement` with the US treaty text, `&` escaped. A fund with no reference row: no payer facts, no `SourceCountry`. A Slovenian payer (Krka): no row. A Brazilian payer with a non-ASCII address: `Value` 4.00. |
| `Doh-DHO_2025.golden.xml` | 2025, `O` | Three Trade Republic interest payments, EUR 0.33, 0.33 and 0.34, allowance answered "above". | One row for the bank: `InterestEarned` 1.00; totals 1.00 and 0.00. |
| `Doh-Obr_2025.golden.xml` | 2025, `O` | `tests/test_data/csv/traderepublic_bond_maturity_fixture.csv`, bond type "ordinary". USD per 1 EUR: 1.25 on 2025-02-14, 1.28 on 2025-08-14. | Two coupons of USD 20.00: `Value` 16.00 and 15.63 (20.00 / 1.28 = 15.625), `Type` 2, no payer facts, `ForeignTax` 0.00. The accrued interest paid with the purchase does not lower them. |
| `Doh-Obr_2023.golden.xml` | 2023, `O` (the run asks for a self-report) | A coupon of EUR 25.00 of a bond that is still held, and Trade Republic interest of EUR 3.45 on 2023-11-30 (before 6 December 2023, so cash interest). | `Type` 2, `Value` 25.00, no payer facts; `Type` 4, `Value` 3.45 with the broker's payer facts (`DE`). The test runs it with `FilingType::SelfReport`; the file is still an original (`SelfReport` false, workflow `O` / `Original`), because eDavki rejects the self-report workflow for the income forms. |
| `Doh-Obr_2023_treaty.golden.xml` | 2023, `O` | Two bonds bought on 2023-01-10 and kept, two taxed coupons, and Trade Republic interest of EUR 3.45 on 2023-11-30 (cash interest). All EUR. A supplied catalogue has both issuers (kind `bond`), a `bond_coupon` text for `US` and the Trade Republic broker payer. The treaty question for `DE` is answered yes. | Coupon of 2023-03-14: `Value` 25.00, `ForeignTax` 3.00, issuer in `US` with its payer facts, `ReliefStatement` `2. odstavek 11. člena` (the reference text). Coupon of 2023-06-16: `Value` 40.00, `ForeignTax` 6.00, issuer in `DE`, no text for `DE`, so `ReliefStatement` `DA`. Cash interest: `Type` 4, `Value` 3.45, `ForeignTax` 0.00, the broker's payer facts, no `ReliefStatement`. |

`tests/integration/cli_report_integration_test.cpp` (`TheIncomeFormsMatchTheGoldenFiles2025`) runs
`taxbroker report` on the rows of the `Doh-Div_2025`, `Doh-DHO_2025` and `Doh-Obr_2025` files and
compares the three files it writes with them byte for byte, so the command and the stages give
the same XML.

After an eDavki draft import of a file, change its status to "eDavki draft import accepted on
<date>" and record the import in `docs/verification.md`. A rejection fixes the writer and the
golden file together.
