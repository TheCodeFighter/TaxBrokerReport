#include "parsers/traderepublic_parser.hpp"
#include "processors/fifo_matcher.hpp"
#include "taxbroker/api/application_result_json.hpp"
#include "taxbroker/application_result.hpp"
#include "taxbroker/decision_session.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {
using namespace taxbroker;

class TemporaryApplicationInputs {
  public:
    TemporaryApplicationInputs() {
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
        mDirectory = std::filesystem::temp_directory_path() /
                     ("taxbroker_application_synthetic_" + std::to_string(token));

        if (!std::filesystem::create_directory(mDirectory))
        {
            throw std::runtime_error{"Unable to create synthetic application directory"};
        }
    }

    TemporaryApplicationInputs(const TemporaryApplicationInputs&) = delete;
    TemporaryApplicationInputs& operator=(const TemporaryApplicationInputs&) = delete;

    ~TemporaryApplicationInputs() {
        std::error_code error;
        std::filesystem::remove_all(mDirectory, error);
    }

    /// Writes exactly the synthetic rows supplied by each scenario.
    std::filesystem::path write(std::string_view aName,
                                std::initializer_list<std::string_view> aRows) const {
        const auto path = mDirectory / aName;
        std::ofstream output{path};

        output << "datetime,date,account_type,category,type,asset_class,name,symbol,shares,price,"
                  "amount,fee,tax,currency,original_amount,original_currency,fx_rate,description,"
                  "transaction_id,counterparty_name,counterparty_iban,payment_reference,mcc_code\n";

        for (const auto row : aRows)
        {
            output << row << '\n';
        }

        output.close();

        if (!output)
        {
            throw std::runtime_error{"Unable to write synthetic application CSV"};
        }

        return path;
    }

    /// Writes the text as the whole file, header included, for a scenario with a broken header.
    std::filesystem::path writeText(std::string_view aName, std::string_view aText) const {
        const auto path = mDirectory / aName;
        std::ofstream output{path};

        output << aText;
        output.close();

        if (!output)
        {
            throw std::runtime_error{"Unable to write synthetic application CSV"};
        }

        return path;
    }

  private:
    std::filesystem::path mDirectory;
};

TEST(ApplicationResultIntegrationTest, ARejectedTradeRowFailsOnlyCapitalGains) {
    // Row 2: the price has text in it, so the row is rejected. Row 3 is a healthy purchase and
    // row 4 a healthy dividend.
    constexpr std::string_view rejectedTrade =
        R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000002,1,12x4567,-10,,,EUR,,,,,synthetic-rejected,,,,)";
    constexpr std::string_view purchase =
        R"(2024-03-02T09:00:00Z,2024-03-02,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-purchase,,,,)";
    constexpr std::string_view dividend =
        R"(2024-06-01T09:00:00Z,2024-06-01,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share,XX0000000001,,,5,,0,EUR,,,,,synthetic-dividend,,,,)";

    const TemporaryApplicationInputs files;
    const auto path = files.write("synthetic.csv", {rejectedTrade, purchase, dividend});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(path)}};
    const auto merged = DeterministicStatementMerger{}.merge(std::move(inputs));

    ApplicationResultInput input{.mReportingYear = 2024, .mMergeDiagnostics = merged.mDiagnostics};
    input.mReports[1].mXml = "<dividends/>";

    const auto result = ApplicationResultBuilder::build(std::move(input));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mId, "diagnostic-0001");
    EXPECT_EQ(std::get<DiagnosticCode>(diagnostic.mCode), DiagnosticCode::InvalidValue);
    EXPECT_EQ(diagnostic.mField, "price");
    EXPECT_EQ(diagnostic.mAffectedReports, (std::vector{ReportType::CapitalGains}));
    EXPECT_TRUE(diagnostic.mBlocking);
    ASSERT_EQ(diagnostic.mSources.size(), 1U);

    EXPECT_EQ(diagnostic.mSources[0].mSourceIndex, 0U);
    EXPECT_EQ(diagnostic.mSources[0].mRow, 2U);
    EXPECT_EQ(diagnostic.mSources[0].mFilename->value(), "synthetic.csv");

    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(ReportType::Dividends).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(ReportType::Dividends).mXml, "<dividends/>");
    EXPECT_EQ(result.report(ReportType::BankDepositInterest).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(ReportType::OtherInterest).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.mStatus, OverallStatus::PartialSuccess);

    // The JSON shows where the problem is, never the bad text or the path of the file.
    const auto text = api::serializeApplicationResultJson(result);

    EXPECT_NE(text.find(R"("row":2)"), std::string::npos);
    EXPECT_EQ(text.find("12x4567"), std::string::npos);
    EXPECT_EQ(text.find(std::filesystem::temp_directory_path().string()), std::string::npos);
}

TEST(ApplicationResultIntegrationTest, TheSameFilesInEitherInputOrderGiveTheSameJson) {
    // Each file has one rejected row: a trade with text in its price, and a dividend with text in
    // its amount. The two files keep their request index; only the merge input order changes.
    constexpr std::string_view rejectedTrade =
        R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1,12x4567,-10,,,EUR,,,,,synthetic-trade,,,,)";
    constexpr std::string_view rejectedDividend =
        R"(2024-06-01T09:00:00Z,2024-06-01,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share,XX0000000001,,,5x,,0,EUR,,,,,synthetic-dividend,,,,)";

    const TemporaryApplicationInputs files;
    const auto firstPath = files.write("first.csv", {rejectedTrade});
    const auto secondPath = files.write("second.csv", {rejectedDividend});
    tr::TradeRepublicParser parser;
    const auto first = parser.parse(firstPath, 0);
    const auto second = parser.parse(secondPath, 1);
    std::vector<StatementMergeInput> forwardInputs{{0, first}, {1, second}};
    std::vector<StatementMergeInput> backwardInputs{{1, second}, {0, first}};

    const auto forward =
        api::serializeApplicationResultJson(ApplicationResultBuilder::build(ApplicationResultInput{
            .mReportingYear = 2024,
            .mMergeDiagnostics =
                DeterministicStatementMerger{}.merge(std::move(forwardInputs)).mDiagnostics}));
    const auto backward =
        api::serializeApplicationResultJson(ApplicationResultBuilder::build(ApplicationResultInput{
            .mReportingYear = 2024,
            .mMergeDiagnostics =
                DeterministicStatementMerger{}.merge(std::move(backwardInputs)).mDiagnostics}));

    EXPECT_EQ(forward, backward);

    const auto firstFile = forward.find(R"("file":"first.csv")");
    const auto secondFile = forward.find(R"("file":"second.csv")");

    ASSERT_NE(firstFile, std::string::npos);
    ASSERT_NE(secondFile, std::string::npos);

    // The trade of the first file is diagnostic-0001, the dividend of the second file is 0002.
    EXPECT_LT(firstFile, secondFile);
    EXPECT_NE(forward.find(R"("id":"diagnostic-0002")"), std::string::npos);
}

TEST(ApplicationResultIntegrationTest, ARejectedTradeRepublicInterestRowIsDepositInterestIn2024) {
    // The amount of a broker interest row is not a number.
    constexpr std::string_view interest =
        R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,abc,,,EUR,,,,,synthetic-interest,,,,)";

    const TemporaryApplicationInputs files;
    const auto path = files.write("synthetic.csv", {interest});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(path)}};
    const auto merged = DeterministicStatementMerger{}.merge(std::move(inputs));

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = merged.mDiagnostics});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports,
              (std::vector{ReportType::BankDepositInterest}));
    EXPECT_EQ(result.report(ReportType::BankDepositInterest).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(ReportType::OtherInterest).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::NoData);
}

