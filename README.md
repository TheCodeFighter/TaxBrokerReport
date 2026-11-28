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

Today the `taxbroker report` command line writes the forms **Doh-KDVP** (capital gains),
**Doh-Div** (dividends), **Doh-DHO** and **Doh-Obr** (interest) for one tax year from **Trade
Republic** CSV exports and **Interactive Brokers** Flex Query XML files. The points below describe
the capital gains form:

- shares and funds (ETFs) bought and sold in EUR;
- units held longer than the holding period are left out as tax free and listed in the summary;
- the 30-day loss rule (*pravilo navidezne odsvojitve*): a loss that a purchase of the same
  investment within 30 days before or after the sale blocks in full is written with `F10` false,
  also across a split;
- questions in the terminal: when the files alone cannot decide a loss, the command asks whether,
  within 30 days of the sale, you also traded the investment anywhere not in your files (another
  broker or account, or after the export), and for a split it asks the ratio and the effective
  date from its announcement and applies the split to the earlier purchases. The answers are
  used only in that run and are never saved. When each problem that blocks the report is tied
  to a single investment (one or several, for example earlier purchases are missing), the
  command also asks, once, whether to write Doh-KDVP without all of them or to write none;
- the XML imports into eDavki as a draft; the checks so far are recorded in
  [docs/verification.md](docs/verification.md).

The tool blocks a report instead of guessing. No XML is written when an investment sold in the
year is crypto, a private fund, a bond or a derivative, has a merger, misses earlier purchases, has
a trade that is not in EUR, or has a question without an answer (with `--no-input`, or when you end
the input). When each such problem except an unanswered question is tied to a single
investment (one or several), the command asks whether to leave all of them out instead.
`scripts/report.sh --command-help` lists every case.

Not yet available: Windows and macOS builds, and a published release (the Linux packages are
built by CI, see "Install on Linux").

## Why TaxBrokerReport

- **Private:** it works offline. Your exports and your tax data never leave your computer. The
  browser app saves only the taxpayer details, only when you tick "Remember my details", in a
  private file on this computer that you can delete there.
- **Free and open source:** MIT licensed, every line can be checked.
- **Whole history:** FIFO runs over all the exports you give it, overlapping files included, so
  older purchases count, across all the brokers you give it.

## Install on Linux

The Linux x86-64 packages are three files from the same build, plus `SHA256SUMS`. CI makes them
for every change to `main` (the artifact `taxbroker-linux-x86_64-<commit>` of the CI run); the
first published release comes with the beta. They need no installation of libraries (the
programs are static). Check the files first: `sha256sum -c SHA256SUMS`.

- **tar.gz:** `tar -xzf taxbroker-2.0.0-linux-x86_64.tar.gz`, then `./taxbroker_server` in the
  new folder.
- **deb (Debian, Ubuntu):** `sudo apt install ./taxbroker_2.0.0_amd64.deb`. It adds TaxBrokerReport
  to the application menu and the commands `taxbroker` and `taxbroker_server`.
- **AppImage:** `chmod +x TaxBrokerReport-2.0.0-x86_64.AppImage`, then a double click, or run it.
  With arguments it runs the command line (`./TaxBrokerReport-2.0.0-x86_64.AppImage report --help`).
  It needs FUSE 2 (the `libfuse2` package) on some systems; `APPIMAGE_EXTRACT_AND_RUN=1` runs it
  without FUSE.

Start it from the application menu (deb), with a double click (AppImage) or with
`taxbroker_server` in a terminal: it
opens your browser on a one-time link (it works once, for 2 minutes). If the browser does not
open, copy the link that `taxbroker_server` prints into it. Stop it with the "Quit the program"
button in the page, or Ctrl+C or closing the window of the terminal it runs in. It also stops by
itself when you close its last browser tab (after 10 seconds, so a reload keeps it), when nobody
logs in within 3 minutes, and after 60 minutes without use. The command line
is `taxbroker report --help`; `taxbroker --version` prints the version.

To remove it: delete the unpacked folder (tar.gz), run `sudo apt remove taxbroker` (deb) or
delete the file (AppImage). The program keeps two folders of its own, which you delete yourself
if you no longer need them: `~/.local/share/taxbroker/` (the saved taxpayer details, only if you
chose to save them) and `~/.local/state/taxbroker/` (the log, which holds no amounts, names or
paths). The packages hold the disclaimer below in their README and licence files.

## Build and run

For now the tool is built and run in Docker on Linux. You need Docker Engine and Docker Compose.

```sh
scripts/build.sh image                # build the development image once
scripts/report.sh --synthetic         # try it on the synthetic example data
scripts/report.sh --command-help      # the options, the taxpayer file and every blocked case
```

For your own report, put your Trade Republic exports (CSV) and Interactive Brokers exports (Flex
Query XML) and a taxpayer file below the ignored `runtime/` folder, then run:

```sh
scripts/report.sh --year 2025 --taxpayer runtime/taxpayer.json \
  --trade-republic runtime/export.csv --ibkr runtime/flex_query.xml
```

Give your whole history, not only the tax year. The XML and a readable summary are written to
`runtime/report/2025/`. Use `--self-report` for a self-report (*samoprijava*) of Doh-KDVP; the
income forms are always originals. The command asks its questions in the terminal; `--no-input`
asks nothing and lists the open questions instead. `--developer-export` is for developers: when a
sale has no purchase history, it adds one more choice that writes an unsafe `Doh-KDVP_YYYY_UNSAFE.xml`
after one confirmation (eDavki may reject it, or the tax may be wrong).

`--support-report` also writes `support_YYYY.json`, a small file for a problem report: the version, the status of
each form and every diagnostic code with its ISIN and row. It holds no amounts, names, tax numbers or
file names; read it before you send it. The "Support" page has a "Save support report" button for the same file, and an "Advanced" section for the log level.

If something fails, run the command again with `--log-level info` and send the log file (it holds
no amounts, names or file names); see [docs/logging.md](docs/logging.md).

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
