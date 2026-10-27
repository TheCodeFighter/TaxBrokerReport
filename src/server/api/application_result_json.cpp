#include "taxbroker/api/application_result_json.hpp"
#include "json_strings.hpp"
#include "taxbroker/application_result.hpp"
#include "utils/numbered_id.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace {

using namespace taxbroker;
using api::detail::messageParamsToJson;
using api::detail::toString;

/// Keeps the keys in insertion order, which is the documented order of the application JSON.
using Json = nlohmann::ordered_json;

std::string_view toString(PipelineStage aStage) {
    switch (aStage)
    {
    case PipelineStage::Parsing:
        return "parsing";
    case PipelineStage::Merging:
        return "merging";
    case PipelineStage::HistoricalValidation:
        return "historical_validation";
    case PipelineStage::CorporateAction:
        return "corporate_action";
    case PipelineStage::FifoTax:
        return "fifo_tax";
    case PipelineStage::XmlGeneration:
        return "xml_generation";
    }

    return "parsing";
}

std::string_view toString(ReportType aType) {
    switch (aType)
    {
    case ReportType::CapitalGains:
        return "capital_gains";
    case ReportType::Dividends:
        return "dividends";
    case ReportType::BankDepositInterest:
        return "bank_deposit_interest";
    case ReportType::OtherInterest:
        return "other_interest";
    }

    return "capital_gains";
}

std::string_view reportKey(ReportType aType) {
    switch (aType)
    {
    case ReportType::CapitalGains:
        return "capitalGains";
    case ReportType::Dividends:
        return "dividends";
    case ReportType::BankDepositInterest:
        return "bankInterest";
    case ReportType::OtherInterest:
        return "otherInterest";
    }

    return "capitalGains";
}

std::string_view toString(MergeDiagnosticCode aCode) {
    switch (aCode)
    {
    case MergeDiagnosticCode::DuplicateSourceIndex:
        return "duplicate_source_index";
    case MergeDiagnosticCode::InconsistentSourceIndex:
        return "inconsistent_source_index";
    case MergeDiagnosticCode::ConflictingDuplicate:
        return "conflicting_duplicate";
    case MergeDiagnosticCode::InstrumentNameConflict:
        return "instrument_name_conflict";
    case MergeDiagnosticCode::InstrumentAssetClassConflict:
        return "instrument_asset_class_conflict";
    case MergeDiagnosticCode::DuplicateTransactionInFile:
        return "duplicate_transaction_in_file";
    }

    return "conflicting_duplicate";
}

std::string_view toString(ProcessingDiagnosticCode aCode) {
    switch (aCode)
    {
    case ProcessingDiagnosticCode::UnrepresentableValue:
        return "unrepresentable_value";
    case ProcessingDiagnosticCode::ArithmeticOverflow:
        return "arithmetic_overflow";
    case ProcessingDiagnosticCode::InvalidExchangeRate:
        return "invalid_exchange_rate";
    case ProcessingDiagnosticCode::CorporateActionUnrepresentable:
        return "corporate_action_unrepresentable";
    case ProcessingDiagnosticCode::InvalidReportingYear:
        return "invalid_reporting_year";
    case ProcessingDiagnosticCode::IncompleteHistory:
        return "incomplete_history";
    case ProcessingDiagnosticCode::XmlValueOutOfRange:
        return "xml_value_out_of_range";
    case ProcessingDiagnosticCode::UnsupportedTaxTreatment:
        return "unsupported_tax_treatment";
    case ProcessingDiagnosticCode::TaxClassificationUnknown:
        return "tax_classification_unknown";
    case ProcessingDiagnosticCode::CorporateActionNotApplied:
        return "corporate_action_not_applied";
    case ProcessingDiagnosticCode::MissingExchangeRate:
        return "missing_exchange_rate";
    case ProcessingDiagnosticCode::LossEligibilityUnresolved:
        return "loss_eligibility_unresolved";
    }

    return "unrepresentable_value";
}

