#include "parsers/traderepublic_parser.hpp"
#include "processors/fifo_matcher.hpp"
#include "taxbroker/api/application_result_json.hpp"
#include "taxbroker/application_result.hpp"

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
    const std::vector<StatementMergeInput> inputs{{0, parser.parse(path)}};
    const auto merged = DeterministicStatementMerger{}.merge(inputs);

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
    const std::vector<StatementMergeInput> forwardInputs{{0, first}, {1, second}};
    const std::vector<StatementMergeInput> backwardInputs{{1, second}, {0, first}};

    const auto forward =
        api::serializeApplicationResultJson(ApplicationResultBuilder::build(ApplicationResultInput{
            .mReportingYear = 2024,
            .mMergeDiagnostics =
                DeterministicStatementMerger{}.merge(forwardInputs).mDiagnostics}));
    const auto backward =
        api::serializeApplicationResultJson(ApplicationResultBuilder::build(ApplicationResultInput{
            .mReportingYear = 2024,
            .mMergeDiagnostics =
                DeterministicStatementMerger{}.merge(backwardInputs).mDiagnostics}));

    EXPECT_EQ(forward, backward);
    // The trade of the first file is diagnostic-0001, the dividend of the second file is 0002.
    EXPECT_LT(forward.find(R"("file":"first.csv")"), forward.find(R"("file":"second.csv")"));
    EXPECT_NE(forward.find(R"("id":"diagnostic-0002")"), std::string::npos);
}

TEST(ApplicationResultIntegrationTest, ARejectedTradeRepublicInterestRowIsDepositInterestIn2024) {
    // The amount of a broker interest row is not a number.
    constexpr std::string_view interest =
        R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,abc,,,EUR,,,,,synthetic-interest,,,,)";

    const TemporaryApplicationInputs files;
    const auto path = files.write("synthetic.csv", {interest});
    tr::TradeRepublicParser parser;
    const std::vector<StatementMergeInput> inputs{{0, parser.parse(path)}};
    const auto merged = DeterministicStatementMerger{}.merge(inputs);

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
    const std::vector<StatementMergeInput> inputs{{0, parser.parse(path)}};
    const auto merged = DeterministicStatementMerger{}.merge(inputs);

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
    const std::vector<StatementMergeInput> inputs{{0, parser.parse(path)}};
    const auto merged = DeterministicStatementMerger{}.merge(inputs);

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
    const std::vector<StatementMergeInput> inputs{{0, parser.parse(path)}};
    const auto prepared =
        TaxProcessor::prepareReportingYear({2024, DeterministicStatementMerger{}.merge(inputs)});

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
}

} // namespace