TEST(ApplicationResultIntegrationTest, ARejectedTradeRepublicInterestRowIsAlsoOtherInterestIn2023) {
    constexpr std::string_view interest =
        R"(2023-03-01T09:00:00Z,2023-03-01,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,abc,,,EUR,,,,,synthetic-interest,,,,)";

    const TemporaryApplicationInputs files;
    const auto path = files.write("synthetic.csv", {interest});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(path)}};
    const auto merged = DeterministicStatementMerger{}.merge(std::move(inputs));

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2023, .mMergeDiagnostics = merged.mDiagnostics});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports,
              (std::vector{ReportType::BankDepositInterest, ReportType::OtherInterest}));
    EXPECT_EQ(result.report(ReportType::BankDepositInterest).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(ReportType::OtherInterest).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(ReportType::Dividends).mStatus, ReportStatus::NoData);
}

TEST(ApplicationResultIntegrationTest, ARowOfUnknownTypeFailsAllFourReports) {
    constexpr std::string_view unknown =
        R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,CASH,SYNTHETIC_UNKNOWN,,,,,,5,,,EUR,,,,,synthetic-unknown,,,,)";

    const TemporaryApplicationInputs files;
    const auto path = files.write("synthetic.csv", {unknown});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(path)}};
    const auto merged = DeterministicStatementMerger{}.merge(std::move(inputs));

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = merged.mDiagnostics});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(std::get<DiagnosticCode>(result.mDiagnostics[0].mCode),
              DiagnosticCode::UnknownRowType);
    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports.size(), 4U);
    EXPECT_EQ(result.mStatus, OverallStatus::Failed);
}

TEST(ApplicationResultIntegrationTest, AnIncompleteHistoryIsScopedToCapitalGainsFromTheMatcher) {
    // A sale of 5 units after a purchase of 2 units: the FIFO pool lacks 3 units.
    constexpr std::string_view purchase =
        R"(2024-03-02T09:00:00Z,2024-03-02,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-purchase,,,,)";
    constexpr std::string_view sale =
        R"(2024-04-02T09:00:00Z,2024-04-02,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-5,10,50,,,EUR,,,,,synthetic-sale,,,,)";

    const TemporaryApplicationInputs files;
    const auto path = files.write("synthetic.csv", {purchase, sale});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(path)}};
    const auto prepared = TaxProcessor::prepareReportingYear(
        {2024, DeterministicStatementMerger{}.merge(std::move(inputs))});

    ASSERT_TRUE(prepared.mLedger.has_value());

    const auto fifo = FifoMatcher::match(prepared.mMerged.mStatement, *prepared.mLedger);

    ApplicationResultInput input{.mReportingYear = 2024,
                                 .mMergeDiagnostics = prepared.mMerged.mDiagnostics,
                                 .mProcessingDiagnostics = fifo.mDiagnostics};

    const auto result = ApplicationResultBuilder::build(std::move(input));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(std::get<ProcessingDiagnosticCode>(diagnostic.mCode),
              ProcessingDiagnosticCode::IncompleteHistory);
    EXPECT_EQ(diagnostic.mAffectedReports, (std::vector{ReportType::CapitalGains}));
    EXPECT_EQ(diagnostic.mIsin, "XX0000000001");
    ASSERT_EQ(diagnostic.mSources.size(), 1U);

    EXPECT_EQ(diagnostic.mSources[0].mRow, 3U);

    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.mStatus, OverallStatus::PartialSuccess);

    // The JSON names the stage, the date of the sale and the instrument.
    const auto text = api::serializeApplicationResultJson(result);

    EXPECT_NE(text.find(R"("stage":"historical_validation")"), std::string::npos);
    EXPECT_NE(text.find(R"("code":"incomplete_history")"), std::string::npos);
    EXPECT_NE(text.find(R"("taxDate":"2024-04-02")"), std::string::npos);
    EXPECT_NE(text.find(R"("instrument":{"name":"Synthetic Share","isin":"XX0000000001"})"),
              std::string::npos);
}

TEST(ApplicationResultIntegrationTest, AConflictingTradeInTwoFilesFailsOnlyCapitalGains) {
    // One transaction ID with two different prices: 10 in the first file, 11 in the second.
    constexpr std::string_view firstCopy =
        R"(2024-03-02T09:00:00Z,2024-03-02,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-trade,,,,)";
    constexpr std::string_view secondCopy =
        R"(2024-03-02T09:00:00Z,2024-03-02,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,11,-22,,,EUR,,,,,synthetic-trade,,,,)";

    const TemporaryApplicationInputs files;
    const auto firstPath = files.write("first.csv", {firstCopy});
    const auto secondPath = files.write("second.csv", {secondCopy});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(firstPath, 0)},
                                            {1, parser.parse(secondPath, 1)}};
    const auto merged = DeterministicStatementMerger{}.merge(std::move(inputs));

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = merged.mDiagnostics});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(std::get<MergeDiagnosticCode>(diagnostic.mCode),
              MergeDiagnosticCode::ConflictingDuplicate);
    EXPECT_EQ(diagnostic.mStage, PipelineStage::Merging);
    EXPECT_EQ(diagnostic.mAffectedReports, (std::vector{ReportType::CapitalGains}));
    EXPECT_TRUE(diagnostic.mBlocking);
    ASSERT_EQ(diagnostic.mSources.size(), 2U);

    EXPECT_EQ(diagnostic.mSources[0].mSourceIndex, 0U);
    EXPECT_EQ(diagnostic.mSources[0].mFilename->value(), "first.csv");
    EXPECT_EQ(diagnostic.mSources[0].mRow, 2U);
    EXPECT_EQ(diagnostic.mSources[0].mTransactionId, "synthetic-trade");
    EXPECT_EQ(diagnostic.mSources[1].mSourceIndex, 1U);
    EXPECT_EQ(diagnostic.mSources[1].mFilename->value(), "second.csv");
    EXPECT_EQ(diagnostic.mSources[1].mRow, 2U);
    EXPECT_EQ(diagnostic.mSources[1].mTransactionId, "synthetic-trade");

    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(ReportType::Dividends).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(ReportType::BankDepositInterest).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(ReportType::OtherInterest).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.mStatus, OverallStatus::PartialSuccess);

    // The JSON gives both files their own source ID and never the differing price.
    const auto text = api::serializeApplicationResultJson(result);

    EXPECT_NE(text.find(R"("stage":"merging","code":"conflicting_duplicate")"), std::string::npos);
    EXPECT_NE(text.find(R"("sourceId":"source-0001","file":"first.csv")"), std::string::npos);
    EXPECT_NE(text.find(R"("sourceId":"source-0002","file":"second.csv")"), std::string::npos);
    EXPECT_EQ(text.find("-22"), std::string::npos);
}

TEST(ApplicationResultIntegrationTest, AConflictingTradeRepublicInterestFailsBothInterestForms) {
    // One broker interest ID with two amounts: 5 in the first file, 6 in the second. The merger
    // does not say whether interest is deposit interest or a coupon, so both interest forms are
    // affected.
    constexpr std::string_view firstCopy =
        R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,5,,,EUR,,,,,synthetic-interest,,,,)";
    constexpr std::string_view secondCopy =
        R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,6,,,EUR,,,,,synthetic-interest,,,,)";

    const TemporaryApplicationInputs files;
    const auto firstPath = files.write("first.csv", {firstCopy});
    const auto secondPath = files.write("second.csv", {secondCopy});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(firstPath, 0)},
                                            {1, parser.parse(secondPath, 1)}};
    const auto merged = DeterministicStatementMerger{}.merge(std::move(inputs));

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = merged.mDiagnostics});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(std::get<MergeDiagnosticCode>(diagnostic.mCode),
              MergeDiagnosticCode::ConflictingDuplicate);
    EXPECT_EQ(diagnostic.mAffectedReports,
              (std::vector{ReportType::BankDepositInterest, ReportType::OtherInterest}));
    EXPECT_TRUE(diagnostic.mBlocking);
    EXPECT_EQ(diagnostic.mSources.size(), 2U);

    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(ReportType::Dividends).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(ReportType::BankDepositInterest).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(ReportType::OtherInterest).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.mStatus, OverallStatus::PartialSuccess);
}