std::string_view toString(RequestType aType) {
    switch (aType)
    {
    case RequestType::ExchangeRatesRequired:
        return "exchange_rates_required";
    case RequestType::CorporateActionIdentityRequired:
        return "corporate_action_identity_required";
    case RequestType::CorporateActionRatioRequired:
        return "corporate_action_ratio_required";
    case RequestType::QuantityCorrectionRequired:
        return "quantity_correction_required";
    case RequestType::LossEligibilityConfirmationRequired:
        return "loss_eligibility_confirmation_required";
    case RequestType::TaxClassificationRequired:
        return "tax_classification_required";
    case RequestType::IncomeDetailsRequired:
        return "income_details_required";
    case RequestType::CapitalGainsFailureResolutionRequired:
        return "capital_gains_failure_resolution_required";
    case RequestType::OtherInterestFailureResolutionRequired:
        return "other_interest_failure_resolution_required";
    }

    return "tax_classification_required";
}

std::string_view toString(ReportStatus aStatus) {
    switch (aStatus)
    {
    case ReportStatus::Generated:
        return "generated";
    case ReportStatus::NoData:
        return "no_data";
    case ReportStatus::NeedsInput:
        return "needs_input";
    case ReportStatus::Failed:
        return "failed";
    }

    return "failed";
}

std::string_view toString(OverallStatus aStatus) {
    switch (aStatus)
    {
    case OverallStatus::Success:
        return "success";
    case OverallStatus::CompletedWithWarnings:
        return "completed_with_warnings";
    case OverallStatus::PartialSuccess:
        return "partial_success";
    case OverallStatus::NeedsInput:
        return "needs_input";
    case OverallStatus::Failed:
        return "failed";
    }

    return "failed";
}

std::string_view toString(NoDataReason aReason) {
    switch (aReason)
    {
    case NoDataReason::BelowDepositInterestThreshold:
        return "below_deposit_interest_threshold";
    case NoDataReason::NoSelectedYearDisposals:
        return "no_selected_year_disposals";
    }

    return "no_selected_year_disposals";
}

std::string_view codeString(const ApplicationDiagnosticCode& aCode) {
    const auto toCodeString = [](const auto& aValue) { return toString(aValue); };

    return std::visit(toCodeString, aCode);
}

std::string isoDate(const Date& aDate) {
    const std::chrono::sys_days days{
        std::chrono::duration_cast<std::chrono::days>(aDate.time_since_epoch())};
    const std::chrono::year_month_day date{days};

    auto month = std::to_string(static_cast<unsigned>(date.month()));
    auto day = std::to_string(static_cast<unsigned>(date.day()));

    if (month.size() < 2)
    {
        month.insert(0, "0");
    }
    if (day.size() < 2)
    {
        day.insert(0, "0");
    }

    return std::to_string(static_cast<int>(date.year())) + "-" + month + "-" + day;
}

Json reportTypesToJson(const std::vector<ReportType>& aTypes) {
    Json types = Json::array();

    std::transform(aTypes.begin(), aTypes.end(), std::back_inserter(types), [](ReportType aType) {
        return Json(toString(aType));
    });

    return types;
}

Json sourcesToJson(const std::vector<ApplicationSource>& aSources) {
    Json sources = Json::array();

    for (const auto& source : aSources)
    {
        Json serialized{
            {"broker", toString(source.mBroker)},
            {"sourceId", numberedId("source", source.mSourceIndex + 1)},
        };

        // Every filename passes through the sanitizer again; a name that fails is left out.
        if (source.mFilename)
        {
            try
            {
                serialized["file"] = SourceFilename::fromPath(source.mFilename->value()).value();
            } catch (const std::invalid_argument&)
            {
                // The source ID still identifies the file.
            }
        }
        if (source.mRow)
        {
            serialized["row"] = *source.mRow;
        }
        if (source.mTransactionId)
        {
            serialized["transactionId"] = *source.mTransactionId;
        }

        sources.push_back(std::move(serialized));
    }

    return sources;
}

