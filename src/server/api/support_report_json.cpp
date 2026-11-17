#include "taxbroker/api/support_report_json.hpp"

#include "taxbroker/api/answer_json.hpp"
#include "taxbroker/build_info.hpp"
#include "json_strings.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iterator>
#include <map>
#include <string>
#include <string_view>

namespace {

using namespace taxbroker;
using api::detail::codeString;
using api::detail::isoDate;
using api::detail::reportKey;
using api::detail::toString;

/// Keeps the keys in insertion order, which is the documented order of the support report.
using Json = nlohmann::ordered_json;

std::string_view filingWord(FilingType aType) {
    return aType == FilingType::SelfReport ? "self_report" : "original";
}

/// The message parameters without `name`, which can hold the name of an instrument.
Json messageParamsWithoutName(const std::vector<MessageParam>& aParams) {
    std::vector<MessageParam> kept;

    std::copy_if(aParams.begin(),
                 aParams.end(),
                 std::back_inserter(kept),
                 [](const MessageParam& aParam) { return aParam.mName != "name"; });

    return api::detail::messageParamsToJson(kept);
}

Json appJson(const api::SupportReportApp& aApp) {
    return Json{{"version", aApp.mVersion}, {"build", aApp.mBuild}, {"platform", aApp.mPlatform}};
}

Json runJson(const api::SupportReportRun& aRun) {
    Json brokers = Json::array();

    for (const auto& [broker, files] : aRun.mBrokerFiles)
    {
        brokers.push_back(Json{{"broker", toString(broker)}, {"files", files}});
    }

    return Json{{"year", aRun.mYear},
                {"filing", filingWord(aRun.mFilingType)},
                {"developerMode", aRun.mDeveloperMode},
                {"brokers", std::move(brokers)},
                {"ecbRateFiles", aRun.mEcbRateFiles},
                {"referenceFiles", aRun.mReferenceFiles}};
}

/// The source number is the 1-based place of the file in the request; never its name.
Json sourcesJson(const std::vector<ApplicationSource>& aSources) {
    Json sources = Json::array();

    for (const ApplicationSource& source : aSources)
    {
        Json serialized{{"source", source.mSourceIndex + 1}};

        if (source.mRow)
        {
            serialized["row"] = *source.mRow;
        }

        sources.push_back(std::move(serialized));
    }

    return sources;
}

Json diagnosticJson(const ApplicationDiagnostic& aDiagnostic) {
    Json affected = Json::array();

    std::transform(aDiagnostic.mAffectedReports.begin(),
                   aDiagnostic.mAffectedReports.end(),
                   std::back_inserter(affected),
                   [](ReportType aType) { return Json(reportKey(aType)); });

    Json serialized{
        {"id", aDiagnostic.mId},
        {"severity", toString(aDiagnostic.mSeverity)},
        {"stage", toString(aDiagnostic.mStage)},
        {"code", codeString(aDiagnostic.mCode)},
    };

    if (aDiagnostic.mMessageKey)
    {
        serialized["messageKey"] = messageDefinition(*aDiagnostic.mMessageKey).mId;
        serialized["messageParams"] = messageParamsWithoutName(aDiagnostic.mMessageParams);
    }

    if (aDiagnostic.mField)
    {
        serialized["field"] = *aDiagnostic.mField;
    }
    if (aDiagnostic.mIsin)
    {
        serialized["isin"] = *aDiagnostic.mIsin;
    }
    if (aDiagnostic.mTaxDate)
    {
        serialized["taxDate"] = isoDate(*aDiagnostic.mTaxDate);
    }

    serialized["affectedReports"] = std::move(affected);
    serialized["sources"] = sourcesJson(aDiagnostic.mSources);

    return serialized;
}

/// The reason codes of a failed investment or broker, for example `["incomplete_history"]`.
Json reasonCodesJson(const std::vector<ApplicationDiagnosticCode>& aCodes) {
    Json codes = Json::array();

    std::transform(aCodes.begin(),
                   aCodes.end(),
                   std::back_inserter(codes),
                   [](const ApplicationDiagnosticCode& aCode) { return Json(codeString(aCode)); });

    return codes;
}

/// The type of every request of the report, in the order of its request IDs.
Json requestTypesJson(const ApplicationResult& aResult, const ReportResult& aReport) {
    Json types = Json::array();

    for (const std::string& id : aReport.mRequestIds)
    {
        const auto request =
            std::find_if(aResult.mRequests.begin(),
                         aResult.mRequests.end(),
                         [&id](const DecisionRequest& aRequest) { return aRequest.mId == id; });

        if (request != aResult.mRequests.end())
        {
            types.push_back(toString(request->mType));
        }
    }

    return types;
}

Json reportJson(const ApplicationResult& aResult, const ReportResult& aReport) {
    Json serialized{{"status", toString(aReport.mStatus)}};

    if (aReport.mNoDataReason)
    {
        serialized["noDataReason"] = toString(*aReport.mNoDataReason);
    }

    serialized["unsafe"] = aReport.mUnsafe;
    serialized["diagnosticIds"] = aReport.mDiagnosticIds;
    serialized["requestTypes"] = requestTypesJson(aResult, aReport);
    serialized["excludedCount"] =
        aReport.mExcludedInvestments.size() + aReport.mExcludedBrokers.size();

    return serialized;
}

Json resultJson(const ApplicationResult& aResult) {
    Json reports = Json::object();
    Json answered = Json::array();

    for (std::size_t index = 0; index < aResult.mReports.size(); ++index)
    {
        const auto type = static_cast<ReportType>(index);
        const ReportResult& report = aResult.mReports[index];

        reports[std::string{reportKey(type)}] = reportJson(aResult, report);

        if (report.mFailureResolution)
        {
            answered.push_back(Json{{"report", reportKey(type)},
                                    {"answer", toString(*report.mFailureResolution)}});
        }
    }

    Json requests = Json::array();

    for (const DecisionRequest& request : aResult.mRequests)
    {
        Json serialized{{"id", request.mId}, {"type", toString(request.mType)}};

        if (request.mIsin)
        {
            serialized["isin"] = *request.mIsin;
        }

        // The investments of a failure question have an ISIN and the codes of their problems; a
        // broker of a Doh-Obr failure question has only the codes.
        if (!request.mInvestments.empty())
        {
            Json investments = Json::array();

            std::transform(request.mInvestments.begin(),
                           request.mInvestments.end(),
                           std::back_inserter(investments),
                           [](const RequestInvestment& aInvestment) {
                               return Json{
                                   {"isin", aInvestment.mIsin},
                                   {"reasonCodes", reasonCodesJson(aInvestment.mReasonCodes)}};
                           });

            serialized["investments"] = std::move(investments);
        }

        if (!request.mBrokers.empty())
        {
            Json brokers = Json::array();

            std::transform(request.mBrokers.begin(),
                           request.mBrokers.end(),
                           std::back_inserter(brokers),
                           [](const RequestBroker& aBroker) {
                               return Json{{"broker", toString(aBroker.mBroker)},
                                           {"reasonCodes", reasonCodesJson(aBroker.mReasonCodes)}};
                           });

            serialized["brokers"] = std::move(brokers);
        }

        requests.push_back(std::move(serialized));
    }

    std::size_t errors = 0;
    std::size_t warnings = 0;
    std::map<std::string, std::size_t> byCode;

    for (const ApplicationDiagnostic& diagnostic : aResult.mDiagnostics)
    {
        ++(diagnostic.mSeverity == DiagnosticSeverity::Error ? errors : warnings);
        ++byCode[std::string{codeString(diagnostic.mCode)}];
    }

    Json codes = Json::object();

    for (const auto& [code, count] : byCode)
    {
        codes[code] = count;
    }

    return Json{{"revision", aResult.mRunRevision},
                {"status", toString(aResult.mStatus)},
                {"reports", std::move(reports)},
                {"requests", std::move(requests)},
                {"answered", std::move(answered)},
                {"counts",
                 Json{{"errors", errors},
                      {"warnings", warnings},
                      {"requests", aResult.mRequests.size()},
                      {"byCode", std::move(codes)}}}};
}

std::string dumpReport(const Json& aReport) {
    return aReport.dump(2, ' ', false, Json::error_handler_t::replace) + "\n";
}

} // namespace

