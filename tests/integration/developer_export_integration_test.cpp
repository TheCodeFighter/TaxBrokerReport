#include "taxbroker/application_request.hpp"
#include "taxbroker/application_run.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace taxbroker;

// All rows are synthetic. The header is the one of the Trade Republic export.
constexpr std::string_view kTradeRepublicHeader =
    "datetime,date,account_type,category,type,asset_class,name,symbol,shares,price,amount,fee,tax,"
    "currency,original_amount,original_currency,fx_rate,description,transaction_id,"
    "counterparty_name,counterparty_iban,payment_reference,mcc_code\n";

// XX0000000001, reporting year 2024: 0.6 units are bought, 0.25 are sold in 2023 (0.35 are left),
// a sale of 0.5 units in 2024 is 0.15 units short, then 1 unit is bought and 0.2 units are sold.
constexpr std::string_view kBuy2022 =
    R"(2022-03-01T09:00:00Z,2022-03-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,0.6,100,-60,,,EUR,,,,,synthetic-buy-2022,,,,)";
constexpr std::string_view kSell2023 =
    R"(2023-08-01T09:00:00Z,2023-08-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-0.25,110,27.5,,,EUR,,,,,synthetic-sell-2023,,,,)";
constexpr std::string_view kShortSale =
    R"(2024-05-01T09:00:00Z,2024-05-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-0.5,120,60,,,EUR,,,,,synthetic-sell-short,,,,)";
constexpr std::string_view kBuy2024 =
    R"(2024-09-01T09:00:00Z,2024-09-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1,90,-90,,,EUR,,,,,synthetic-buy-2024,,,,)";
constexpr std::string_view kSellAfter =
    R"(2024-10-01T09:00:00Z,2024-10-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-0.2,95,19,,,EUR,,,,,synthetic-sell-after,,,,)";
// A sale of 0.1 units at EUR 105 before the short sale: 0.1 x (105 - 100) = EUR 0.5 gain.
constexpr std::string_view kSellBefore =
    R"(2024-02-01T09:00:00Z,2024-02-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-0.1,105,10.5,,,EUR,,,,,synthetic-sell-before,,,,)";
// Only 0.2 units are bought, so the sale of 0.25 units in 2023 is short by 0.05 units.
constexpr std::string_view kSmallBuy2022 =
    R"(2022-03-01T09:00:00Z,2022-03-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,0.2,100,-20,,,EUR,,,,,synthetic-buy-2022,,,,)";
constexpr std::string_view kBuyMarch2024 =
    R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1,90,-90,,,EUR,,,,,synthetic-buy-2024,,,,)";
constexpr std::string_view kSellJune2024 =
    R"(2024-06-01T09:00:00Z,2024-06-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-0.5,95,47.5,,,EUR,,,,,synthetic-sell-june,,,,)";
constexpr std::string_view kBuyComplete =
    R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-complete,,,,)";
constexpr std::string_view kSellComplete =
    R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-5,30,150,,,EUR,,,,,synthetic-sell-complete,,,,)";

// A split row of 2024-06-03, after the short sale of 2024-05-01.
constexpr std::string_view kSplitAfterGap =
    R"(2024-06-03T09:00:00Z,2024-06-03,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,2,,,,,,,,,,synthetic-split-after-gap,,,,)";
// A second investment with no purchase at all: its sale of 1 unit is 1 unit short.
constexpr std::string_view kOtherShortSale =
    R"(2024-04-01T09:00:00Z,2024-04-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Other,XX0000000002,-1,50,50,,,EUR,,,,,synthetic-sell-other,,,,)";
// A coupon with a RUB original amount: the ECB has no RUB rate for 2024, so Doh-Obr fails for
// XX0000000003.
constexpr std::string_view kCouponWithoutRate =
    R"(2024-08-01T10:00:00Z,2024-08-01,DEFAULT,CASH,INTEREST_PAYMENT,BOND,Synthetic Bond A,XX0000000003,,,1.80,,,EUR,200.00,RUB,0.009000,Synthetic bond interest,synthetic-coupon-1,,,,)";

TaxpayerMetadata syntheticTaxpayer() {
    return {.mTaxNumber = "12345678",
            .mName = "Synthetic Taxpayer",
            .mAddress = "Synthetic Street 1",
            .mCity = "Synthetic City",
            .mPostCode = "1000",
            .mPostName = "Synthetic City",
            .mBirthDate = *parseCalendarDate("1990-01-31"),
            .mIsResident = true,
            .mEmail = "taxpayer@example.com",
            .mPhoneNumber = "+38600000000"};
}

