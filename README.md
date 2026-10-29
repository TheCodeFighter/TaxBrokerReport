# TaxBrokerReport

<p align="center">
  <img src="docs/assets/icon.jpg" alt="TaxBrokerReport Icon" width="200"/>
</p>

**TaxBrokerReport** is a free, open-source tool for Slovenian investors. It turns broker CSV
exports into FURS XML files that you import into eDavki.

> [!WARNING]
>
> ## This project is currently under major reconstruction
>
> ### The legacy codebase, together with the full legacy documentation, is available on the [main-legacy branch](https://github.com/TheCodeFighter/TaxBrokerReport/tree/main-legacy)
>

## Status

Today the `taxbroker report` command line writes the **Doh-KDVP** form (capital gains) for one
tax year from **Trade Republic** CSV exports:

- shares and funds (ETFs) bought and sold in EUR;
- units held longer than the holding period are left out as tax free and listed in the summary;
- the 30-day loss rule (*pravilo navidezne odsvojitve*): a loss that a purchase of the same
  investment within 30 days before or after the sale blocks in full is written with `F10` false;
- questions in the terminal: when the files alone cannot decide a loss, the command asks whether,
  within 30 days of the sale, you also traded the investment anywhere not in your files (another
  broker or account, or after the export), and for a split it asks the ratio and the effective
  date from its announcement and applies the split to the earlier purchases. The answers are
  used only in that run and are never saved;
- the XML imports into eDavki as a draft; the checks so far are recorded in
  [docs/verification.md](docs/verification.md).

The tool blocks a report instead of guessing. No XML is written when an investment sold in the
year is crypto, a private fund, a bond or a derivative, has a merger, sells units that a split
changed or sells at a loss near a split (both are not supported yet), misses earlier
purchases, has a trade that is not in EUR, or has a question without an answer (with
`--no-input`, or when you end the input). `scripts/report.sh --command-help` lists every case.

Not yet available: Doh-Div (dividends), Doh-DHO and Doh-Obr (interest), Interactive Brokers, the
browser app, Windows and macOS builds, and downloadable releases.

## Why TaxBrokerReport

- **Private:** it works offline. Your exports and your tax data never leave your computer.
- **Free and open source:** MIT licensed, every line can be checked.
- **Whole history:** FIFO runs over all the exports you give it, overlapping files included, so
  older purchases count. FIFO across brokers is planned with Interactive Brokers.

## Build and run

For now the tool is built and run in Docker on Linux. You need Docker Engine and Docker Compose.

```sh
scripts/build.sh image                # build the development image once
scripts/report.sh --synthetic         # try it on the synthetic example data
scripts/report.sh --command-help      # the options, the taxpayer file and every blocked case
```

For your own report, put your Trade Republic exports and a taxpayer file below the ignored
`runtime/` folder, then run:

```sh
scripts/report.sh --year 2025 --taxpayer runtime/taxpayer.json runtime/export.csv
```

Give your whole history, not only the tax year. The XML and a readable summary are written to
`runtime/report/2025/`. Use `--self-report` for a self-report (*samoprijava*). The command asks
its questions in the terminal; `--no-input` asks nothing and lists the open questions instead.

More detail: [docker/docker_doc.md](docker/docker_doc.md) (the Docker setup),
[docs/development.md](docs/development.md) (builds, tests, checks and debug dumps) and
[docs/logging.md](docs/logging.md) (logging).

## Disclaimer

This is an unofficial community tool. It is not tax advice, and it is not affiliated with FURS or
any broker. You are responsible for checking every value in the generated files and for filing
your return in eDavki.

This software is provided "as is" under the [MIT License](LICENSE), without warranty of any kind.
The developer assumes no liability for tax errors, fines or omissions.

## Contributing and support

Contributions are welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md) and the
[Code of Conduct](CODE_OF_CONDUCT.md) before contributing. Report security vulnerabilities
privately by following [SECURITY.md](SECURITY.md).

Questions or suggestions: [tax.brokerage.report@gmail.com](mailto:tax.brokerage.report@gmail.com).

If this tool saves you time or money, please consider a donation to
[Karitas Slovenija](https://www.karitas.si/daruj/donacija/) to help families in need.

TaxBrokerReport is available under the [MIT License](LICENSE).