namespace taxbroker::api {

SupportReportApp currentSupportReportApp() {
    return {.mVersion = std::string{buildVersion()},
            .mBuild = std::string{buildTypeName()},
            .mPlatform = buildSystemName()};
}

SupportReportRun supportReportRun(const ApplicationRequest& aRequest) {
    SupportReportRun run{.mYear = aRequest.mReportingYear,
                         .mFilingType = aRequest.mFilingType,
                         .mDeveloperMode = aRequest.mDeveloperMode,
                         .mEcbRateFiles = aRequest.mEcbRateFiles.size(),
                         .mReferenceFiles = aRequest.mReferenceFiles.size()};

    for (const BrokerInputFile& file : aRequest.mBrokerFiles)
    {
        const auto known =
            std::find_if(run.mBrokerFiles.begin(),
                         run.mBrokerFiles.end(),
                         [&file](const auto& aEntry) { return aEntry.first == file.mBroker; });

        if (known == run.mBrokerFiles.end())
        {
            run.mBrokerFiles.emplace_back(file.mBroker, 1);
        }
        else
        {
            ++known->second;
        }
    }

    return run;
}

std::string serializeSupportReport(const SupportReportApp& aApp,
                                   const SupportReportRun& aRun,
                                   const ApplicationResult& aResult) {
    Json diagnostics = Json::array();

    std::transform(aResult.mDiagnostics.begin(),
                   aResult.mDiagnostics.end(),
                   std::back_inserter(diagnostics),
                   diagnosticJson);

    return dumpReport(Json{{"supportReportVersion", kSupportReportVersion},
                           {"app", appJson(aApp)},
                           {"run", runJson(aRun)},
                           {"result", resultJson(aResult)},
                           {"diagnostics", std::move(diagnostics)}});
}

std::string serializeSupportReport(const SupportReportApp& aApp,
                                   const SupportReportRun& aRun,
                                   const ApplicationInputFault& aFault) {
    // `<kind> <message id>`, the same words as the log line of the fault.
    const std::string reason = inputFaultLogReason(aFault);
    const auto space = reason.find(' ');

    return dumpReport(
        Json{{"supportReportVersion", kSupportReportVersion},
             {"app", appJson(aApp)},
             {"run", runJson(aRun)},
             {"inputFault",
              Json{{"kind", reason.substr(0, space)}, {"messageKey", reason.substr(space + 1)}}}});
}

} // namespace taxbroker::api
