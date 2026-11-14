#include "../support/xsd_validation.hpp"
#include "taxbroker/application_request.hpp"
#include "taxbroker/application_run.hpp"
#include "taxbroker/input_file.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace taxbroker;
using taxbroker::test::expectValidForDhoSchema;
using taxbroker::test::expectValidForDivSchema;
using taxbroker::test::expectValidForKdvpSchemas;
using taxbroker::test::expectValidForObrSchema;

// All rows are synthetic. The header is the one of the Trade Republic export.
constexpr std::string_view kTradeRepublicHeader =
    "datetime,date,account_type,category,type,asset_class,name,symbol,shares,price,amount,fee,tax,"
    "currency,original_amount,original_currency,fx_rate,description,transaction_id,"
    "counterparty_name,counterparty_iban,payment_reference,mcc_code\n";

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

/// A committed file under `tests/test_data`, read byte for byte.
std::string readTestData(std::string_view aFolder, std::string_view aName) {
    const auto path =
        std::filesystem::path{__FILE__}.parent_path().parent_path() / "test_data" / aFolder / aName;
    std::ifstream input{path, std::ios::binary};

    if (!input.is_open())
    {
        throw std::runtime_error{"Unable to open the test file " + std::string{aName}};
    }

    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

/// A Trade Republic file made of the header and the given rows.
BrokerInputFile fileOfRows(std::string_view aName, const std::vector<std::string_view>& aRows) {
    std::string content{kTradeRepublicHeader};

    for (const auto row : aRows)
    {
        content += row;
        content += '\n';
    }

    return {.mBroker = Broker::TradeRepublic,
            .mFile = {.mName = std::string{aName}, .mContent = std::move(content)}};
}

/// A Trade Republic file read from the fixture folder `tests/test_data/pipeline`.
BrokerInputFile fixtureFile(std::string_view aName) {
    return {.mBroker = Broker::TradeRepublic,
            .mFile = {.mName = std::string{aName}, .mContent = readTestData("pipeline", aName)}};
}

/// A request for the year with the synthetic taxpayer and the given files.
ApplicationRequest requestOf(int aYear, std::vector<BrokerInputFile> aFiles) {
    return {.mReportingYear = aYear,
            .mFilingType = FilingType::Original,
            .mTaxpayer = syntheticTaxpayer(),
            .mBrokerFiles = std::move(aFiles)};
}

/// The made-up USD rates of `tests/test_data/cli`: 1.20 on 2024-02-14, 1.25 on 2024-03-14 and
/// 1.40 on 2024-08-14.
InputFile fixtureRates() {
    return {.mName = "report_ecb_rates.csv",
            .mContent = readTestData("cli", "report_ecb_rates.csv")};
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

/// Answers the request of the given ID at the current revision of the run.
std::vector<RejectedAnswer>
answerRequest(ApplicationRun& aRun, const std::string& aRequestId, AnswerValue aValue) {
    return aRun.session().answer({{aRequestId, aRun.result().mRunRevision, std::move(aValue)}});
}

// The loss question: 10 units bought at EUR 30 and sold at EUR 20 in 2025, with no purchase within
// 30 days of the sale. The loss is 10 x 20 - 10 x 30 = EUR 100.
constexpr std::string_view kLossBuy =
    R"(2025-01-10T09:00:00Z,2025-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,30,-300,,,EUR,,,,,synthetic-buy-1,,,,)";
constexpr std::string_view kLossSell =
    R"(2025-03-10T09:00:00Z,2025-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,20,200,,,EUR,,,,,synthetic-sell-1,,,,)";

TEST(FullPipelineIntegrationTest, TwoOverlappingExportsGiveFourGoldenFormsFor2024) {
    auto request =
        requestOf(2024, {fixtureFile("export_2023_2024.csv"), fixtureFile("export_2024_2025.csv")});

    request.mEcbRateFiles.push_back(fixtureRates());

    auto run = startRun(std::move(request));

    ASSERT_TRUE(run.has_value());

    // Before the answer: the one request is the deposit interest total, which never blocks a form,
    // so all four forms are generated; the payer warning of the coupons makes the status
    // completed_with_warnings.
    ASSERT_EQ(run->result().mRequests.size(), 1U);
    EXPECT_EQ(run->result().mRequests[0].mType, RequestType::DepositInterestTotalRequired);
    EXPECT_EQ(run->result().report(ReportType::BankDepositInterest).mStatus,
              ReportStatus::Generated);
    EXPECT_EQ(run->result().mStatus, OverallStatus::CompletedWithWarnings);

    const auto requestId = run->result().mRequests[0].mId;

    EXPECT_TRUE(
        answerRequest(*run, requestId, DepositInterestTotalAnswer{.mAboveLimit = true}).empty());

    const auto& result = run->result();

    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.mStatus, OverallStatus::CompletedWithWarnings);

    // Doh-KDVP: purchase 2023-03-01 (10 at EUR 10, F8 10), purchase 2023-09-01 (5 of 10 at
    // EUR 20, F8 15) and the sale 2024-02-01 (15 at EUR 30, F8 0).
    const auto& capital = result.report(ReportType::CapitalGains);

    EXPECT_EQ(capital.mStatus, ReportStatus::Generated);
    ASSERT_TRUE(capital.mXml.has_value());
    EXPECT_EQ(*capital.mXml, readTestData("pipeline", "Doh-KDVP_2024.golden.xml"));
    expectValidForKdvpSchemas(*capital.mXml);

    // Doh-Div: Apple USD 12.50 / 1.25 = EUR 10.00; Krka pays its own tax and is not a row.
    const auto& dividends = result.report(ReportType::Dividends);

    EXPECT_EQ(dividends.mStatus, ReportStatus::Generated);
    ASSERT_TRUE(dividends.mXml.has_value());
    EXPECT_EQ(*dividends.mXml, readTestData("pipeline", "Doh-Div_2024.golden.xml"));
    expectValidForDivSchema(*dividends.mXml);

    // The two excluded items follow tax rules, in ISIN order: Krka pays its own tax, and the bond
    // is a debt security that Doh-KDVP does not list.
    ASSERT_EQ(result.mExcludedItems.size(), 2U);
    EXPECT_EQ(result.mExcludedItems[0].mIsin, "SI0031102120");
    EXPECT_EQ(result.mExcludedItems[0].mReason, ExcludedReason::SlovenianTaxSettled);
    EXPECT_EQ(result.mExcludedItems[1].mIsin, "XS0000000002");
    EXPECT_EQ(result.mExcludedItems[1].mReason, ExcludedReason::DebtSecurity);

    // Two warnings: Doh-DHO holds only the imported interest (the answer was above_limit), and the
    // payer of the bond is not in the reference files.
    ASSERT_EQ(result.mDiagnostics.size(), 2U);
    EXPECT_EQ(result.mDiagnostics[0].mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports,
              (std::vector{ReportType::BankDepositInterest}));
    EXPECT_EQ(result.mDiagnostics[0].mMessageKey, MessageKey::DepositInterestAddOtherBanks);
    EXPECT_FALSE(result.mDiagnostics[0].mIsin.has_value());
    EXPECT_EQ(result.mDiagnostics[1].mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(result.mDiagnostics[1].mAffectedReports, (std::vector{ReportType::OtherInterest}));
    EXPECT_EQ(result.mDiagnostics[1].mMessageKey, MessageKey::InterestPayerNotFound);
    EXPECT_EQ(result.mDiagnostics[1].mIsin, "XS0000000002");

    // Doh-DHO: 0.33 + 0.33 + 0.34 = EUR 1.00; the two payments in both files count once.
    const auto& deposit = result.report(ReportType::BankDepositInterest);

    EXPECT_EQ(deposit.mStatus, ReportStatus::Generated);
    ASSERT_TRUE(deposit.mXml.has_value());
    EXPECT_EQ(*deposit.mXml, readTestData("pipeline", "Doh-DHO_2024.golden.xml"));
    expectValidForDhoSchema(*deposit.mXml);

    // Doh-Obr: USD 20.00 / 1.20 = EUR 16.67 and USD 20.00 / 1.40 = EUR 14.29, each coupon once.
    const auto& other = result.report(ReportType::OtherInterest);

    EXPECT_EQ(other.mStatus, ReportStatus::Generated);
    ASSERT_TRUE(other.mXml.has_value());
    EXPECT_EQ(*other.mXml, readTestData("pipeline", "Doh-Obr_2024.golden.xml"));
    expectValidForObrSchema(*other.mXml);

    // Fees: the bond purchase EUR 1.00 and the sale EUR 1.00, the sale once although both files
    // hold it. The purchase fee of 2023 is another year. The saveback is EUR 2.50.
    ASSERT_EQ(result.mFees.mBrokers.size(), 1U);
    EXPECT_EQ(result.mFees.mBrokers[0].mByCurrency.at(0).mCount, 2U);
    EXPECT_EQ(result.mFees.mEurTotal, 20'000); // EUR 2.00 at 4 decimals

    EXPECT_EQ(result.mBenefits.mEurTotal, 25'000); // EUR 2.50 at 4 decimals
}

TEST(FullPipelineIntegrationTest, TheSameExportsGiveTheGoldenDohKdvpFor2025) {
    auto run = startRun(
        requestOf(2025,
                  {fixtureFile("export_2023_2024.csv"), fixtureFile("export_2024_2025.csv")}));

    ASSERT_TRUE(run.has_value());

    const auto& result = run->result();

    // Doh-KDVP: the 5 units of the purchase of 2023-09-01 that are left (F8 5) and the sale of
    // 2025-02-03 (5 at EUR 40, F8 0).
    const auto& capital = result.report(ReportType::CapitalGains);

    EXPECT_EQ(capital.mStatus, ReportStatus::Generated);
    ASSERT_TRUE(capital.mXml.has_value());
    EXPECT_EQ(*capital.mXml, readTestData("pipeline", "Doh-KDVP_2025.golden.xml"));
    expectValidForKdvpSchemas(*capital.mXml);

    // No income row is dated 2025.
    EXPECT_EQ(result.report(ReportType::Dividends).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(ReportType::BankDepositInterest).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(ReportType::OtherInterest).mStatus, ReportStatus::NoData);

    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_EQ(result.mStatus, OverallStatus::Success);

    // The fee of 2023 and 2024 and the saveback of 2024 are not in 2025.
    EXPECT_TRUE(result.mFees.mBrokers.empty());
    EXPECT_TRUE(result.mBenefits.mBrokers.empty());
}

TEST(FullPipelineIntegrationTest, EitherFileOrderGivesTheSameResult) {
    auto request =
        requestOf(2024, {fixtureFile("export_2023_2024.csv"), fixtureFile("export_2024_2025.csv")});
    auto reversed =
        requestOf(2024, {fixtureFile("export_2024_2025.csv"), fixtureFile("export_2023_2024.csv")});

    request.mEcbRateFiles.push_back(fixtureRates());
    reversed.mEcbRateFiles.push_back(fixtureRates());

    auto first = startRun(std::move(request));
    auto second = startRun(std::move(reversed));

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());

    // The four documents are the same strings.
    const auto& left = first->result();
    const auto& right = second->result();

    EXPECT_EQ(*left.report(ReportType::CapitalGains).mXml,
              *right.report(ReportType::CapitalGains).mXml);
    EXPECT_EQ(*left.report(ReportType::Dividends).mXml, *right.report(ReportType::Dividends).mXml);
    EXPECT_EQ(*left.report(ReportType::BankDepositInterest).mXml,
              *right.report(ReportType::BankDepositInterest).mXml);
    EXPECT_EQ(*left.report(ReportType::OtherInterest).mXml,
              *right.report(ReportType::OtherInterest).mXml);

    // The two warnings are the same, in another order: the first sort key after the stage is the
    // earliest source file, so the file order decides which warning comes first.
    ASSERT_EQ(left.mDiagnostics.size(), 2U);
    ASSERT_EQ(right.mDiagnostics.size(), 2U);

    EXPECT_EQ(left.mDiagnostics[0].mMessageKey, MessageKey::DepositInterestAddOtherBanks);
    EXPECT_EQ(left.mDiagnostics[1].mMessageKey, MessageKey::InterestPayerNotFound);
    EXPECT_EQ(left.mDiagnostics[1].mIsin, "XS0000000002");

    EXPECT_EQ(right.mDiagnostics[0].mMessageKey, MessageKey::InterestPayerNotFound);
    EXPECT_EQ(right.mDiagnostics[0].mIsin, "XS0000000002");
    EXPECT_EQ(right.mDiagnostics[1].mMessageKey, MessageKey::DepositInterestAddOtherBanks);

    // The open request is the deposit interest total in both.
    ASSERT_EQ(left.mRequests.size(), 1U);
    ASSERT_EQ(right.mRequests.size(), 1U);
    EXPECT_EQ(left.mRequests[0].mType, RequestType::DepositInterestTotalRequired);
    EXPECT_EQ(right.mRequests[0].mType, RequestType::DepositInterestTotalRequired);

    EXPECT_EQ(left.mStatus, right.mStatus);
}

TEST(FullPipelineIntegrationTest, AFileWithOnlyTheHeaderGivesFourNoDataAndSuccess) {
    auto run = startRun(requestOf(2024, {fileOfRows("empty.csv", {})}));

    ASSERT_TRUE(run.has_value());

    const auto& result = run->result();

    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(ReportType::Dividends).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(ReportType::BankDepositInterest).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(ReportType::OtherInterest).mStatus, ReportStatus::NoData);

    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.mStatus, OverallStatus::Success);
}

TEST(FullPipelineIntegrationTest, ABrokenRecordFailsAllFourFormsAndTheRun) {
    // 22 fields: mcc_code is missing.
    constexpr std::string_view shortRecord =
        R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,)";

    auto run = startRun(requestOf(2024, {fileOfRows("broken.csv", {shortRecord})}));

    ASSERT_TRUE(run.has_value());

    const auto& result = run->result();

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::FieldCountMismatch);
    EXPECT_EQ(diagnostic.mAffectedReports.size(), 4U);
    EXPECT_TRUE(diagnostic.mBlocking);

    for (const auto type : {ReportType::CapitalGains,
                            ReportType::Dividends,
                            ReportType::BankDepositInterest,
                            ReportType::OtherInterest})
    {
        const auto& report = result.report(type);

        EXPECT_EQ(report.mStatus, ReportStatus::Failed);
        EXPECT_EQ(report.mDiagnosticIds, (std::vector<std::string>{diagnostic.mId}));
    }

    EXPECT_EQ(result.mStatus, OverallStatus::Failed);
}