/// A request for 2024 with one Trade Republic file made of the given rows.
ApplicationRequest requestWithRows(const std::vector<std::string_view>& aRows,
                                   bool aDeveloperMode) {
    std::string content{kTradeRepublicHeader};

    for (const auto row : aRows)
    {
        content += row;
        content += '\n';
    }

    ApplicationRequest request{
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
        .mDeveloperMode = aDeveloperMode,
        .mTaxpayer = syntheticTaxpayer(),
    };

    request.mBrokerFiles.push_back(
        {.mBroker = Broker::TradeRepublic,
         .mFile = {.mName = "synthetic.csv", .mContent = std::move(content)}});

    return request;
}

/// The run, or an empty optional (and a failed test) when the files were not accepted.
std::optional<ApplicationRun> startRun(ApplicationRequest aRequest) {
    auto started = ApplicationRun::start(std::move(aRequest));
    auto* run = std::get_if<ApplicationRun>(&started);

    EXPECT_NE(run, nullptr);

    if (run == nullptr)
    {
        return std::nullopt;
    }

    return std::optional<ApplicationRun>{std::move(*run)};
}

/// Answers the only open request; the test checks that there is exactly one.
std::vector<RejectedAnswer> answerOnlyRequest(ApplicationRun& aRun, AnswerValue aValue) {
    const auto& result = aRun.result();

    EXPECT_EQ(result.mRequests.size(), 1U);

    return aRun.session().answer(
        {{result.mRequests.at(0).mId, result.mRunRevision, std::move(aValue)}});
}

/// Answers the one open request of the given type; the test checks that there is exactly one.
std::vector<RejectedAnswer>
answerRequestOfType(ApplicationRun& aRun, RequestType aType, AnswerValue aValue) {
    const auto& result = aRun.result();
    const auto request =
        std::find_if(result.mRequests.begin(),
                     result.mRequests.end(),
                     [aType](const DecisionRequest& aRequest) { return aRequest.mType == aType; });

    EXPECT_NE(request, result.mRequests.end());

    if (request == result.mRequests.end())
    {
        return {};
    }

    return aRun.session().answer({{request->mId, result.mRunRevision, std::move(aValue)}});
}

/// The warnings `developer_export_unavailable` of the result, with their message keys.
std::vector<MessageKey> unavailableWarningKeys(const ApplicationResult& aResult) {
    std::vector<MessageKey> keys;

    for (const auto& diagnostic : aResult.mDiagnostics)
    {
        const auto* code = std::get_if<ProcessingDiagnosticCode>(&diagnostic.mCode);

        if (code != nullptr && *code == ProcessingDiagnosticCode::DeveloperExportUnavailable)
        {
            EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Warning);
            EXPECT_FALSE(diagnostic.mBlocking);
            EXPECT_EQ(diagnostic.mIsin, "XX0000000001");
            keys.push_back(diagnostic.mMessageKey.value());
        }
    }

    return keys;
}

const AnswerValue kDeveloperExport = FailureResolutionAnswer{FailureResolution::DeveloperExport};
const AnswerValue kConfirm = DeveloperExportConfirmAnswer{};
const AnswerValue kCancel = CancelAnswer{};

/// The texts between `<aTag>` and `</aTag>`, in document order.
std::vector<std::string> valuesOf(const std::string& aXml, const std::string& aTag) {
    std::vector<std::string> values;
    const auto open = "<" + aTag + ">";
    const auto close = "</" + aTag + ">";
    auto position = aXml.find(open);

    while (position != std::string::npos)
    {
        const auto start = position + open.size();
        const auto end = aXml.find(close, start);

        values.push_back(aXml.substr(start, end - start));
        position = aXml.find(open, end);
    }

    return values;
}

/// Runs the three confirmations on a run that shows the failure question.
void chooseAndConfirmDeveloperExport(ApplicationRun& aRun) {
    EXPECT_TRUE(answerOnlyRequest(aRun, kDeveloperExport).empty());
    EXPECT_TRUE(answerOnlyRequest(aRun, kConfirm).empty());
    EXPECT_TRUE(answerOnlyRequest(aRun, kConfirm).empty());
    EXPECT_TRUE(answerOnlyRequest(aRun, kConfirm).empty());
}

