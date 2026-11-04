# Built-in ECB reference rates

These files are built into the program: the ECB euro foreign exchange reference rates of the
calendar years 2016 to 2025, one file per year. A user can replace a year with an ECB file of
their own (`--ecb-rates`) and add years.

Source: ECB statistics. Euro foreign exchange reference rates (dataset `EXR`, daily, spot,
average, against the euro). The values are unchanged; only the format was reduced to the three
columns the program reads.

## Format

`rates_<year>.csv`: UTF-8, comma-separated, `\n` line endings, one header row
`currency,date,rate`, then one row per currency and day, sorted by currency and then date.
`currency` is the ISO 4217 code, `date` is `YYYY-MM-DD`, and `rate` is foreign units per 1 EUR
exactly as the ECB gives it, with no extra zeros. Every date is in the year of the file name.
The header is exactly that text, and a row has three fields and no quotes: the program reads the
files with its own small reader, not with the general CSV reader.

## How the files were made

Each year was downloaded unmodified on *2026-11-04* from the ECB Data Portal:

```text
https://data-api.ecb.europa.eu/service/data/EXR/D..EUR.SP00.A?startPeriod=<year>-01-01&endPeriod=<year>-12-31&format=csvdata
```

and converted with the program's own reader, which applies all checks of a supplied file:

```text
scripts/dump_ecb_rates.sh --compact resources/ecb runtime/ecb/exr_<year>.csv ...
```

### Files summary

| Year | Rates | Currencies | First date | Last date |
| --- | ---: | ---: | --- | --- |
| 2016 | 8,995 | 35 | 2016-01-04 | 2016-12-30 |
| 2017 | 8,925 | 35 | 2017-01-02 | 2017-12-29 |
| 2018 | 9,158 | 36 | 2018-01-02 | 2018-12-31 |
| 2019 | 9,180 | 36 | 2019-01-02 | 2019-12-31 |
| 2020 | 9,080 | 36 | 2020-01-02 | 2020-12-31 |
| 2021 | 8,256 | 32 | 2021-01-04 | 2021-12-31 |
| 2022 | 8,009 | 32 | 2022-01-03 | 2022-12-30 |
| 2023 | 7,650 | 30 | 2023-01-02 | 2023-12-29 |
| 2024 | 7,680 | 30 | 2024-01-02 | 2024-12-31 |
| 2025 | 7,650 | 30 | 2025-01-02 | 2025-12-31 |

## Adding a year

Only complete years are built in. To add one: download the year as above, convert it with the
command above, add the year to `BUILT_IN_ECB_YEARS` in `src/CMakeLists.txt` and a row to the
table, and update the year range in `--ecb-rates` help, in the `scripts/report.sh` usage text, in
`THIRD_PARTY_NOTICES.txt` and in the tests that name the first and last built-in year.
