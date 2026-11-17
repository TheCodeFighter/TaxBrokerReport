#include "taxbroker/api/support_report_json.hpp"
#include "taxbroker/application_result.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <utility>

namespace {

using namespace taxbroker;

using Json = nlohmann::json;

api::SupportReportApp fixedApp() {
    return {.mVersion = "2.0.0", .mBuild = "release", .mPlatform = "linux x86_64"};
}

api::SupportReportRun fixedRun() {
    return {.mYear = 2024, .mFilingType = FilingType::SelfReport, .mDeveloperMode = true};
}

Json serialize(const ApplicationResult& aResult) {
    return Json::parse(api::serializeSupportReport(fixedApp(), fixedRun(), aResult));
}

TEST(SupportReportJsonUnitTest, TheRunHoldsTheFilingTypeAndTheDeveloperModeAsWords) {
    ApplicationResultInput input{.mReportingYear = 2024};
    const Json report = serialize(ApplicationResultBuilder::build(std::move(input)));

    EXPECT_EQ(report["run"]["filing"], "self_report");
    EXPECT_EQ(report["run"]["developerMode"], true);
    EXPECT_EQ(report["run"]["brokers"], Json::array());
}

TEST(SupportReportJsonUnitTest, ADiagnosticKeepsItsFieldAndDateAndDropsTheNameParameter) {
    ApplicationResultInput input{.mReportingYear = 2024};
    // A message with a `name` parameter. The name can be the name of something private, so the
    // report keeps the key and leaves the parameter out.
    auto message = makeDiagnosticMessage(MessageKey::ReferenceFileUnknownName,
                                         {textParam("name", "Private Name.csv")});

    input.mProcessingDiagnostics = {ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Warning,
        .mCode = ProcessingDiagnosticCode::DividendPayerNotFound,
        .mContext = {.mStage = PipelineStage::FifoTax,
                     .mAffectedReports = {ReportType::Dividends},
                     .mTaxDate = parseCalendarDate("2024-06-20"),
                     .mField = "amount",
                     .mIsin = "XX0000000010"},
        .mMessage = std::move(message.mEnglish),
        .mMessageKey = message.mKey,
        .mMessageParams = std::move(message.mParams),
    }};

    const Json report = serialize(ApplicationResultBuilder::build(std::move(input)));
    const Json& diagnostic = report["diagnostics"][0];

    EXPECT_EQ(diagnostic["code"], "dividend_payer_not_found");
    EXPECT_EQ(diagnostic["messageKey"], "reference_file.unknown_name");
    EXPECT_EQ(diagnostic["messageParams"], Json::object());
    EXPECT_EQ(diagnostic["field"], "amount");
    EXPECT_EQ(diagnostic["isin"], "XX0000000010");
    EXPECT_EQ(diagnostic["taxDate"], "2024-06-20");
    EXPECT_EQ(diagnostic["affectedReports"], Json::parse(R"(["dividends"])"));
    EXPECT_EQ(diagnostic["sources"], Json::array());
    EXPECT_EQ(report.dump().find("Private Name"), std::string::npos);
}

TEST(SupportReportJsonUnitTest, ACountryParameterStaysInTheMessageParams) {
    ApplicationResultInput input{.mReportingYear = 2024};
    auto message =
        makeDiagnosticMessage(MessageKey::TreatyClaimDetailsMissing, {textParam("country", "US")});

    input.mProcessingDiagnostics = {ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Warning,
        .mCode = ProcessingDiagnosticCode::TreatyClaimDetailsMissing,
        .mContext = {.mStage = PipelineStage::FifoTax,
                     .mAffectedReports = {ReportType::OtherInterest}},
        .mMessage = std::move(message.mEnglish),
        .mMessageKey = message.mKey,
        .mMessageParams = std::move(message.mParams),
    }};

    const Json report = serialize(ApplicationResultBuilder::build(std::move(input)));

    // `country` is a country code, not a name, so it stays.
    EXPECT_EQ(report["diagnostics"][0]["messageParams"], Json::parse(R"({"country": "US"})"));
    EXPECT_EQ(report["result"]["counts"]["warnings"], 1);
    EXPECT_EQ(report["result"]["counts"]["byCode"],
              Json::parse(R"({"treaty_claim_details_missing": 1})"));
}

TEST(SupportReportJsonUnitTest, ADohObrFailureQuestionListsItsBrokerAndTheReasonCode) {
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mProcessingDiagnostics = {ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::MissingExchangeRate,
        .mContext = {.mStage = PipelineStage::FifoTax,
                     .mAffectedReports = {ReportType::OtherInterest},
                     .mCashInterestBroker = Broker::TradeRepublic},
    }};
    input.mRequests = {RequestInput{
        .mQuestion =
            {
                .mType = RequestType::OtherInterestFailureResolutionRequired,
                .mAffectedReports = {ReportType::OtherInterest},
                .mData =
                    FailureResolutionQuestion{
                        .mInvestments = {},
                        .mBrokers = {FailedBroker{
                            .mBroker = Broker::TradeRepublic,
                            .mReasonCodes = {ProcessingDiagnosticCode::MissingExchangeRate},
                        }}},
            },
        .mProcessingDiagnostics = {0},
    }};

    const Json report = serialize(ApplicationResultBuilder::build(std::move(input)));
    const Json& request = report["result"]["requests"][0];

    EXPECT_EQ(request["type"], "other_interest_failure_resolution_required");
    EXPECT_FALSE(request.contains("investments"));
    EXPECT_EQ(request["brokers"], Json::parse(R"([{"broker": "trade_republic",
                               "reasonCodes": ["missing_exchange_rate"]}])"));
    EXPECT_EQ(report["result"]["reports"]["otherInterest"]["requestTypes"],
              Json::parse(R"(["other_interest_failure_resolution_required"])"));
}

TEST(SupportReportJsonUnitTest, AnExclusionIsCountedAndTheAnswerIsNamed) {
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mProcessingDiagnostics = {ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::IncompleteHistory,
        .mContext = {.mStage = PipelineStage::FifoTax,
                     .mAffectedReports = {ReportType::CapitalGains},
                     .mInstrumentName = "Private Name",
                     .mIsin = "XX0000000001"},
        .mMessage = "Synthetic error.",
    }};
    input.mResolvedDiagnostics = {0};
    input.mReports[0].mFailureResolution = FailureResolution::ExcludeFailedInstruments;
    input.mReports[0].mExcludedInvestments = {{
        .mIsin = "XX0000000001",
        .mName = "Private Name",
        .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
    }};

    const Json report = serialize(ApplicationResultBuilder::build(std::move(input)));

    EXPECT_EQ(report["result"]["reports"]["capitalGains"]["excludedCount"], 1);
    EXPECT_EQ(
        report["result"]["answered"],
        Json::parse(R"([{"report": "capitalGains", "answer": "exclude_failed_instruments"}])"));
    EXPECT_EQ(report.dump().find("Private Name"), std::string::npos);
}

} // namespace