TEST(DeveloperExportIntegrationTest, WithoutDeveloperModeTheQuestionHasTwoChoices) {
    auto run =
        startRun(requestWithRows({kBuy2022, kSell2023, kShortSale, kBuy2024, kSellAfter}, false));

    ASSERT_TRUE(run.has_value());

    const auto& result = run->result();

    ASSERT_EQ(result.mRequests.size(), 1U);
    EXPECT_EQ(result.mRequests[0].mType, RequestType::CapitalGainsFailureResolutionRequired);

    const auto& question = std::get<FailureResolutionQuestion>(result.mRequests[0].mData);

    EXPECT_FALSE(question.mDeveloperExportOffered);

    // The third choice is rejected when it is not offered, and nothing changes.
    const auto revision = result.mRunRevision;
    const auto rejections = answerOnlyRequest(*run, kDeveloperExport);

    ASSERT_EQ(rejections.size(), 1U);
    EXPECT_EQ(rejections[0].mReason, AnswerRejection::AnswerDoesNotFit);
    EXPECT_EQ(run->result().mRunRevision, revision);
    EXPECT_EQ(run->result().report(ReportType::CapitalGains).mStatus, ReportStatus::NeedsInput);
    EXPECT_FALSE(run->result().report(ReportType::CapitalGains).mUnsafe);
}

TEST(DeveloperExportIntegrationTest, DeveloperModeOffersTheDeveloperExportAsAThirdChoice) {
    auto run =
        startRun(requestWithRows({kBuy2022, kSell2023, kShortSale, kBuy2024, kSellAfter}, true));

    ASSERT_TRUE(run.has_value());

    const auto& result = run->result();

    // No confirmation is asked before the choice.
    ASSERT_EQ(result.mRequests.size(), 1U);
    EXPECT_EQ(result.mRequests[0].mType, RequestType::CapitalGainsFailureResolutionRequired);
    EXPECT_TRUE(
        std::get<FailureResolutionQuestion>(result.mRequests[0].mData).mDeveloperExportOffered);
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::NeedsInput);
}