TEST(ApplicationResultIntegrationTest, AConflictBetweenATradeAndADividendFailsBothForms) {
    // One transaction ID used by a purchase in the first file and a dividend in the second.
    constexpr std::string_view purchase =
        R"(2024-03-02T09:00:00Z,2024-03-02,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-shared-id,,,,)";
    constexpr std::string_view dividend =
        R"(2024-06-01T09:00:00Z,2024-06-01,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share,XX0000000001,,,5,,0,EUR,,,,,synthetic-shared-id,,,,)";

    const TemporaryApplicationInputs files;
    const auto firstPath = files.write("first.csv", {purchase});
    const auto secondPath = files.write("second.csv", {dividend});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(firstPath, 0)},
                                            {1, parser.parse(secondPath, 1)}};
    const auto merged = DeterministicStatementMerger{}.merge(std::move(inputs));

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = merged.mDiagnostics});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(std::get<MergeDiagnosticCode>(diagnostic.mCode),
              MergeDiagnosticCode::ConflictingDuplicate);
    // Both kinds, in the fixed report order.
    EXPECT_EQ(diagnostic.mAffectedReports,
              (std::vector{ReportType::CapitalGains, ReportType::Dividends}));
    EXPECT_TRUE(diagnostic.mBlocking);

    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(ReportType::Dividends).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(ReportType::BankDepositInterest).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(ReportType::OtherInterest).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.mStatus, OverallStatus::PartialSuccess);
}

TEST(ApplicationResultIntegrationTest, TwoIdenticalRowsInOneFileWarnWithoutBlocking) {
    // Rows 2 and 3 are the same purchase; the merger counts it once and warns.
    constexpr std::string_view purchase =
        R"(2024-03-02T09:00:00Z,2024-03-02,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-purchase,,,,)";

    const TemporaryApplicationInputs files;
    const auto path = files.write("synthetic.csv", {purchase, purchase});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(path)}};
    const auto merged = DeterministicStatementMerger{}.merge(std::move(inputs));

    ApplicationResultInput input{.mReportingYear = 2024, .mMergeDiagnostics = merged.mDiagnostics};
    input.mReports[1].mXml = "<dividends/>";

    const auto result = ApplicationResultBuilder::build(std::move(input));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(std::get<MergeDiagnosticCode>(diagnostic.mCode),
              MergeDiagnosticCode::DuplicateTransactionInFile);
    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(diagnostic.mAffectedReports, (std::vector{ReportType::CapitalGains}));
    EXPECT_FALSE(diagnostic.mBlocking);
    ASSERT_EQ(diagnostic.mSources.size(), 2U);

    EXPECT_EQ(diagnostic.mSources[0].mRow, 2U);
    EXPECT_EQ(diagnostic.mSources[1].mRow, 3U);

    // The warning is listed under capital gains, but nothing fails.
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(ReportType::CapitalGains).mDiagnosticIds,
              (std::vector<std::string>{"diagnostic-0001"}));
    EXPECT_EQ(result.report(ReportType::Dividends).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.mStatus, OverallStatus::CompletedWithWarnings);
}

TEST(ApplicationResultIntegrationTest, ANameConflictOfADividendInstrumentWarnsUnderDividends) {
    // One ISIN with two names on two dividends.
    constexpr std::string_view firstDividend =
        R"(2024-06-01T09:00:00Z,2024-06-01,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share,XX0000000001,,,5,,0,EUR,,,,,synthetic-dividend-1,,,,)";
    constexpr std::string_view secondDividend =
        R"(2024-09-01T09:00:00Z,2024-09-01,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share Renamed,XX0000000001,,,5,,0,EUR,,,,,synthetic-dividend-2,,,,)";

    const TemporaryApplicationInputs files;
    const auto path = files.write("synthetic.csv", {firstDividend, secondDividend});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(path)}};
    const auto merged = DeterministicStatementMerger{}.merge(std::move(inputs));

    ApplicationResultInput input{.mReportingYear = 2024, .mMergeDiagnostics = merged.mDiagnostics};
    input.mReports[1].mXml = "<dividends/>";

    const auto result = ApplicationResultBuilder::build(std::move(input));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(std::get<MergeDiagnosticCode>(diagnostic.mCode),
              MergeDiagnosticCode::InstrumentNameConflict);
    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(diagnostic.mAffectedReports, (std::vector{ReportType::Dividends}));
    EXPECT_FALSE(diagnostic.mBlocking);
    EXPECT_EQ(diagnostic.mIsin, "XX0000000001");
    EXPECT_EQ(diagnostic.mSources.size(), 2U);

    EXPECT_EQ(result.report(ReportType::Dividends).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.mStatus, OverallStatus::CompletedWithWarnings);

    const auto text = api::serializeApplicationResultJson(result);

    EXPECT_NE(text.find(R"("code":"instrument_name_conflict")"), std::string::npos);
    EXPECT_NE(text.find(R"("affectedReports":["dividends"],"blocking":false)"), std::string::npos);
    EXPECT_NE(text.find(R"("isin":"XX0000000001")"), std::string::npos);
}

TEST(ApplicationResultIntegrationTest, AnAssetClassConflictFailsCapitalGains) {
    // One ISIN bought as a stock in the first file and as a fund in the second. In two files only
    // the merger sees the conflict; in one file the parser would also report it.
    constexpr std::string_view stockPurchase =
        R"(2024-03-02T09:00:00Z,2024-03-02,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-purchase-1,,,,)";
    constexpr std::string_view fundPurchase =
        R"(2024-04-02T09:00:00Z,2024-04-02,DEFAULT,TRADING,BUY,FUND,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-purchase-2,,,,)";

    const TemporaryApplicationInputs files;
    const auto firstPath = files.write("first.csv", {stockPurchase});
    const auto secondPath = files.write("second.csv", {fundPurchase});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(firstPath, 0)},
                                            {1, parser.parse(secondPath, 1)}};
    const auto merged = DeterministicStatementMerger{}.merge(std::move(inputs));

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = merged.mDiagnostics});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(std::get<MergeDiagnosticCode>(diagnostic.mCode),
              MergeDiagnosticCode::InstrumentAssetClassConflict);
    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mAffectedReports, (std::vector{ReportType::CapitalGains}));
    EXPECT_TRUE(diagnostic.mBlocking);
    EXPECT_EQ(diagnostic.mIsin, "XX0000000001");

    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(ReportType::Dividends).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.mStatus, OverallStatus::PartialSuccess);
}

