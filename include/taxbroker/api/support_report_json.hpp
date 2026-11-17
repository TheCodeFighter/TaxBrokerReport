#pragma once

#include "taxbroker/application_request.hpp"
#include "taxbroker/application_result.hpp"
#include "taxbroker/application_run.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace taxbroker::api {

/// The version of the support report layout.
inline constexpr unsigned kSupportReportVersion = 1;

/// The build that made the report. Plain values, so a test can fix them.
struct SupportReportApp {
    std::string mVersion;  ///< For example `2.0.0`.
    std::string mBuild;    ///< `release` or `debug`.
    std::string mPlatform; ///< For example `linux x86_64`.
};

/// The shape of a run: what was put in, as counts only. No file name, no content, no taxpayer.
struct SupportReportRun {
    int mYear{};
    FilingType mFilingType{}; ///< Always set by the creator.
    bool mDeveloperMode{};
    /// The number of files of each broker, in the order of the first file of each broker.
    std::vector<std::pair<Broker, std::size_t>> mBrokerFiles;
    std::size_t mEcbRateFiles{};
    std::size_t mReferenceFiles{};
};

/// The version and build of this program.
[[nodiscard]] SupportReportApp currentSupportReportApp();

/// The shape of a request. Call it before the request is moved into a run.
[[nodiscard]] SupportReportRun supportReportRun(const ApplicationRequest& aRequest);

/// The support report of a finished run as indented JSON (2 spaces) with a line feed at the end.
/// It holds the build, the run shape, the statuses, the open question types and every diagnostic
/// with its code, message key, stage, field, ISIN and source row. It never holds an amount, a
/// unit, a price, a name, a tax number, a transaction ID, a file name or a time.
[[nodiscard]] std::string serializeSupportReport(const SupportReportApp& aApp,
                                                 const SupportReportRun& aRun,
                                                 const ApplicationResult& aResult);

/// The support report of a run that a supplied file stopped. It has `inputFault` and no result.
[[nodiscard]] std::string serializeSupportReport(const SupportReportApp& aApp,
                                                 const SupportReportRun& aRun,
                                                 const ApplicationInputFault& aFault);

} // namespace taxbroker::api