TEST(FullPipelineIntegrationTest, AChangedOverlappingSaleFailsOnlyDohKdvp) {
    // The two files hold the transaction p-sell with another price: EUR 30 (amount EUR 450) and
    // EUR 31 (amount EUR 465).
    constexpr std::string_view buy =
        R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,15,10,-150,,,EUR,,,,,p-buy,,,,)";
    constexpr std::string_view sale =
        R"(2024-02-01T09:00:00Z,2024-02-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-15,30,450,,,EUR,,,,,p-sell,,,,)";
    constexpr std::string_view changedSale =
        R"(2024-02-01T09:00:00Z,2024-02-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-15,31,465,,,EUR,,,,,p-sell,,,,)";
    constexpr std::string_view dividend =
        R"(2024-03-14T09:00:00Z,2024-03-14,DEFAULT,CASH,DIVIDEND,STOCK,Apple Inc,US0378331005,,,10.00,,,EUR,12.50,USD,0.800000,,p-div-apple,,,,)";

    auto run = startRun(requestOf(
        2024,
        {fileOfRows("first.csv", {buy, sale, dividend}), fileOfRows("second.csv", {changedSale})}));

    ASSERT_TRUE(run.has_value());

    const auto& result = run->result();

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& conflict = result.mDiagnostics[0];

    EXPECT_EQ(std::get<MergeDiagnosticCode>(conflict.mCode),
              MergeDiagnosticCode::ConflictingDuplicate);
    EXPECT_EQ(conflict.mAffectedReports, (std::vector{ReportType::CapitalGains}));

    // The conflict lists the sale of both files, in file order.
    ASSERT_EQ(conflict.mSources.size(), 2U);
    ASSERT_TRUE(conflict.mSources[0].mFilename.has_value());
    ASSERT_TRUE(conflict.mSources[1].mFilename.has_value());
    EXPECT_EQ(conflict.mSources[0].mFilename->value(), "first.csv");
    EXPECT_EQ(conflict.mSources[0].mTransactionId, "p-sell");
    EXPECT_EQ(conflict.mSources[1].mFilename->value(), "second.csv");
    EXPECT_EQ(conflict.mSources[1].mTransactionId, "p-sell");

    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(ReportType::Dividends).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(ReportType::BankDepositInterest).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(ReportType::OtherInterest).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.mStatus, OverallStatus::PartialSuccess);
}