TEST(DeveloperExportIntegrationTest, ThreeConfirmationsGiveTheUnsafeExportOfAGapInTheYear) {
    auto run =
        startRun(requestWithRows({kBuy2022, kSell2023, kShortSale, kBuy2024, kSellAfter}, true));

    ASSERT_TRUE(run.has_value());

    // The choice asks step 1 only.
    EXPECT_TRUE(answerOnlyRequest(*run, kDeveloperExport).empty());
    ASSERT_EQ(run->result().mRequests.size(), 1U);
    EXPECT_EQ(run->result().mRequests[0].mType, RequestType::DeveloperExportConfirmationRequired);
    EXPECT_EQ(std::get<DeveloperExportQuestion>(run->result().mRequests[0].mData).mStep, 1);
    ASSERT_EQ(run->result().mRequests[0].mInvestments.size(), 1U);
    EXPECT_EQ(run->result().mRequests[0].mInvestments[0].mIsin, "XX0000000001");

    // Step 1 confirmed: step 2 only.
    EXPECT_TRUE(answerOnlyRequest(*run, kConfirm).empty());
    ASSERT_EQ(run->result().mRequests.size(), 1U);
    EXPECT_EQ(std::get<DeveloperExportQuestion>(run->result().mRequests[0].mData).mStep, 2);
    EXPECT_EQ(run->result().report(ReportType::CapitalGains).mStatus, ReportStatus::NeedsInput);

    // Step 2 confirmed: step 3 only.
    EXPECT_TRUE(answerOnlyRequest(*run, kConfirm).empty());
    ASSERT_EQ(run->result().mRequests.size(), 1U);
    EXPECT_EQ(std::get<DeveloperExportQuestion>(run->result().mRequests[0].mData).mStep, 3);
    EXPECT_FALSE(run->result().report(ReportType::CapitalGains).mXml.has_value());

    // Step 3 confirmed: the unsafe export.
    EXPECT_TRUE(answerOnlyRequest(*run, kConfirm).empty());

    const auto& result = run->result();
    const auto& capital = result.report(ReportType::CapitalGains);

    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(capital.mStatus, ReportStatus::Generated);
    EXPECT_TRUE(capital.mUnsafe);
    EXPECT_EQ(capital.mFailureResolution, FailureResolution::DeveloperExport);
    EXPECT_TRUE(capital.mExcludedInvestments.empty());

    ASSERT_EQ(capital.mUnsafeInvestments.size(), 1U);
    EXPECT_EQ(capital.mUnsafeInvestments[0].mIsin, "XX0000000001");
    EXPECT_EQ(capital.mUnsafeInvestments[0].mName, "Synthetic Share");
    EXPECT_EQ(capital.mUnsafeInvestments[0].mDiagnosticIds.size(), 1U);

    // The incomplete_history error stays, but it no longer blocks.
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(std::get<ProcessingDiagnosticCode>(result.mDiagnostics[0].mCode),
              ProcessingDiagnosticCode::IncompleteHistory);
    EXPECT_EQ(result.mDiagnostics[0].mSeverity, DiagnosticSeverity::Error);
    EXPECT_FALSE(result.mDiagnostics[0].mBlocking);
    EXPECT_EQ(result.mStatus, OverallStatus::CompletedWithWarnings);

    const auto& xml = capital.mXml.value();

    // Rows: purchase of 2022 with the 0.35 units still open at the short sale (0.6 - 0.25);
    // the short sale in full, 0.35 - 0.5 = -0.15; the purchase of 1; the last sale of 0.2.
    EXPECT_EQ(valuesOf(xml, "F1"), (std::vector<std::string>{"2022-03-01", "2024-09-01"}));
    EXPECT_EQ(valuesOf(xml, "F3"), (std::vector<std::string>{"0.3500", "1.0000"}));
    EXPECT_EQ(valuesOf(xml, "F4"), (std::vector<std::string>{"100.0000", "90.0000"}));
    EXPECT_EQ(valuesOf(xml, "F6"), (std::vector<std::string>{"2024-05-01", "2024-10-01"}));
    EXPECT_EQ(valuesOf(xml, "F7"), (std::vector<std::string>{"0.5000", "0.2000"}));
    EXPECT_EQ(valuesOf(xml, "F9"), (std::vector<std::string>{"120.0000", "95.0000"}));

    // F8: 0.35, then 0.35 - 0.5 = -0.15, then -0.15 + 1 = 0.85, then 0.85 - 0.2 = 0.65.
    EXPECT_EQ(valuesOf(xml, "F8"),
              (std::vector<std::string>{"0.3500", "-0.1500", "0.8500", "0.6500"}));

    // A sale of unknown purchase value has no F10.
    EXPECT_TRUE(valuesOf(xml, "F10").empty());
}

TEST(DeveloperExportIntegrationTest, ASaleBeforeTheGapKeepsItsNormalRowAndF10) {
    auto run = startRun(
        requestWithRows({kBuy2022, kSell2023, kSellBefore, kShortSale, kBuy2024, kSellAfter},
                        true));

    ASSERT_TRUE(run.has_value());
    chooseAndConfirmDeveloperExport(*run);

    const auto& capital = run->result().report(ReportType::CapitalGains);

    ASSERT_EQ(capital.mStatus, ReportStatus::Generated);

    const auto& xml = capital.mXml.value();

    // The purchase lists 0.1 units that the February sale uses and 0.25 units open at the short
    // sale (0.6 - 0.25 - 0.1).
    EXPECT_EQ(valuesOf(xml, "F3"), (std::vector<std::string>{"0.3500", "1.0000"}));
    EXPECT_EQ(valuesOf(xml, "F6"),
              (std::vector<std::string>{"2024-02-01", "2024-05-01", "2024-10-01"}));
    EXPECT_EQ(valuesOf(xml, "F7"), (std::vector<std::string>{"0.1000", "0.5000", "0.2000"}));

    // F8: 0.35, 0.35 - 0.1 = 0.25, 0.25 - 0.5 = -0.25, -0.25 + 1 = 0.75, 0.75 - 0.2 = 0.55.
    EXPECT_EQ(valuesOf(xml, "F8"),
              (std::vector<std::string>{"0.3500", "0.2500", "-0.2500", "0.7500", "0.5500"}));

    // The February sale made a gain of 0.1 x (105 - 100) = EUR 0.5, so its F10 is true. The two
    // sales from the gap on have none.
    EXPECT_EQ(valuesOf(xml, "F10"), (std::vector<std::string>{"true"}));
}