TEST(ApplicationResultIntegrationTest, TwoFilesWithOneSourceIndexFailAllFourReports) {
    // Both files are handed to the merger with request index 0.
    constexpr std::string_view firstPurchase =
        R"(2024-03-02T09:00:00Z,2024-03-02,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-purchase-1,,,,)";
    constexpr std::string_view secondPurchase =
        R"(2024-04-02T09:00:00Z,2024-04-02,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-purchase-2,,,,)";

    const TemporaryApplicationInputs files;
    const auto firstPath = files.write("first.csv", {firstPurchase});
    const auto secondPath = files.write("second.csv", {secondPurchase});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(firstPath, 0)},
                                            {0, parser.parse(secondPath, 0)}};
    const auto merged = DeterministicStatementMerger{}.merge(std::move(inputs));

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = merged.mDiagnostics});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(std::get<MergeDiagnosticCode>(diagnostic.mCode),
              MergeDiagnosticCode::DuplicateSourceIndex);
    EXPECT_EQ(diagnostic.mAffectedReports,
              (std::vector{ReportType::CapitalGains,
                           ReportType::Dividends,
                           ReportType::BankDepositInterest,
                           ReportType::OtherInterest}));
    EXPECT_TRUE(diagnostic.mBlocking);

    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(ReportType::Dividends).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(ReportType::BankDepositInterest).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(ReportType::OtherInterest).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.mStatus, OverallStatus::Failed);
}

TEST(ApplicationResultIntegrationTest, AFileParsedUnderAnotherIndexFailsAllFourReports) {
    // The file is parsed as request index 1 but handed to the merger as index 0.
    constexpr std::string_view purchase =
        R"(2024-03-02T09:00:00Z,2024-03-02,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-purchase,,,,)";

    const TemporaryApplicationInputs files;
    const auto path = files.write("synthetic.csv", {purchase});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(path, 1)}};
    const auto merged = DeterministicStatementMerger{}.merge(std::move(inputs));

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = merged.mDiagnostics});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(std::get<MergeDiagnosticCode>(diagnostic.mCode),
              MergeDiagnosticCode::InconsistentSourceIndex);
    EXPECT_EQ(diagnostic.mAffectedReports.size(), 4U);
    EXPECT_TRUE(diagnostic.mBlocking);
    EXPECT_EQ(result.mStatus, OverallStatus::Failed);
}

TEST(ApplicationResultIntegrationTest, ARejectedBondCouponFailsOnlyOtherInterest) {
    // A bond coupon whose amount is not a number.
    constexpr std::string_view coupon =
        R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,CASH,INTEREST_PAYMENT,BOND,Synthetic Bond,XX0000000003,,,abc,,,EUR,,,,,synthetic-coupon,,,,)";

    const TemporaryApplicationInputs files;
    const auto path = files.write("synthetic.csv", {coupon});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(path)}};
    const auto merged = DeterministicStatementMerger{}.merge(std::move(inputs));

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = merged.mDiagnostics});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(std::get<DiagnosticCode>(result.mDiagnostics[0].mCode), DiagnosticCode::InvalidValue);
    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports, (std::vector{ReportType::OtherInterest}));
    EXPECT_TRUE(result.mDiagnostics[0].mBlocking);

    EXPECT_EQ(result.report(ReportType::BankDepositInterest).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(ReportType::OtherInterest).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.mStatus, OverallStatus::PartialSuccess);
}

TEST(ApplicationResultIntegrationTest, AnInterestRowOfUnknownMappingFailsBothInterestForms) {
    // An interest row with a name but no asset class is neither broker interest nor a coupon.
    constexpr std::string_view interest =
        R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,CASH,INTEREST_PAYMENT,,Synthetic Fund,,,,5,,,EUR,,,,,synthetic-interest,,,,)";

    const TemporaryApplicationInputs files;
    const auto path = files.write("synthetic.csv", {interest});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(path)}};
    const auto merged = DeterministicStatementMerger{}.merge(std::move(inputs));

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = merged.mDiagnostics});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(std::get<DiagnosticCode>(result.mDiagnostics[0].mCode),
              DiagnosticCode::UnsupportedAssetClass);
    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports,
              (std::vector{ReportType::BankDepositInterest, ReportType::OtherInterest}));

    EXPECT_EQ(result.report(ReportType::BankDepositInterest).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(ReportType::OtherInterest).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::NoData);
}

TEST(ApplicationResultIntegrationTest, AFixedIncomeRowIsVisibleButBlocksNothing) {
    // FIXED_INCOME is a known row type that no form of this application uses.
    constexpr std::string_view fixedIncome =
        R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,CASH,FIXED_INCOME,,,,,,5,,,EUR,,,,,synthetic-fixed-income,,,,)";

    const TemporaryApplicationInputs files;
    const auto path = files.write("synthetic.csv", {fixedIncome});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(path)}};
    const auto merged = DeterministicStatementMerger{}.merge(std::move(inputs));

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = merged.mDiagnostics});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(std::get<DiagnosticCode>(result.mDiagnostics[0].mCode),
              DiagnosticCode::UnsupportedRowType);
    EXPECT_EQ(result.mDiagnostics[0].mSeverity, DiagnosticSeverity::Error);
    EXPECT_TRUE(result.mDiagnostics[0].mAffectedReports.empty());
    EXPECT_FALSE(result.mDiagnostics[0].mBlocking);

    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(ReportType::Dividends).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(ReportType::BankDepositInterest).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(ReportType::OtherInterest).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.mStatus, OverallStatus::CompletedWithWarnings);

    const auto text = api::serializeApplicationResultJson(result);

    EXPECT_NE(text.find(R"("affectedReports":[],"blocking":false)"), std::string::npos);
}

TEST(ApplicationResultIntegrationTest, ARejectedSplitFailsOnlyCapitalGains) {
    // The split's share count is not a number.
    constexpr std::string_view split =
        R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,abc,,,,,,,,,,synthetic-split,,,,)";

    const TemporaryApplicationInputs files;
    const auto path = files.write("synthetic.csv", {split});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(path)}};
    const auto merged = DeterministicStatementMerger{}.merge(std::move(inputs));

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = merged.mDiagnostics});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports, (std::vector{ReportType::CapitalGains}));
    EXPECT_TRUE(result.mDiagnostics[0].mBlocking);
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(ReportType::Dividends).mStatus, ReportStatus::NoData);
}

TEST(ApplicationResultIntegrationTest, ARejectedPrivateMarketBuyBlocksNoReport) {
    // The amount of a private-market purchase is not a number. Private-market events are skipped,
    // so the rejected row changes no form.
    constexpr std::string_view privateMarket =
        R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,CASH,PRIVATE_MARKET_BUY,PRIVATE_FUND,Synthetic Private Fund,XX0000000004,,,abc,,,EUR,,,,,synthetic-private-market,,,,)";

    const TemporaryApplicationInputs files;
    const auto path = files.write("synthetic.csv", {privateMarket});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(path)}};
    const auto merged = DeterministicStatementMerger{}.merge(std::move(inputs));

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = merged.mDiagnostics});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mSeverity, DiagnosticSeverity::Error);
    EXPECT_TRUE(result.mDiagnostics[0].mAffectedReports.empty());
    EXPECT_FALSE(result.mDiagnostics[0].mBlocking);
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(ReportType::OtherInterest).mStatus, ReportStatus::NoData);
}

TEST(ApplicationResultIntegrationTest, ARejectedSavebackBlocksNoReport) {
    // The amount of a saveback benefit is not a number; benefits have no form of their own.
    constexpr std::string_view saveback =
        R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,CASH,BENEFITS_SAVEBACK,,,,,,abc,,,EUR,,,,,synthetic-saveback,,,,)";

    const TemporaryApplicationInputs files;
    const auto path = files.write("synthetic.csv", {saveback});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(path)}};
    const auto merged = DeterministicStatementMerger{}.merge(std::move(inputs));

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = merged.mDiagnostics});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mSeverity, DiagnosticSeverity::Error);
    EXPECT_TRUE(result.mDiagnostics[0].mAffectedReports.empty());
    EXPECT_FALSE(result.mDiagnostics[0].mBlocking);
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.mStatus, OverallStatus::CompletedWithWarnings);
}