Json diagnosticToJson(const ApplicationDiagnostic& aDiagnostic) {
    Json serialized{
        {"id", aDiagnostic.mId},
        {"severity", toString(aDiagnostic.mSeverity)},
        {"stage", toString(aDiagnostic.mStage)},
        {"code", codeString(aDiagnostic.mCode)},
        {"message", aDiagnostic.mMessage},
    };

    // The keys follow the order of the JSON example in diagnostics.md.
    if (aDiagnostic.mMessageKey)
    {
        serialized["messageKey"] = messageDefinition(*aDiagnostic.mMessageKey).mId;
        serialized["messageParams"] = messageParamsToJson(aDiagnostic.mMessageParams);
    }

    serialized["affectedReports"] = reportTypesToJson(aDiagnostic.mAffectedReports);
    serialized["blocking"] = aDiagnostic.mBlocking;
    serialized["sources"] = sourcesToJson(aDiagnostic.mSources);

    if (aDiagnostic.mTaxDate)
    {
        serialized["taxDate"] = isoDate(*aDiagnostic.mTaxDate);
    }
    if (aDiagnostic.mField)
    {
        serialized["field"] = *aDiagnostic.mField;
    }
    if (aDiagnostic.mInstrumentName || aDiagnostic.mIsin)
    {
        Json instrument = Json::object();

        if (aDiagnostic.mInstrumentName)
        {
            instrument["name"] = *aDiagnostic.mInstrumentName;
        }
        if (aDiagnostic.mIsin)
        {
            instrument["isin"] = *aDiagnostic.mIsin;
        }

        serialized["instrument"] = std::move(instrument);
    }
    if (aDiagnostic.mRequestId)
    {
        serialized["requestId"] = *aDiagnostic.mRequestId;
    }

    return serialized;
}

Json reasonCodesToJson(const std::vector<ApplicationDiagnosticCode>& aCodes) {
    Json codes = Json::array();

    std::transform(aCodes.begin(), aCodes.end(), std::back_inserter(codes), [](const auto& aCode) {
        return Json(codeString(aCode));
    });

    return codes;
}

std::string_view toString(FailureResolution aResolution) {
    switch (aResolution)
    {
    case FailureResolution::ExcludeFailedInstruments:
        return "exclude_failed_instruments";
    case FailureResolution::FailReport:
        return "fail_report";
    }

    return "fail_report";
}

std::string_view answerString(const AnswerValue& aAnswer) {
    const auto toAnswerString = [](const auto& aValue) -> std::string_view {
        using Value = std::decay_t<decltype(aValue)>;

        if constexpr (std::is_same_v<Value, FailureResolutionAnswer>)
        {
            return toString(aValue.mChoice);
        }
        else
        {
            static_assert(std::is_same_v<Value, CancelAnswer>);
            return "cancel";
        }
    };

    return std::visit(toAnswerString, aAnswer);
}

Json requestToJson(const DecisionRequest& aRequest) {
    // The keys follow the order of the JSON example in diagnostics.md.
    Json serialized{
        {"id", aRequest.mId},
        {"type", toString(aRequest.mType)},
        {"runRevision", aRequest.mRunRevision},
        {"affectedReports", reportTypesToJson(aRequest.mAffectedReports)},
        {"sources", sourcesToJson(aRequest.mSources)},
    };

    if (aRequest.mTaxDate)
    {
        serialized["taxDate"] = isoDate(*aRequest.mTaxDate);
    }
    if (aRequest.mIsin)
    {
        serialized["isin"] = *aRequest.mIsin;
    }

    // Only the two failure-resolution types have question data so far.
    if (std::holds_alternative<FailureResolutionQuestion>(aRequest.mData))
    {
        Json investments = Json::array();

        std::transform(aRequest.mInvestments.begin(),
                       aRequest.mInvestments.end(),
                       std::back_inserter(investments),
                       [](const RequestInvestment& aInvestment) {
                           return Json{
                               {"name", aInvestment.mName},
                               {"isin", aInvestment.mIsin},
                               {"reasonCodes", reasonCodesToJson(aInvestment.mReasonCodes)},
                               {"diagnosticIds", aInvestment.mDiagnosticIds},
                           };
                       });

        serialized["options"] = Json::array({toString(FailureResolution::ExcludeFailedInstruments),
                                             toString(FailureResolution::FailReport)});
        serialized["investments"] = std::move(investments);
    }

    return serialized;
}