TEST(FullPipelineIntegrationTest, FailingDohDivLeavesDohKdvpAndIsAPartialSuccess) {
    // The share sale makes Doh-KDVP. XX0000000001 pays a dividend in RUB, which has no official
    // rate after March 2022; XX0000000002 pays EUR 10.00.
    constexpr std::string_view buy =
        R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000009,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)";
    constexpr std::string_view sale =
        R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000009,-10,30,300,,,EUR,,,,,synthetic-sell-1,,,,)";
    constexpr std::string_view rubDividend =
        R"(2024-04-01T09:00:00Z,2024-04-01,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share A,XX0000000001,,,9,,,EUR,1000,RUB,0.009000,,synthetic-dividend-1,,,,)";
    constexpr std::string_view eurDividend =
        R"(2024-04-02T09:00:00Z,2024-04-02,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share B,XX0000000002,,,10,,,EUR,,,,,synthetic-dividend-2,,,,)";

    auto run = startRun(
        requestOf(2024, {fileOfRows("synthetic.csv", {buy, sale, rubDividend, eurDividend})}));

    ASSERT_TRUE(run.has_value());
    ASSERT_EQ(run->result().mRequests.size(), 1U);
    EXPECT_EQ(run->result().mRequests[0].mType, RequestType::DividendsFailureResolutionRequired);
    EXPECT_EQ(run->result().mStatus, OverallStatus::NeedsInput);

    const auto requestId = run->result().mRequests[0].mId;

    EXPECT_TRUE(
        answerRequest(*run, requestId, FailureResolutionAnswer{FailureResolution::FailReport})
            .empty());

    const auto& result = run->result();

    EXPECT_EQ(result.report(ReportType::Dividends).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(ReportType::BankDepositInterest).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(ReportType::OtherInterest).mStatus, ReportStatus::NoData);
    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.mStatus, OverallStatus::PartialSuccess);
}