TEST(ApplicationResultIntegrationTest, ARecordWithTheWrongFieldCountFailsAllFourReports) {
    // 22 fields: mcc_code is missing.
    constexpr std::string_view shortRecord =
        R"(2024-03-02T09:00:00Z,2024-03-02,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-purchase,,,)";

    const TemporaryApplicationInputs files;
    const auto path = files.write("synthetic.csv", {shortRecord});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(path)}};
    const auto merged = DeterministicStatementMerger{}.merge(std::move(inputs));

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = merged.mDiagnostics});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(std::get<DiagnosticCode>(diagnostic.mCode), DiagnosticCode::FieldCountMismatch);
    EXPECT_EQ(diagnostic.mAffectedReports.size(), 4U);
    EXPECT_TRUE(diagnostic.mBlocking);
    ASSERT_EQ(diagnostic.mSources.size(), 1U);

    EXPECT_EQ(diagnostic.mSources[0].mRow, 2U);

    EXPECT_EQ(result.mStatus, OverallStatus::Failed);
}

TEST(ApplicationResultIntegrationTest, AFileWithABrokenHeaderFailsAllFourReports) {
    // The header has 22 columns: the required transaction_id is missing.
    constexpr std::string_view text =
        "datetime,date,account_type,category,type,asset_class,name,symbol,shares,price,amount,fee,"
        "tax,currency,original_amount,original_currency,fx_rate,description,"
        "counterparty_name,counterparty_iban,payment_reference,mcc_code\n";

    const TemporaryApplicationInputs files;
    const auto path = files.writeText("synthetic.csv", text);
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(path)}};
    const auto merged = DeterministicStatementMerger{}.merge(std::move(inputs));

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = merged.mDiagnostics});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(std::get<DiagnosticCode>(diagnostic.mCode), DiagnosticCode::InvalidHeader);
    EXPECT_EQ(diagnostic.mAffectedReports.size(), 4U);
    EXPECT_TRUE(diagnostic.mBlocking);
    ASSERT_EQ(diagnostic.mSources.size(), 1U);

    // A file-level error has the file but no row.
    EXPECT_EQ(diagnostic.mSources[0].mFilename->value(), "synthetic.csv");
    EXPECT_FALSE(diagnostic.mSources[0].mRow.has_value());

    EXPECT_EQ(result.mStatus, OverallStatus::Failed);
}

TEST(ApplicationResultIntegrationTest, ACardTransactionLeavesNoDiagnostic) {
    // A card payment is a known row type that the application ignores without a diagnostic.
    constexpr std::string_view cardPayment =
        R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,CASH,CARD_TRANSACTION,,,,,,-5,,,EUR,,,,,synthetic-card,,,,)";

    const TemporaryApplicationInputs files;
    const auto path = files.write("synthetic.csv", {cardPayment});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(path)}};
    const auto merged = DeterministicStatementMerger{}.merge(std::move(inputs));

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = merged.mDiagnostics});

    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(ReportType::Dividends).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(ReportType::BankDepositInterest).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(ReportType::OtherInterest).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.mStatus, OverallStatus::Success);
}

TEST(ApplicationResultIntegrationTest,
     ARejectedInterestRowDatedAfterTheLicenceChangeStillAffectsBothFormsIn2023) {
    // Dated 20 December 2023, after the change on 6 December, but the rejected row's date is not
    // trusted, so a 2023 run keeps both interest forms affected.
    constexpr std::string_view interest =
        R"(2023-12-20T09:00:00Z,2023-12-20,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,abc,,,EUR,,,,,synthetic-interest,,,,)";

    const TemporaryApplicationInputs files;
    const auto path = files.write("synthetic.csv", {interest});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(path)}};
    const auto merged = DeterministicStatementMerger{}.merge(std::move(inputs));

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2023, .mMergeDiagnostics = merged.mDiagnostics});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports,
              (std::vector{ReportType::BankDepositInterest, ReportType::OtherInterest}));
    EXPECT_EQ(result.report(ReportType::BankDepositInterest).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(ReportType::OtherInterest).mStatus, ReportStatus::Failed);
}

TEST(ApplicationResultIntegrationTest, ARejectedInterestRowAffectsBothFormsIn2022) {
    constexpr std::string_view interest =
        R"(2022-03-01T09:00:00Z,2022-03-01,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,abc,,,EUR,,,,,synthetic-interest,,,,)";

    const TemporaryApplicationInputs files;
    const auto path = files.write("synthetic.csv", {interest});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(path)}};
    const auto merged = DeterministicStatementMerger{}.merge(std::move(inputs));

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2022, .mMergeDiagnostics = merged.mDiagnostics});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports,
              (std::vector{ReportType::BankDepositInterest, ReportType::OtherInterest}));
    EXPECT_EQ(result.report(ReportType::BankDepositInterest).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(ReportType::OtherInterest).mStatus, ReportStatus::Failed);
}

TEST(ApplicationResultIntegrationTest, AnInvalidReportingYearFailsAllFourReports) {
    // 1999 is below the first supported year, 2000.
    constexpr std::string_view purchase =
        R"(2024-03-02T09:00:00Z,2024-03-02,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-purchase,,,,)";

    const TemporaryApplicationInputs files;
    const auto path = files.write("synthetic.csv", {purchase});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(path)}};
    const auto prepared = TaxProcessor::prepareReportingYear(
        {1999, DeterministicStatementMerger{}.merge(std::move(inputs))});

    EXPECT_FALSE(prepared.mLedger.has_value());

    ApplicationResultInput input{.mReportingYear = 1999,
                                 .mMergeDiagnostics = prepared.mMerged.mDiagnostics,
                                 .mProcessingDiagnostics = prepared.mDiagnostics};

    const auto result = ApplicationResultBuilder::build(std::move(input));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(std::get<ProcessingDiagnosticCode>(diagnostic.mCode),
              ProcessingDiagnosticCode::InvalidReportingYear);
    EXPECT_EQ(diagnostic.mStage, PipelineStage::HistoricalValidation);
    EXPECT_EQ(diagnostic.mField, "reporting_year");
    EXPECT_EQ(diagnostic.mAffectedReports.size(), 4U);
    EXPECT_TRUE(diagnostic.mSources.empty());
    EXPECT_EQ(result.mStatus, OverallStatus::Failed);

    const auto text = api::serializeApplicationResultJson(result);

    EXPECT_NE(text.find(R"("code":"invalid_reporting_year")"), std::string::npos);
    EXPECT_NE(text.find(R"("sources":[],"field":"reporting_year")"), std::string::npos);
}