/// A recorded answer: its type, question data and answer. It has no sources, because they are
/// run-local and the answer may come from an earlier run.
Json decisionToJson(const RecordedDecision& aDecision) {
    Json serialized{
        {"id", aDecision.mId},
        {"type", toString(aDecision.mType)},
        {"fromEarlierRun", aDecision.mFromEarlierRun},
        {"used", aDecision.mUsed},
    };

    if (const auto* failure = std::get_if<FailureResolutionQuestion>(&aDecision.mData))
    {
        Json investments = Json::array();

        std::transform(failure->mInvestments.begin(),
                       failure->mInvestments.end(),
                       std::back_inserter(investments),
                       [](const FailedInvestment& aInvestment) {
                           return Json{
                               {"name", aInvestment.mName},
                               {"isin", aInvestment.mIsin},
                               {"reasonCodes", reasonCodesToJson(aInvestment.mReasonCodes)},
                           };
                       });

        serialized["investments"] = std::move(investments);
    }

    serialized["answer"] = answerString(aDecision.mAnswer);

    return serialized;
}

Json reportToJson(ReportType aType, const ReportResult& aReport) {
    Json serialized{
        {"status", toString(aReport.mStatus)},
        {"diagnosticIds", aReport.mDiagnosticIds},
        {"requestIds", aReport.mRequestIds},
    };

    // Only the two forms with a failure choice list excluded investments; none exist yet.
    if (aType == ReportType::CapitalGains || aType == ReportType::OtherInterest)
    {
        serialized["excludedInvestments"] = Json::array();
    }

    serialized["unsafe"] = aReport.mUnsafe;

    if (aReport.mStatus == ReportStatus::Generated)
    {
        serialized["downloadAvailable"] = true;
    }
    if (aReport.mNoDataReason)
    {
        serialized["reason"] = toString(*aReport.mNoDataReason);
    }

    return serialized;
}

} // namespace

namespace taxbroker::api {

std::string_view diagnosticCodeName(const ApplicationDiagnosticCode& aCode) {
    return codeString(aCode);
}

std::string serializeApplicationResultJson(const ApplicationResult& aResult, int aIndent) {
    Json reports = Json::object();

    for (std::size_t index = 0; index < aResult.mReports.size(); ++index)
    {
        const auto type = static_cast<ReportType>(index);

        reports[std::string{reportKey(type)}] = reportToJson(type, aResult.mReports[index]);
    }

    Json diagnostics = Json::array();

    std::transform(aResult.mDiagnostics.begin(),
                   aResult.mDiagnostics.end(),
                   std::back_inserter(diagnostics),
                   diagnosticToJson);

    Json requests = Json::array();

    std::transform(aResult.mRequests.begin(),
                   aResult.mRequests.end(),
                   std::back_inserter(requests),
                   requestToJson);

    Json decisions = Json::array();

    std::transform(aResult.mDecisions.begin(),
                   aResult.mDecisions.end(),
                   std::back_inserter(decisions),
                   decisionToJson);

    const Json response{
        {"schemaVersion", kApplicationResultSchemaVersion},
        {"status", toString(aResult.mStatus)},
        {"runRevision", aResult.mRunRevision},
        {"reports", std::move(reports)},
        {"diagnostics", std::move(diagnostics)},
        {"requests", std::move(requests)},
        {"decisions", std::move(decisions)},
    };

    return response.dump(aIndent, ' ', false, Json::error_handler_t::replace);
}

} // namespace taxbroker::api