TEST(FullPipelineIntegrationTest, AStaleAndAReplayedLossAnswerAreRejectedWithoutARecalculation) {
    auto run = startRun(requestOf(2025, {fileOfRows("synthetic.csv", {kLossBuy, kLossSell})}));

    ASSERT_TRUE(run.has_value());
    ASSERT_EQ(run->result().mRequests.size(), 1U);
    EXPECT_EQ(run->result().mRequests[0].mType, RequestType::LossEligibilityConfirmationRequired);
    EXPECT_EQ(run->result().mRunRevision, 1U);

    const auto requestId = run->result().mRequests[0].mId;

    // An answer for revision 0 is stale: nothing changes.
    const auto stale = run->session().answer({{requestId, 0, NoOtherActivityAnswer{}}});

    ASSERT_EQ(stale.size(), 1U);
    EXPECT_EQ(stale[0].mReason, AnswerRejection::StaleRevision);
    EXPECT_EQ(run->result().mRunRevision, 1U);
    EXPECT_EQ(run->result().mRequests.size(), 1U);
    EXPECT_TRUE(run->result().mDecisions.empty());

    // The answer for revision 1 is taken, and the revision goes up by 1.
    EXPECT_TRUE(run->session().answer({{requestId, 1, NoOtherActivityAnswer{}}}).empty());
    EXPECT_EQ(run->result().mRunRevision, 2U);
    EXPECT_EQ(run->result().report(ReportType::CapitalGains).mStatus, ReportStatus::Generated);
    EXPECT_TRUE(run->result().mRequests.empty());

    // The same answer again, still for revision 1, is stale: the revision stays 2.
    const auto replayed = run->session().answer({{requestId, 1, NoOtherActivityAnswer{}}});

    ASSERT_EQ(replayed.size(), 1U);
    EXPECT_EQ(replayed[0].mReason, AnswerRejection::StaleRevision);
    EXPECT_EQ(run->result().mRunRevision, 2U);
    EXPECT_EQ(run->result().mDecisions.size(), 1U);
}