TEST(DeveloperExportIntegrationTest, AGapBeforeTheYearListsTheEarlierSaleToo) {
    auto run =
        startRun(requestWithRows({kSmallBuy2022, kSell2023, kBuyMarch2024, kSellJune2024}, true));

    ASSERT_TRUE(run.has_value());
    chooseAndConfirmDeveloperExport(*run);

    const auto& capital = run->result().report(ReportType::CapitalGains);

    ASSERT_EQ(capital.mStatus, ReportStatus::Generated);
    EXPECT_TRUE(capital.mUnsafe);

    const auto& xml = capital.mXml.value();

    // The sale of 2023 is short by 0.05 units and is listed with the later sale of the year.
    EXPECT_EQ(valuesOf(xml, "F1"), (std::vector<std::string>{"2022-03-01", "2024-03-01"}));
    EXPECT_EQ(valuesOf(xml, "F3"), (std::vector<std::string>{"0.2000", "1.0000"}));
    EXPECT_EQ(valuesOf(xml, "F6"), (std::vector<std::string>{"2023-08-01", "2024-06-01"}));
    EXPECT_EQ(valuesOf(xml, "F7"), (std::vector<std::string>{"0.2500", "0.5000"}));

    // F8: 0.2, 0.2 - 0.25 = -0.05, -0.05 + 1 = 0.95, 0.95 - 0.5 = 0.45.
    EXPECT_EQ(valuesOf(xml, "F8"),
              (std::vector<std::string>{"0.2000", "-0.0500", "0.9500", "0.4500"}));
    EXPECT_TRUE(valuesOf(xml, "F10").empty());
}

TEST(DeveloperExportIntegrationTest, CancellingStepOneAsksTheFailureQuestionAgain) {
    auto run =
        startRun(requestWithRows({kBuy2022, kSell2023, kShortSale, kBuy2024, kSellAfter}, true));

    ASSERT_TRUE(run.has_value());
    EXPECT_TRUE(answerOnlyRequest(*run, kDeveloperExport).empty());
    EXPECT_TRUE(answerOnlyRequest(*run, kCancel).empty());

    const auto& result = run->result();
    const auto& capital = result.report(ReportType::CapitalGains);

    ASSERT_EQ(result.mRequests.size(), 1U);
    EXPECT_EQ(result.mRequests[0].mType, RequestType::CapitalGainsFailureResolutionRequired);
    EXPECT_EQ(capital.mStatus, ReportStatus::NeedsInput);
    EXPECT_FALSE(capital.mXml.has_value());
    EXPECT_FALSE(capital.mUnsafe);
    EXPECT_FALSE(capital.mFailureResolution.has_value());
}

TEST(DeveloperExportIntegrationTest, CancellingStepTwoAsksTheFailureQuestionAgain) {
    auto run =
        startRun(requestWithRows({kBuy2022, kSell2023, kShortSale, kBuy2024, kSellAfter}, true));

    ASSERT_TRUE(run.has_value());
    EXPECT_TRUE(answerOnlyRequest(*run, kDeveloperExport).empty());
    EXPECT_TRUE(answerOnlyRequest(*run, kConfirm).empty());
    EXPECT_TRUE(answerOnlyRequest(*run, kCancel).empty());

    const auto& result = run->result();

    ASSERT_EQ(result.mRequests.size(), 1U);
    EXPECT_EQ(result.mRequests[0].mType, RequestType::CapitalGainsFailureResolutionRequired);
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::NeedsInput);
    EXPECT_FALSE(result.report(ReportType::CapitalGains).mXml.has_value());
    EXPECT_FALSE(result.report(ReportType::CapitalGains).mUnsafe);
}

TEST(DeveloperExportIntegrationTest, CancellingStepThreeAsksTheFailureQuestionAgain) {
    auto run =
        startRun(requestWithRows({kBuy2022, kSell2023, kShortSale, kBuy2024, kSellAfter}, true));

    ASSERT_TRUE(run.has_value());
    EXPECT_TRUE(answerOnlyRequest(*run, kDeveloperExport).empty());
    EXPECT_TRUE(answerOnlyRequest(*run, kConfirm).empty());
    EXPECT_TRUE(answerOnlyRequest(*run, kConfirm).empty());
    EXPECT_TRUE(answerOnlyRequest(*run, kCancel).empty());

    const auto& result = run->result();

    ASSERT_EQ(result.mRequests.size(), 1U);
    EXPECT_EQ(result.mRequests[0].mType, RequestType::CapitalGainsFailureResolutionRequired);
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::NeedsInput);
    EXPECT_FALSE(result.report(ReportType::CapitalGains).mXml.has_value());
    EXPECT_FALSE(result.report(ReportType::CapitalGains).mUnsafe);
}

