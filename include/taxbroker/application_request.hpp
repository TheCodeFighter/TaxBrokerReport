#pragma once

#include "taxbroker/event_metadata.hpp"
#include "taxbroker/input_file.hpp"
#include "taxbroker/report_models.hpp"

#include <vector>

namespace taxbroker {

/// Inclusive bounds of a reporting year that a run accepts.
inline constexpr int kMinimumReportingYear = 2000;
inline constexpr int kMaximumReportingYear = 9999;

/// One broker export held in memory, with the broker that wrote it.
struct BrokerInputFile {
    Broker mBroker{Broker::Unknown};
    InputFile mFile;
};

/// Everything one application run starts from, without HTTP or JSON. The answers are not part of
/// it: the decision session keeps them.
struct ApplicationRequest {
    int mReportingYear{};
    FilingType mFilingType{};
    TaxpayerMetadata mTaxpayer;
    /// In request order
    std::vector<BrokerInputFile> mBrokerFiles;
    /// Supplied ECB rate files; a file replaces the built-in year it covers. May be empty.
    std::vector<InputFile> mEcbRateFiles;
    /// Supplied reference files, named `payers.csv`, `relief_statements.csv` or
    /// `broker_payers.csv`. May be empty.
    std::vector<InputFile> mReferenceFiles;
};

} // namespace taxbroker