TEST(ApplicationResultIntegrationTest, AnOpenRequestForTheIncompleteHistoryNeedsInput) {
    // A sale of 5 units after a purchase of 2 units: the FIFO pool lacks 3 units.
    constexpr std::string_view purchase =
        R"(2024-03-02T09:00:00Z,2024-03-02,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-purchase,,,,)";
    constexpr std::string_view sale =
        R"(2024-04-02T09:00:00Z,2024-04-02,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-5,10,50,,,EUR,,,,,synthetic-sale,,,,)";

    const TemporaryApplicationInputs files;
    const auto path = files.write("synthetic.csv", {purchase, sale});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(path)}};
    const auto prepared = TaxProcessor::prepareReportingYear(
        {2024, DeterministicStatementMerger{}.merge(std::move(inputs))});

    ASSERT_TRUE(prepared.mLedger.has_value());

    const auto fifo = FifoMatcher::match(prepared.mMerged.mStatement, *prepared.mLedger);

    ASSERT_EQ(fifo.mDiagnostics.size(), 1U);

    // No stage produces requests yet (#215), so the request is written here, on the sources of the
    // real FIFO diagnostic at position 0.
    ApplicationResultInput input{
        .mReportingYear = 2024,
        .mMergeDiagnostics = prepared.mMerged.mDiagnostics,
        .mProcessingDiagnostics = fifo.mDiagnostics,
        .mRequests = {RequestInput{
            .mQuestion =
                {
                    .mType = RequestType::CapitalGainsFailureResolutionRequired,
                    .mAffectedReports = {ReportType::CapitalGains},
                    .mSources = fifo.mDiagnostics[0].mContext.mSources,
                    .mIsin = fifo.mDiagnostics[0].mContext.mIsin,
                    .mData =
                        FailureResolutionQuestion{
                            .mInvestments = {FailedInvestment{
                                .mIsin = "XX0000000001",
                                .mName = "Synthetic Share",
                                .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
                            }}},
                },
            .mProcessingDiagnostics = {0},
        }},
    };
    input.mReports[1].mXml = "<dividends/>";

    const auto result = ApplicationResultBuilder::build(std::move(input));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    ASSERT_EQ(result.mRequests.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mRequestId, "request-0001");
    EXPECT_TRUE(result.mDiagnostics[0].mBlocking);

    const auto& request = result.mRequests[0];

    EXPECT_EQ(request.mId, "request-0001");
    EXPECT_EQ(request.mType, RequestType::CapitalGainsFailureResolutionRequired);
    EXPECT_EQ(request.mAffectedReports, (std::vector{ReportType::CapitalGains}));
    EXPECT_EQ(request.mIsin, "XX0000000001");
    ASSERT_EQ(request.mSources.size(), 1U);

    // Row 3 is the sale.
    EXPECT_EQ(request.mSources[0].mRow, 3U);
    EXPECT_EQ(request.mSources[0].mFilename->value(), "synthetic.csv");

    // The open question keeps capital gains from failing; the dividend XML stays available.
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::NeedsInput);
    EXPECT_EQ(result.report(ReportType::CapitalGains).mRequestIds,
              (std::vector<std::string>{"request-0001"}));
    EXPECT_EQ(result.report(ReportType::Dividends).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.mStatus, OverallStatus::NeedsInput);

    const auto text = api::serializeApplicationResultJson(result);

    EXPECT_NE(text.find(R"("requestId":"request-0001")"), std::string::npos);
    EXPECT_NE(
        text.find(
            R"("type":"capital_gains_failure_resolution_required","runRevision":1,"affectedReports":["capital_gains"])"),
        std::string::npos);
    // The investment names the real FIFO diagnostic of its ISIN.
    EXPECT_NE(
        text.find(
            R"("investments":[{"name":"Synthetic Share","isin":"XX0000000001","reasonCodes":["incomplete_history"],"diagnosticIds":["diagnostic-0001"]}])"),
        std::string::npos);
}

TEST(ApplicationResultIntegrationTest, AnAnsweredQuestionIsRecalculatedFromTheFullHistory) {
    // A sale of 5 units after a purchase of 2 units: the FIFO pool lacks 3 units.
    constexpr std::string_view purchase =
        R"(2024-03-02T09:00:00Z,2024-03-02,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-purchase,,,,)";
    constexpr std::string_view sale =
        R"(2024-04-02T09:00:00Z,2024-04-02,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-5,10,50,,,EUR,,,,,synthetic-sale,,,,)";

    const TemporaryApplicationInputs files;
    const auto path = files.write("synthetic.csv", {purchase, sale});
    tr::TradeRepublicParser parser;
    const auto parsed = parser.parse(path);
    int calculations = 0;

    // Every calculation starts again from the parsed file; no ledger is kept between them.
    const auto calculation = [&parsed, &calculations](RunDecisions& aDecisions) {
        ++calculations;

        std::vector<StatementMergeInput> inputs{{0, parsed}};
        auto prepared = TaxProcessor::prepareReportingYear(
            {2024, DeterministicStatementMerger{}.merge(std::move(inputs))});
        auto fifo = FifoMatcher::match(prepared.mMerged.mStatement, prepared.mLedger.value());

        // No stage asks the failure choice yet (#227), so the question is written here, on the
        // sources of the real FIFO diagnostic at position 0.
        const DecisionQuestion question{
            .mType = RequestType::CapitalGainsFailureResolutionRequired,
            .mAffectedReports = {ReportType::CapitalGains},
            .mSources = fifo.mDiagnostics.at(0).mContext.mSources,
            .mIsin = "XX0000000001",
            .mData =
                FailureResolutionQuestion{
                    .mInvestments = {FailedInvestment{
                        .mIsin = "XX0000000001",
                        .mName = "Synthetic Share",
                        .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
                    }},
                },
        };
        ApplicationResultInput input{
            .mReportingYear = 2024,
            .mMergeDiagnostics = std::move(prepared.mMerged.mDiagnostics),
            .mProcessingDiagnostics = std::move(fifo.mDiagnostics),
        };

        if (aDecisions.find(question) == nullptr)
        {
            input.mRequests = {RequestInput{.mQuestion = question, .mProcessingDiagnostics = {0}}};
        }

        return input;
    };
    DecisionSession session;

    const auto& first = session.start(calculation);

    EXPECT_EQ(first.mRunRevision, 1U);
    EXPECT_EQ(first.report(ReportType::CapitalGains).mStatus, ReportStatus::NeedsInput);
    ASSERT_EQ(first.mRequests.size(), 1U);
    ASSERT_EQ(first.mRequests[0].mSources.size(), 1U);

    // Row 3 is the sale.
    EXPECT_EQ(first.mRequests[0].mSources[0].mRow, 3U);

    ASSERT_EQ(first.mRequests[0].mInvestments.size(), 1U);
    EXPECT_EQ(first.mRequests[0].mInvestments[0].mDiagnosticIds,
              (std::vector<std::string>{"diagnostic-0001"}));

    const auto rejections = session.answer({DecisionAnswer{
        .mRequestId = "request-0001",
        .mRunRevision = 1,
        .mValue = FailureResolutionAnswer{FailureResolution::FailReport},
    }});

    EXPECT_TRUE(rejections.empty());

    // The report fails and keeps its error; no cost basis is invented.
    const auto& second = session.result();

    EXPECT_EQ(second.mRunRevision, 2U);
    EXPECT_EQ(calculations, 2);
    EXPECT_TRUE(second.mRequests.empty());
    EXPECT_EQ(second.report(ReportType::CapitalGains).mStatus, ReportStatus::Failed);
    EXPECT_FALSE(second.report(ReportType::CapitalGains).mXml.has_value());
    ASSERT_EQ(second.mDiagnostics.size(), 1U);

    const auto& diagnostic = second.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mId, "diagnostic-0001");
    EXPECT_EQ(diagnostic.mCode,
              ApplicationDiagnosticCode{ProcessingDiagnosticCode::IncompleteHistory});
    EXPECT_TRUE(diagnostic.mBlocking);
    EXPECT_FALSE(diagnostic.mRequestId.has_value());
}