TEST(DeveloperExportIntegrationTest, ACancelledExportCanStillBeExcludedOrFailed) {
    auto run =
        startRun(requestWithRows({kBuy2022, kSell2023, kShortSale, kBuy2024, kSellAfter}, true));

    ASSERT_TRUE(run.has_value());
    EXPECT_TRUE(answerOnlyRequest(*run, kDeveloperExport).empty());
    EXPECT_TRUE(answerOnlyRequest(*run, kCancel).empty());

    // The normal choice: the whole investment is left out and no unsafe file is made.
    EXPECT_TRUE(
        answerOnlyRequest(*run,
                          FailureResolutionAnswer{FailureResolution::ExcludeFailedInstruments})
            .empty());

    const auto& capital = run->result().report(ReportType::CapitalGains);

    // The only investment is excluded, so nothing is left to write.
    EXPECT_FALSE(capital.mUnsafe);
    EXPECT_EQ(capital.mFailureResolution, FailureResolution::ExcludeFailedInstruments);
    EXPECT_TRUE(capital.mUnsafeInvestments.empty());
    ASSERT_EQ(capital.mExcludedInvestments.size(), 1U);
    EXPECT_EQ(capital.mExcludedInvestments[0].mIsin, "XX0000000001");
}

TEST(DeveloperExportIntegrationTest, ANewChoiceAsksTheConfirmationsFromStepOneAgain) {
    auto run =
        startRun(requestWithRows({kBuy2022, kSell2023, kShortSale, kBuy2024, kSellAfter}, true));

    ASSERT_TRUE(run.has_value());

    // Step 1 is confirmed, step 2 is cancelled, then the export is chosen again.
    EXPECT_TRUE(answerOnlyRequest(*run, kDeveloperExport).empty());
    EXPECT_TRUE(answerOnlyRequest(*run, kConfirm).empty());
    EXPECT_TRUE(answerOnlyRequest(*run, kCancel).empty());
    EXPECT_TRUE(answerOnlyRequest(*run, kDeveloperExport).empty());

    // The earlier confirmation does not count: step 1 is asked again.
    ASSERT_EQ(run->result().mRequests.size(), 1U);
    EXPECT_EQ(run->result().mRequests[0].mType, RequestType::DeveloperExportConfirmationRequired);
    EXPECT_EQ(std::get<DeveloperExportQuestion>(run->result().mRequests[0].mData).mStep, 1);
}

TEST(DeveloperExportIntegrationTest, AStaleConfirmationIsRejected) {
    auto run =
        startRun(requestWithRows({kBuy2022, kSell2023, kShortSale, kBuy2024, kSellAfter}, true));

    ASSERT_TRUE(run.has_value());
    EXPECT_TRUE(answerOnlyRequest(*run, kDeveloperExport).empty());

    const auto staleRevision = run->result().mRunRevision;
    const auto stepOneId = run->result().mRequests.at(0).mId;

    EXPECT_TRUE(answerOnlyRequest(*run, kConfirm).empty());

    // The answer to step 1 made a new revision; an answer of the old one is refused.
    const auto rejections = run->session().answer({{stepOneId, staleRevision, kConfirm}});

    ASSERT_EQ(rejections.size(), 1U);
    EXPECT_EQ(rejections[0].mReason, AnswerRejection::StaleRevision);
    EXPECT_EQ(std::get<DeveloperExportQuestion>(run->result().mRequests.at(0).mData).mStep, 2);
}

TEST(DeveloperExportIntegrationTest, ACompleteHistoryAsksNoDeveloperQuestion) {
    auto run = startRun(requestWithRows({kBuyComplete, kSellComplete}, true));

    ASSERT_TRUE(run.has_value());

    const auto& capital = run->result().report(ReportType::CapitalGains);

    // 10 units at EUR 10 and a sale of 5 units at EUR 30: a normal Doh-KDVP, not unsafe.
    EXPECT_TRUE(run->result().mRequests.empty());
    EXPECT_EQ(capital.mStatus, ReportStatus::Generated);
    EXPECT_FALSE(capital.mUnsafe);
    EXPECT_TRUE(capital.mUnsafeInvestments.empty());
    EXPECT_EQ(valuesOf(capital.mXml.value(), "F10"), (std::vector<std::string>{"true"}));
}

