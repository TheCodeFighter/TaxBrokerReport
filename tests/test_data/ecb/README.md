# ECB rate file fixture

`exr_2025_synthetic.csv` has the 32 columns of the ECB Data Portal `csvdata` export of the `EXR`
dataset. Its four rows (GBP and USD on 2025-01-02 and 2025-01-03) carry invented rates, not ECB
data. `tests/integration/ecb_rate_file_integration_test.cpp` loads it.

`exr_2024_synthetic.csv` has the same columns and two USD rows (2024-12-30 and 2024-12-31) with
invented rates. Its first date is late in the year, so it also tests the late-start warning. Together with the 2025 file it covers the turn of the year in the rate-selection
integration tests.

`exr_2015_synthetic.csv` has the same columns and two USD rows (2015-01-02 and 2015-12-31) with
invented rates (1.2 and 1.1). 2015 is a year without built-in rates, so the file is added to the
built-in years in the tests of `loadOfficialRates`.
