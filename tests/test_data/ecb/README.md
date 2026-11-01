# ECB rate file fixture

`exr_2025_synthetic.csv` has the 32 columns of the ECB Data Portal `csvdata` export of the `EXR`
dataset. Its four rows (GBP and USD on 2025-01-02 and 2025-01-03) carry invented rates, not ECB
data. `tests/integration/ecb_rate_file_integration_test.cpp` loads it.