TEST(DeveloperExportIntegrationTest, AnotherRunAsksTheFailureQuestionAgain) {
    const auto rows =
        std::vector<std::string_view>{kBuy2022, kSell2023, kShortSale, kBuy2024, kSellAfter};
    auto first = startRun(requestWithRows(rows, true));

    ASSERT_TRUE(first.has_value());
    chooseAndConfirmDeveloperExport(*first);
    ASSERT_TRUE(first->result().report(ReportType::CapitalGains).mUnsafe);

    // Nothing is kept for another run: it shows the failure question and no confirmation.
    auto second = startRun(requestWithRows(rows, true));

    ASSERT_TRUE(second.has_value());
    ASSERT_EQ(second->result().mRequests.size(), 1U);
    EXPECT_EQ(second->result().mRequests[0].mType,
              RequestType::CapitalGainsFailureResolutionRequired);
    EXPECT_FALSE(second->result().report(ReportType::CapitalGains).mUnsafe);
    EXPECT_EQ(second->result().report(ReportType::CapitalGains).mStatus, ReportStatus::NeedsInput);
}

TEST(DeveloperExportIntegrationTest, ARestartAsksTheFailureQuestionAgainWithoutTheDeveloperExport) {
    const auto rows =
        std::vector<std::string_view>{kBuy2022, kSell2023, kShortSale, kBuy2024, kSellAfter};
    auto run = startRun(requestWithRows(rows, true));

    ASSERT_TRUE(run.has_value());
    chooseAndConfirmDeveloperExport(*run);
    ASSERT_TRUE(run->result().report(ReportType::CapitalGains).mUnsafe);

    // The same files again: neither the choice nor its confirmations are kept.
    EXPECT_FALSE(run->restart(requestWithRows(rows, true)).has_value());

    const auto& result = run->result();

    ASSERT_EQ(result.mRequests.size(), 1U);
    EXPECT_EQ(result.mRequests[0].mType, RequestType::CapitalGainsFailureResolutionRequired);
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::NeedsInput);
    EXPECT_FALSE(result.report(ReportType::CapitalGains).mUnsafe);
    EXPECT_FALSE(result.report(ReportType::CapitalGains).mXml.has_value());
    EXPECT_TRUE(result.mDecisions.empty());
}

TEST(DeveloperExportIntegrationTest, ASplitAfterTheGapTakesTheDeveloperExportAwayWithAWarning) {
    // The split of 2024-06-03 follows the short sale of 2024-05-01. The pool stopped at the sale,
    // so the split is never applied and the rows would mix units before and after it.
    auto run = startRun(
        requestWithRows({kBuy2022, kSell2023, kShortSale, kSplitAfterGap, kBuy2024, kSellAfter},
                        true));

    ASSERT_TRUE(run.has_value());

    const auto& result = run->result();

    // The question keeps its two choices, and the warning tells why.
    ASSERT_EQ(result.mRequests.size(), 1U);
    EXPECT_EQ(result.mRequests[0].mType, RequestType::CapitalGainsFailureResolutionRequired);
    EXPECT_FALSE(
        std::get<FailureResolutionQuestion>(result.mRequests[0].mData).mDeveloperExportOffered);
    EXPECT_EQ(
        unavailableWarningKeys(result),
        std::vector<MessageKey>{MessageKey::DeveloperExportUnavailableCorporateActionAfterGap});

    // The third choice is refused.
    const auto rejections = answerOnlyRequest(*run, kDeveloperExport);

    ASSERT_EQ(rejections.size(), 1U);
    EXPECT_EQ(rejections[0].mReason, AnswerRejection::AnswerDoesNotFit);
    EXPECT_FALSE(run->result().report(ReportType::CapitalGains).mUnsafe);
    EXPECT_EQ(run->result().report(ReportType::CapitalGains).mStatus, ReportStatus::NeedsInput);
}

TEST(DeveloperExportIntegrationTest, WithoutDeveloperModeASplitAfterTheGapGivesNoWarning) {
    auto run = startRun(
        requestWithRows({kBuy2022, kSell2023, kShortSale, kSplitAfterGap, kBuy2024, kSellAfter},
                        false));

    ASSERT_TRUE(run.has_value());
    EXPECT_TRUE(unavailableWarningKeys(run->result()).empty());
    ASSERT_EQ(run->result().mRequests.size(), 1U);
    EXPECT_EQ(run->result().mRequests[0].mType, RequestType::CapitalGainsFailureResolutionRequired);
}