TEST(ApplicationResultIntegrationTest, ANewRunOfTheSameFileUsesTheRecordedAnswerVisibly) {
    // A sale of 5 units after a purchase of 2 units: the FIFO pool lacks 3 units.
    constexpr std::string_view purchase =
        R"(2024-03-02T09:00:00Z,2024-03-02,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-purchase,,,,)";
    constexpr std::string_view sale =
        R"(2024-04-02T09:00:00Z,2024-04-02,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-5,10,50,,,EUR,,,,,synthetic-sale,,,,)";

    const TemporaryApplicationInputs files;
    const auto path = files.write("synthetic.csv", {purchase, sale});
    tr::TradeRepublicParser parser;
    const auto parsed = parser.parse(path);
    int calculations = 0;

    // Every calculation starts again from the parsed file; no ledger is kept between them.
    const auto calculation = [&parsed, &calculations](RunDecisions& aDecisions) {
        ++calculations;

        std::vector<StatementMergeInput> inputs{{0, parsed}};
        auto prepared = TaxProcessor::prepareReportingYear(
            {2024, DeterministicStatementMerger{}.merge(std::move(inputs))});
        auto fifo = FifoMatcher::match(prepared.mMerged.mStatement, prepared.mLedger.value());

        // No stage asks the failure choice yet (#227), so the question is written here, on the
        // sources of the real FIFO diagnostic at position 0.
        const DecisionQuestion question{
            .mType = RequestType::CapitalGainsFailureResolutionRequired,
            .mAffectedReports = {ReportType::CapitalGains},
            .mSources = fifo.mDiagnostics.at(0).mContext.mSources,
            .mIsin = "XX0000000001",
            .mData =
                FailureResolutionQuestion{
                    .mInvestments = {FailedInvestment{
                        .mIsin = "XX0000000001",
                        .mName = "Synthetic Share",
                        .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
                    }},
                },
        };
        ApplicationResultInput input{
            .mReportingYear = 2024,
            .mMergeDiagnostics = std::move(prepared.mMerged.mDiagnostics),
            .mProcessingDiagnostics = std::move(fifo.mDiagnostics),
        };

        if (aDecisions.find(question) == nullptr)
        {
            input.mRequests = {RequestInput{.mQuestion = question, .mProcessingDiagnostics = {0}}};
        }

        return input;
    };
    DecisionSession session;
    session.start(calculation);
    ASSERT_TRUE(session
                    .answer({DecisionAnswer{
                        .mRequestId = "request-0001",
                        .mRunRevision = 1,
                        .mValue = FailureResolutionAnswer{FailureResolution::FailReport},
                    }})
                    .empty());

    // A new run of the same file: the recorded fail_report applies again, so nothing is asked.
    const auto& third = session.start(calculation);

    EXPECT_EQ(third.mRunRevision, 3U);
    EXPECT_EQ(calculations, 3);
    EXPECT_TRUE(third.mRequests.empty());
    EXPECT_EQ(third.report(ReportType::CapitalGains).mStatus, ReportStatus::Failed);
    EXPECT_FALSE(third.report(ReportType::CapitalGains).mXml.has_value());
    ASSERT_EQ(third.mDiagnostics.size(), 1U);

    EXPECT_EQ(third.mDiagnostics[0].mId, "diagnostic-0001");
    EXPECT_TRUE(third.mDiagnostics[0].mBlocking);

    // The answer is listed, without sources, so it is a visible default, not a silent one.
    ASSERT_EQ(third.mDecisions.size(), 1U);

    const auto& decision = third.mDecisions[0];

    EXPECT_EQ(decision.mId, "decision-0001");
    EXPECT_EQ(decision.mAnswer,
              AnswerValue{FailureResolutionAnswer{FailureResolution::FailReport}});
    EXPECT_TRUE(decision.mFromEarlierRun);
    EXPECT_TRUE(decision.mUsed);

    const auto text = api::serializeApplicationResultJson(third);

    EXPECT_NE(text.find(R"("runRevision":3)"), std::string::npos);
    EXPECT_NE(
        text.find(
            R"("decisions":[{"id":"decision-0001","type":"capital_gains_failure_resolution_required","fromEarlierRun":true,"used":true,"investments":[{"name":"Synthetic Share","isin":"XX0000000001","reasonCodes":["incomplete_history"]}],"answer":"fail_report"}])"),
        std::string::npos);

    // Replaying the answer of the first run is rejected: its revision is old.
    const auto replayed = session.answer({DecisionAnswer{
        .mRequestId = "request-0001",
        .mRunRevision = 1,
        .mValue = FailureResolutionAnswer{FailureResolution::ExcludeFailedInstruments},
    }});

    ASSERT_EQ(replayed.size(), 1U);

    EXPECT_EQ(replayed[0].mReason, AnswerRejection::StaleRevision);
    EXPECT_EQ(replayed[0].mMessageKey, MessageKey::AnswerRejectedStaleRevision);
    EXPECT_EQ(replayed[0].mMessageParams,
              (std::vector<MessageParam>{textParam("requestId", "request-0001")}));

    // An answer with the current revision names no open request: nothing is asked in this run.
    const auto unasked = session.answer({DecisionAnswer{
        .mRequestId = "request-0001",
        .mRunRevision = 3,
        .mValue = FailureResolutionAnswer{FailureResolution::ExcludeFailedInstruments},
    }});

    ASSERT_EQ(unasked.size(), 1U);

    EXPECT_EQ(unasked[0].mReason, AnswerRejection::UnknownRequest);
    EXPECT_EQ(unasked[0].mMessageKey, MessageKey::AnswerRejectedUnknownRequest);

    // Neither rejection recalculated or changed the recorded answer.
    EXPECT_EQ(session.result().mRunRevision, 3U);
    EXPECT_EQ(calculations, 3);
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer,
              AnswerValue{FailureResolutionAnswer{FailureResolution::FailReport}});
}