TEST(FullPipelineIntegrationTest, CapitalAndDohObrExclusionsAreIndependent) {
    constexpr std::string_view buy1 =
        R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)";
    constexpr std::string_view buy2 =
        R"(2024-02-10T09:00:00Z,2024-02-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,20,20,-400,,,EUR,,,,,synthetic-buy-2,,,,)";
    constexpr std::string_view sale =
        R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-15,30,450,,,EUR,,,,,synthetic-sell,,,,)";
    // A sale of 5 units of XX0000000002 that was never bought: the history is incomplete.
    constexpr std::string_view saleWithoutPurchase =
        R"(2024-04-02T09:00:00Z,2024-04-02,DEFAULT,TRADING,SELL,STOCK,Other Share,XX0000000002,-5,10,50,,,EUR,,,,,synthetic-sale-2,,,,)";
    // A coupon with a RUB original amount: the ECB has no RUB rate for 2024, so Doh-Obr fails for
    // XX0000000003.
    constexpr std::string_view failingCoupon =
        R"(2024-08-01T10:00:00Z,2024-08-01,DEFAULT,CASH,INTEREST_PAYMENT,BOND,Synthetic Bond A,XX0000000003,,,1.80,,,EUR,200.00,RUB,0.009000,Synthetic bond interest,synthetic-coupon-1,,,,)";
    constexpr std::string_view goodCoupon =
        R"(2024-03-10T10:00:00Z,2024-03-10,DEFAULT,CASH,INTEREST_PAYMENT,BOND,Synthetic Bond B,XS0000000002,,,10.00,,,EUR,,,,Interest payment,synthetic-coupon-2,,,,)";
    constexpr std::string_view interest1 =
        R"(2024-01-31T10:00:00Z,2024-01-31,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,0.33,,,EUR,,,,Synthetic bank interest,synthetic-interest-1,,,,)";
    constexpr std::string_view interest2 =
        R"(2024-02-29T10:00:00Z,2024-02-29,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,0.33,,,EUR,,,,Synthetic bank interest,synthetic-interest-2,,,,)";
    constexpr std::string_view interest3 =
        R"(2024-03-28T10:00:00Z,2024-03-28,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,0.34,,,EUR,,,,Synthetic bank interest,synthetic-interest-3,,,,)";

    auto run = startRun(requestOf(2024,
                                  {fileOfRows("synthetic.csv",
                                              {buy1,
                                               buy2,
                                               sale,
                                               saleWithoutPurchase,
                                               failingCoupon,
                                               goodCoupon,
                                               interest1,
                                               interest2,
                                               interest3})}));

    ASSERT_TRUE(run.has_value());

    // Two failure questions (capital gains and Doh-Obr) and the deposit total question.
    const auto& before = run->result();

    ASSERT_EQ(before.mRequests.size(), 3U);
    EXPECT_EQ(before.report(ReportType::CapitalGains).mStatus, ReportStatus::NeedsInput);
    EXPECT_EQ(before.report(ReportType::OtherInterest).mStatus, ReportStatus::NeedsInput);
    EXPECT_EQ(before.report(ReportType::BankDepositInterest).mStatus, ReportStatus::Generated);

    // Doh-DHO is made as for yes while its question is open: EUR 1.00.
    const auto depositBefore = *before.report(ReportType::BankDepositInterest).mXml;

    // The requests are in a fixed order: capital gains, then Doh-Obr, then the deposit total.
    EXPECT_EQ(before.mRequests[0].mType, RequestType::CapitalGainsFailureResolutionRequired);
    EXPECT_EQ(before.mRequests[1].mType, RequestType::OtherInterestFailureResolutionRequired);
    EXPECT_EQ(before.mRequests[2].mType, RequestType::DepositInterestTotalRequired);

    std::vector<DecisionAnswer> answers;

    answers.push_back({before.mRequests[0].mId,
                       before.mRunRevision,
                       FailureResolutionAnswer{FailureResolution::ExcludeFailedInstruments}});
    answers.push_back({before.mRequests[1].mId,
                       before.mRunRevision,
                       FailureResolutionAnswer{FailureResolution::ExcludeFailedInstruments}});

    // Both exclusions are answered in one call.
    EXPECT_TRUE(run->session().answer(std::move(answers)).empty());

    const auto& result = run->result();

    // Doh-KDVP lists XX0000000001 and leaves out XX0000000002 only.
    const auto& capital = result.report(ReportType::CapitalGains);

    EXPECT_EQ(capital.mStatus, ReportStatus::Generated);
    EXPECT_EQ(capital.mFailureResolution, FailureResolution::ExcludeFailedInstruments);
    ASSERT_EQ(capital.mExcludedInvestments.size(), 1U);
    EXPECT_EQ(capital.mExcludedInvestments[0].mIsin, "XX0000000002");

    // Doh-Obr keeps the coupon of Synthetic Bond B and leaves out XX0000000003 only.
    const auto& other = result.report(ReportType::OtherInterest);

    EXPECT_EQ(other.mStatus, ReportStatus::Generated);
    EXPECT_EQ(other.mFailureResolution, FailureResolution::ExcludeFailedInstruments);
    ASSERT_EQ(other.mExcludedInvestments.size(), 1U);
    EXPECT_EQ(other.mExcludedInvestments[0].mIsin, "XX0000000003");
    EXPECT_TRUE(other.mExcludedBrokers.empty());

    // Doh-DHO is the same document as before.
    EXPECT_EQ(result.report(ReportType::BankDepositInterest).mStatus, ReportStatus::Generated);
    EXPECT_EQ(*result.report(ReportType::BankDepositInterest).mXml, depositBefore);

    // Both errors stay as non-blocking errors; only the deposit question is open, and it never
    // blocks a form.
    ASSERT_EQ(result.mDiagnostics.size(), 5U);

    // Error: the sale of XX0000000002 has no purchase.
    EXPECT_EQ(result.mDiagnostics[0].mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(result.mDiagnostics[0].mMessageKey, MessageKey::IncompleteHistory);
    EXPECT_EQ(result.mDiagnostics[0].mIsin, "XX0000000002");
    EXPECT_FALSE(result.mDiagnostics[0].mBlocking);

    // Error: the RUB coupon of XX0000000003 has no official rate on its date.
    EXPECT_EQ(result.mDiagnostics[1].mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(result.mDiagnostics[1].mMessageKey, MessageKey::MissingExchangeRateNoRateOnDate);
    EXPECT_EQ(result.mDiagnostics[1].mIsin, "XX0000000003");
    EXPECT_FALSE(result.mDiagnostics[1].mBlocking);

    // Warnings: the payer of both bonds is not in the reference files, and Doh-DHO holds only the
    // imported interest while the deposit question is open.
    EXPECT_EQ(result.mDiagnostics[2].mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(result.mDiagnostics[2].mMessageKey, MessageKey::InterestPayerNotFound);
    EXPECT_EQ(result.mDiagnostics[2].mIsin, "XX0000000003");

    EXPECT_EQ(result.mDiagnostics[3].mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(result.mDiagnostics[3].mMessageKey, MessageKey::InterestPayerNotFound);
    EXPECT_EQ(result.mDiagnostics[3].mIsin, "XS0000000002");

    EXPECT_EQ(result.mDiagnostics[4].mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(result.mDiagnostics[4].mMessageKey, MessageKey::DepositInterestAddOtherBanks);

    ASSERT_EQ(result.mRequests.size(), 1U);
    EXPECT_EQ(result.mRequests[0].mType, RequestType::DepositInterestTotalRequired);
    EXPECT_EQ(result.mStatus, OverallStatus::CompletedWithWarnings);
}

TEST(FullPipelineIntegrationTest, ARestartWithTheSameInputUsesTheRecordedAnswerVisibly) {
    const auto request = requestOf(2025, {fileOfRows("synthetic.csv", {kLossBuy, kLossSell})});
    auto run = startRun(request);

    ASSERT_TRUE(run.has_value());
    ASSERT_EQ(run->result().mRequests.size(), 1U);

    const auto requestId = run->result().mRequests[0].mId;

    EXPECT_TRUE(answerRequest(*run, requestId, NoOtherActivityAnswer{}).empty());
    ASSERT_EQ(run->result().mRunRevision, 2U);

    const auto fault = run->restart(request);

    EXPECT_FALSE(fault.has_value());

    const auto& result = run->result();

    // Revision: 1 start, 2 answer, 3 restart.
    EXPECT_EQ(result.mRunRevision, 3U);
    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Generated);

    // The answer of the first run still applies to the same question, and the result says so.
    ASSERT_EQ(result.mDecisions.size(), 1U);
    EXPECT_EQ(result.mDecisions[0].mType, RequestType::LossEligibilityConfirmationRequired);
    EXPECT_TRUE(result.mDecisions[0].mFromEarlierRun);
    EXPECT_TRUE(result.mDecisions[0].mUsed);
}

TEST(FullPipelineIntegrationTest, AnAnswerGivenBeforeARestartIsStale) {
    const auto request = requestOf(2025, {fileOfRows("synthetic.csv", {kLossBuy, kLossSell})});
    auto run = startRun(request);

    ASSERT_TRUE(run.has_value());
    ASSERT_EQ(run->result().mRequests.size(), 1U);
    ASSERT_EQ(run->result().mRunRevision, 1U);

    const auto requestId = run->result().mRequests[0].mId;

    EXPECT_FALSE(run->restart(request).has_value());
    EXPECT_EQ(run->result().mRunRevision, 2U);

    // The user answers the question of revision 1 after the new input arrived.
    const auto rejections = run->session().answer({{requestId, 1, NoOtherActivityAnswer{}}});

    ASSERT_EQ(rejections.size(), 1U);
    EXPECT_EQ(rejections[0].mReason, AnswerRejection::StaleRevision);
    EXPECT_TRUE(run->result().mDecisions.empty());
    EXPECT_EQ(run->result().mRunRevision, 2U);
    ASSERT_EQ(run->result().mRequests.size(), 1U);
    EXPECT_EQ(run->result().mRequests[0].mType, RequestType::LossEligibilityConfirmationRequired);
}

TEST(FullPipelineIntegrationTest, AChangedSaleAsksTheQuestionAgain) {
    // The sale moves from 2025-03-10 to 2025-03-11: its date is part of the question, with the
    // window around it (2025-02-09 to 2025-04-10).
    constexpr std::string_view laterSell =
        R"(2025-03-11T09:00:00Z,2025-03-11,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,20,200,,,EUR,,,,,synthetic-sell-1,,,,)";

    auto run = startRun(requestOf(2025, {fileOfRows("synthetic.csv", {kLossBuy, kLossSell})}));

    ASSERT_TRUE(run.has_value());
    ASSERT_EQ(run->result().mRequests.size(), 1U);

    const auto requestId = run->result().mRequests[0].mId;

    EXPECT_TRUE(answerRequest(*run, requestId, NoOtherActivityAnswer{}).empty());
    EXPECT_TRUE(run->result().mRequests.empty());

    EXPECT_FALSE(run->restart(requestOf(2025, {fileOfRows("synthetic.csv", {kLossBuy, laterSell})}))
                     .has_value());

    const auto& result = run->result();

    // The recorded answer is for another question: it stays listed and is not used.
    ASSERT_EQ(result.mDecisions.size(), 1U);
    EXPECT_TRUE(result.mDecisions[0].mFromEarlierRun);
    EXPECT_FALSE(result.mDecisions[0].mUsed);

    ASSERT_EQ(result.mRequests.size(), 1U);
    EXPECT_EQ(result.mRequests[0].mType, RequestType::LossEligibilityConfirmationRequired);
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::NeedsInput);
    EXPECT_EQ(result.mStatus, OverallStatus::NeedsInput);
}

TEST(FullPipelineIntegrationTest, APurchaseWithin30DaysRemovesTheQuestionAndTheAnswerIsUnused) {
    // 10 units bought on 2025-02-20 at EUR 25, within 30 days before the sale of 2025-03-10: they
    // replace the 10 units sold at a loss, so the files decide and nothing is asked.
    constexpr std::string_view replacement =
        R"(2025-02-20T09:00:00Z,2025-02-20,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,25,-250,,,EUR,,,,,synthetic-buy-2,,,,)";

    auto run = startRun(requestOf(2025, {fileOfRows("synthetic.csv", {kLossBuy, kLossSell})}));

    ASSERT_TRUE(run.has_value());
    ASSERT_EQ(run->result().mRequests.size(), 1U);

    const auto requestId = run->result().mRequests[0].mId;

    EXPECT_TRUE(answerRequest(*run, requestId, NoOtherActivityAnswer{}).empty());

    EXPECT_FALSE(
        run->restart(
               requestOf(2025, {fileOfRows("synthetic.csv", {kLossBuy, replacement, kLossSell})}))
            .has_value());

    const auto& result = run->result();

    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Generated);

    // The answer is kept in the list, but no stage asked its question.
    ASSERT_EQ(result.mDecisions.size(), 1U);
    EXPECT_TRUE(result.mDecisions[0].mFromEarlierRun);
    EXPECT_FALSE(result.mDecisions[0].mUsed);
}

TEST(FullPipelineIntegrationTest, AnInvalidRateFileInARestartLeavesTheRunAsItWas) {
    const auto request = requestOf(2025, {fileOfRows("synthetic.csv", {kLossBuy, kLossSell})});
    auto run = startRun(request);

    ASSERT_TRUE(run.has_value());
    ASSERT_EQ(run->result().mRequests.size(), 1U);

    const auto requestId = run->result().mRequests[0].mId;

    // The header of the rate file is unknown.
    auto brokenRates = request;

    brokenRates.mEcbRateFiles.push_back(
        {.mName = "rates.csv", .mContent = "unknown,header\n1,2\n"});

    const auto fault = run->restart(std::move(brokenRates));

    ASSERT_TRUE(fault.has_value());
    EXPECT_TRUE(std::holds_alternative<RateFileFault>(*fault));

    // The result, its revision and its open request are those from before.
    EXPECT_EQ(run->result().mRunRevision, 1U);
    ASSERT_EQ(run->result().mRequests.size(), 1U);
    EXPECT_EQ(run->result().mRequests[0].mId, requestId);
    EXPECT_TRUE(run->request().mEcbRateFiles.empty());

    // The next answer still works at revision 1.
    EXPECT_TRUE(run->session().answer({{requestId, 1, NoOtherActivityAnswer{}}}).empty());
    EXPECT_EQ(run->result().mRunRevision, 2U);
    EXPECT_EQ(run->result().report(ReportType::CapitalGains).mStatus, ReportStatus::Generated);
}

TEST(FullPipelineIntegrationTest, ARestartWithAnotherCatalogueAsksThePayerAgain) {
    constexpr std::string_view dividend =
        R"(2024-04-01T09:00:00Z,2024-04-01,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share A,XX0000000010,,,10,,,EUR,,,,,synthetic-dividend-1,,,,)";
    constexpr std::string_view header =
        "isin,name,tax_number,address,country,instrument_kind,verified_on,isin_source,source,"
        "source_revision\n";

    // A reference ISIN needs a valid check digit.
    auto request = requestOf(2024, {fileOfRows("synthetic.csv", {dividend})});

    request.mReferenceFiles.push_back(
        {.mName = "payers.csv",
         .mContent = std::string{header} +
                     "XX0000000010,Payer X,TAX-X,1 Main Street,DE,,,issuer,manual,\n"
                     "XX0000000010,Payer Y,TAX-Y,2 Main Street,FR,,,issuer,manual,\n"});

    auto run = startRun(request);

    ASSERT_TRUE(run.has_value());
    ASSERT_EQ(run->result().mRequests.size(), 1U);
    EXPECT_EQ(run->result().mRequests[0].mType, RequestType::IncomeDetailsRequired);

    const auto requestId = run->result().mRequests[0].mId;

    // The user picks row 1, Payer X.
    EXPECT_TRUE(answerRequest(*run, requestId, PayerChoiceAnswer{.mPickedOption = 0}).empty());
    EXPECT_TRUE(run->result().mRequests.empty());
    EXPECT_EQ(run->result().report(ReportType::Dividends).mStatus, ReportStatus::Generated);

    // A third row for the ISIN: the list of the question changes.
    auto changed = request;

    changed.mReferenceFiles.clear();
    changed.mReferenceFiles.push_back(
        {.mName = "payers.csv",
         .mContent = std::string{header} +
                     "XX0000000010,Payer X,TAX-X,1 Main Street,DE,,,issuer,manual,\n"
                     "XX0000000010,Payer Y,TAX-Y,2 Main Street,FR,,,issuer,manual,\n"
                     "XX0000000010,Payer Z,TAX-Z,3 Main Street,IT,,,issuer,manual,\n"});

    EXPECT_FALSE(run->restart(std::move(changed)).has_value());

    const auto& result = run->result();

    // Revision: 1 start, 2 answer, 3 restart.
    EXPECT_EQ(result.mRunRevision, 3U);
    ASSERT_EQ(result.mRequests.size(), 1U);
    EXPECT_EQ(result.mRequests[0].mType, RequestType::IncomeDetailsRequired);

    const auto& question = std::get<PayerChoiceQuestion>(result.mRequests[0].mData);

    EXPECT_EQ(question.mOptions.size(), 3U);

    // The earlier answer was for the list of two rows: it is kept and not used.
    ASSERT_EQ(result.mDecisions.size(), 1U);
    EXPECT_TRUE(result.mDecisions[0].mFromEarlierRun);
    EXPECT_FALSE(result.mDecisions[0].mUsed);
}

} // namespace