TEST(DeveloperExportIntegrationTest, TwoInvestmentsAreConfirmedAndExportedTogether) {
    auto run = startRun(
        requestWithRows({kBuy2022, kSell2023, kShortSale, kBuy2024, kSellAfter, kOtherShortSale},
                        true));

    ASSERT_TRUE(run.has_value());

    // One question lists both investments, sorted by ISIN.
    ASSERT_EQ(run->result().mRequests.size(), 1U);
    ASSERT_EQ(run->result().mRequests[0].mInvestments.size(), 2U);
    EXPECT_EQ(run->result().mRequests[0].mInvestments[0].mIsin, "XX0000000001");
    EXPECT_EQ(run->result().mRequests[0].mInvestments[1].mIsin, "XX0000000002");

    // Step 1 lists both too.
    EXPECT_TRUE(answerOnlyRequest(*run, kDeveloperExport).empty());
    ASSERT_EQ(run->result().mRequests.size(), 1U);
    EXPECT_EQ(
        std::get<DeveloperExportQuestion>(run->result().mRequests[0].mData).mInvestments.size(),
        2U);

    EXPECT_TRUE(answerOnlyRequest(*run, kConfirm).empty());
    EXPECT_TRUE(answerOnlyRequest(*run, kConfirm).empty());
    EXPECT_TRUE(answerOnlyRequest(*run, kConfirm).empty());

    const auto& capital = run->result().report(ReportType::CapitalGains);

    EXPECT_EQ(capital.mStatus, ReportStatus::Generated);
    EXPECT_TRUE(capital.mUnsafe);
    ASSERT_EQ(capital.mUnsafeInvestments.size(), 2U);
    EXPECT_EQ(capital.mUnsafeInvestments[0].mIsin, "XX0000000001");
    EXPECT_EQ(capital.mUnsafeInvestments[1].mIsin, "XX0000000002");

    // The second list is only its sale: 1 unit at EUR 50, stock 0 - 1 = -1, no F10.
    EXPECT_EQ(valuesOf(capital.mXml.value(), "F6"),
              (std::vector<std::string>{"2024-05-01", "2024-10-01", "2024-04-01"}));
    EXPECT_EQ(valuesOf(capital.mXml.value(), "F8").back(), "-1.0000");
}

TEST(DeveloperExportIntegrationTest, ADohObrErrorIsNotTouchedByTheDeveloperExport) {
    auto run = startRun(
        requestWithRows({kBuy2022, kSell2023, kShortSale, kBuy2024, kSellAfter, kCouponWithoutRate},
                        true));

    ASSERT_TRUE(run.has_value());
    ASSERT_EQ(run->result().mRequests.size(), 2U);

    EXPECT_TRUE(answerRequestOfType(*run,
                                    RequestType::CapitalGainsFailureResolutionRequired,
                                    kDeveloperExport)
                    .empty());

    // Steps 1, 2 and 3.
    EXPECT_TRUE(
        answerRequestOfType(*run, RequestType::DeveloperExportConfirmationRequired, kConfirm)
            .empty());
    EXPECT_TRUE(
        answerRequestOfType(*run, RequestType::DeveloperExportConfirmationRequired, kConfirm)
            .empty());
    EXPECT_TRUE(
        answerRequestOfType(*run, RequestType::DeveloperExportConfirmationRequired, kConfirm)
            .empty());

    const auto& result = run->result();

    // Doh-KDVP is the unsafe export; Doh-Obr still waits for its own failure answer.
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Generated);
    EXPECT_TRUE(result.report(ReportType::CapitalGains).mUnsafe);
    EXPECT_EQ(result.report(ReportType::OtherInterest).mStatus, ReportStatus::NeedsInput);
    EXPECT_FALSE(result.report(ReportType::OtherInterest).mUnsafe);
    EXPECT_FALSE(result.report(ReportType::OtherInterest).mFailureResolution.has_value());
    ASSERT_EQ(result.mRequests.size(), 1U);
    EXPECT_EQ(result.mRequests[0].mType, RequestType::OtherInterestFailureResolutionRequired);
}

} // namespace