TEST(ApplicationResultIntegrationTest, DeletingTheRecordedAnswerAsksTheQuestionAgain) {
    // A sale of 5 units after a purchase of 2 units: the FIFO pool lacks 3 units.
    constexpr std::string_view purchase =
        R"(2024-03-02T09:00:00Z,2024-03-02,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-purchase,,,,)";
    constexpr std::string_view sale =
        R"(2024-04-02T09:00:00Z,2024-04-02,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-5,10,50,,,EUR,,,,,synthetic-sale,,,,)";

    const TemporaryApplicationInputs files;
    const auto path = files.write("synthetic.csv", {purchase, sale});
    tr::TradeRepublicParser parser;
    const auto parsed = parser.parse(path);
    int calculations = 0;

    // Every calculation starts again from the parsed file; no ledger is kept between them.
    const auto calculation = [&parsed, &calculations](RunDecisions& aDecisions) {
        ++calculations;

        std::vector<StatementMergeInput> inputs{{0, parsed}};
        auto prepared = TaxProcessor::prepareReportingYear(
            {2024, DeterministicStatementMerger{}.merge(std::move(inputs))});
        auto fifo = FifoMatcher::match(prepared.mMerged.mStatement, prepared.mLedger.value());

        // No stage asks the failure choice yet (#227), so the question is written here, on the
        // sources of the real FIFO diagnostic at position 0.
        const DecisionQuestion question{
            .mType = RequestType::CapitalGainsFailureResolutionRequired,
            .mAffectedReports = {ReportType::CapitalGains},
            .mSources = fifo.mDiagnostics.at(0).mContext.mSources,
            .mIsin = "XX0000000001",
            .mData =
                FailureResolutionQuestion{
                    .mInvestments = {FailedInvestment{
                        .mIsin = "XX0000000001",
                        .mName = "Synthetic Share",
                        .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
                    }},
                },
        };
        ApplicationResultInput input{
            .mReportingYear = 2024,
            .mMergeDiagnostics = std::move(prepared.mMerged.mDiagnostics),
            .mProcessingDiagnostics = std::move(fifo.mDiagnostics),
        };

        if (aDecisions.find(question) == nullptr)
        {
            input.mRequests = {RequestInput{.mQuestion = question, .mProcessingDiagnostics = {0}}};
        }

        return input;
    };
    DecisionSession session;
    session.start(calculation);
    ASSERT_TRUE(session
                    .answer({DecisionAnswer{
                        .mRequestId = "request-0001",
                        .mRunRevision = 1,
                        .mValue = FailureResolutionAnswer{FailureResolution::FailReport},
                    }})
                    .empty());

    // Deleting with the revision of the first calculation, a decision that is not listed, or the
    // same decision twice is rejected and changes nothing.
    const auto staleAll = session.deleteAllAnswers(1);
    const auto unknown = session.deleteAnswers({"decision-0002"}, 2);
    const auto repeated = session.deleteAnswers({"decision-0001", "decision-0001"}, 2);

    ASSERT_EQ(staleAll.size(), 1U);

    EXPECT_EQ(staleAll[0].mReason, AnswerRejection::StaleRevision);
    EXPECT_EQ(staleAll[0].mMessageKey, MessageKey::AnswerRejectedStaleRevisionDelete);
    EXPECT_EQ(staleAll[0].mMessageParams,
              (std::vector<MessageParam>{textParam("decisionId", "all")}));

    ASSERT_EQ(unknown.size(), 1U);

    EXPECT_EQ(unknown[0].mReason, AnswerRejection::UnknownDecision);
    EXPECT_EQ(unknown[0].mMessageKey, MessageKey::AnswerRejectedUnknownDecision);
    EXPECT_EQ(unknown[0].mMessageParams,
              (std::vector<MessageParam>{textParam("decisionId", "decision-0002")}));

    ASSERT_EQ(repeated.size(), 1U);

    EXPECT_EQ(repeated[0].mIndex, 1U);
    EXPECT_EQ(repeated[0].mReason, AnswerRejection::DuplicateAnswer);
    EXPECT_EQ(repeated[0].mMessageKey, MessageKey::AnswerRejectedDuplicateDelete);

    EXPECT_EQ(session.result().mRunRevision, 2U);
    EXPECT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(calculations, 2);

    const auto rejections = session.deleteAnswers({"decision-0001"}, 2);

    EXPECT_TRUE(rejections.empty());

    // The full history is calculated again and the same question is open again.
    const auto& third = session.result();

    EXPECT_EQ(third.mRunRevision, 3U);
    EXPECT_EQ(calculations, 3);
    EXPECT_TRUE(third.mDecisions.empty());
    EXPECT_EQ(third.report(ReportType::CapitalGains).mStatus, ReportStatus::NeedsInput);
    EXPECT_FALSE(third.report(ReportType::CapitalGains).mXml.has_value());
    ASSERT_EQ(third.mRequests.size(), 1U);

    const auto& request = third.mRequests[0];

    EXPECT_EQ(request.mId, "request-0001");
    EXPECT_EQ(request.mRunRevision, 3U);
    ASSERT_EQ(request.mSources.size(), 1U);

    // Row 3 is the sale.
    EXPECT_EQ(request.mSources[0].mRow, 3U);

    ASSERT_EQ(request.mInvestments.size(), 1U);
    EXPECT_EQ(request.mInvestments[0].mDiagnosticIds,
              (std::vector<std::string>{"diagnostic-0001"}));

    // The real FIFO error names the open request again.
    ASSERT_EQ(third.mDiagnostics.size(), 1U);

    EXPECT_EQ(third.mDiagnostics[0].mId, "diagnostic-0001");
    EXPECT_EQ(third.mDiagnostics[0].mRequestId, "request-0001");
}

TEST(ApplicationResultIntegrationTest, AMixedRunWritesTheDocumentedJson) {
    // Row 2: a purchase with text in its price, so it is rejected. Row 3: a healthy purchase.
    // Rows 4 and 5: the same dividend twice, which the merger counts once with a warning.
    constexpr std::string_view rejectedTrade =
        R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000002,1,12x4567,-10,,,EUR,,,,,synthetic-rejected,,,,)";
    constexpr std::string_view purchase =
        R"(2024-03-02T09:00:00Z,2024-03-02,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-purchase,,,,)";
    constexpr std::string_view dividend =
        R"(2024-06-01T09:00:00Z,2024-06-01,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share,XX0000000001,,,5,,0,EUR,,,,,synthetic-dividend,,,,)";

    const TemporaryApplicationInputs files;
    const auto path = files.write("mixed.csv", {rejectedTrade, purchase, dividend, dividend});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(path)}};
    const auto merged = DeterministicStatementMerger{}.merge(std::move(inputs));

    ApplicationResultInput input{.mReportingYear = 2024, .mMergeDiagnostics = merged.mDiagnostics};
    input.mReports[1].mXml = "<dividends/>";

    const auto text =
        api::serializeApplicationResultJson(ApplicationResultBuilder::build(std::move(input)), 2);

    // Key order as in the diagnostics.md example. The warning is listed under dividends but does
    // not block it. Only capital gains and other interest have excludedInvestments. No XML, no
    // path and no raw value of the rejected row ("12x4567") appear.
    EXPECT_EQ(text, R"({
  "schemaVersion": 2,
  "status": "partial_success",
  "runRevision": 1,
  "reports": {
    "capitalGains": {
      "status": "failed",
      "diagnosticIds": [
        "diagnostic-0001"
      ],
      "requestIds": [],
      "excludedInvestments": [],
      "unsafe": false
    },
    "dividends": {
      "status": "generated",
      "diagnosticIds": [
        "diagnostic-0002"
      ],
      "requestIds": [],
      "unsafe": false,
      "downloadAvailable": true
    },
    "bankInterest": {
      "status": "no_data",
      "diagnosticIds": [],
      "requestIds": [],
      "unsafe": false
    },
    "otherInterest": {
      "status": "no_data",
      "diagnosticIds": [],
      "requestIds": [],
      "excludedInvestments": [],
      "unsafe": false
    }
  },
  "diagnostics": [
    {
      "id": "diagnostic-0001",
      "severity": "error",
      "stage": "parsing",
      "code": "invalid_value",
      "message": "Field 'price' has an invalid value in trade row; the row was skipped.",
      "messageKey": "invalid_value.field",
      "messageParams": {
        "field": "price",
        "rowKind": "row_kind.trade"
      },
      "affectedReports": [
        "capital_gains"
      ],
      "blocking": true,
      "sources": [
        {
          "broker": "trade_republic",
          "sourceId": "source-0001",
          "file": "mixed.csv",
          "row": 2,
          "transactionId": "synthetic-rejected"
        }
      ],
      "field": "price"
    },
    {
      "id": "diagnostic-0002",
      "severity": "warning",
      "stage": "merging",
      "code": "duplicate_transaction_in_file",
      "message": "The same transaction appears more than once in one file; it was counted once. Check the export for a copy error.",
      "messageKey": "duplicate_transaction_in_file",
      "messageParams": {},
      "affectedReports": [
        "dividends"
      ],
      "blocking": false,
      "sources": [
        {
          "broker": "trade_republic",
          "sourceId": "source-0001",
          "file": "mixed.csv",
          "row": 4,
          "transactionId": "synthetic-dividend"
        },
        {
          "broker": "trade_republic",
          "sourceId": "source-0001",
          "file": "mixed.csv",
          "row": 5,
          "transactionId": "synthetic-dividend"
        }
      ],
      "taxDate": "2024-06-01",
      "instrument": {
        "name": "Synthetic Share",
        "isin": "XX0000000001"
      }
    }
  ],
  "requests": [],
  "decisions": []
})");
}

} // namespace
