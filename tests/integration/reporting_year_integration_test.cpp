#include "parsers/traderepublic_parser.hpp"
#include "processors/tax_processor.hpp"
#include "utils/date_utils.hpp"

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

class TemporaryReportingYearInputs {
  public:
    TemporaryReportingYearInputs() {
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
        mDirectory = std::filesystem::temp_directory_path() /
                     ("taxbroker_year_synthetic_" + std::to_string(token));

        if (!std::filesystem::create_directory(mDirectory))
        {
            throw std::runtime_error{"Unable to create synthetic reporting-year directory"};
        }
    }

    TemporaryReportingYearInputs(const TemporaryReportingYearInputs&) = delete;
    TemporaryReportingYearInputs& operator=(const TemporaryReportingYearInputs&) = delete;

    ~TemporaryReportingYearInputs() {
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
            throw std::runtime_error{"Unable to write synthetic reporting-year CSV"};
        }

        return path;
    }

  private:
    std::filesystem::path mDirectory;
};

TEST(ReportingYearIntegrationTest, WholeFileRetainsHistoryAndSeparatesJanuaryEvidence) {
    constexpr std::string_view oldPurchase =
        R"(2020-01-01T09:00:00Z,2020-01-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-old,,,,)";
    constexpr std::string_view decemberPurchase =
        R"(2023-12-02T09:00:00Z,2023-12-02,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-december,,,,)";
    constexpr std::string_view selectedSale =
        R"(2024-12-31T09:00:00Z,2024-12-31,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-3,10,30,,,EUR,,,,,synthetic-sale,,,,)";
    constexpr std::string_view januaryPurchase =
        R"(2025-01-30T09:00:00Z,2025-01-30,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,3,10,-30,,,EUR,,,,,synthetic-replacement,,,,)";
    constexpr std::string_view outsideSale =
        R"(2025-01-31T09:00:00Z,2025-01-31,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-1,10,10,,,EUR,,,,,synthetic-outside,,,,)";
    constexpr std::string_view januaryAction =
        R"(2025-01-30T08:00:00Z,2025-01-30,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,12,,,,,,,,,,synthetic-action,,,,)";
    constexpr std::string_view dividend =
        R"(2024-01-01T09:00:00Z,2024-01-01,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share,XX0000000001,,,5,,0,EUR,,,,,synthetic-dividend,,,,)";

    const TemporaryReportingYearInputs files;
    const auto path = files.write("whole.csv",
                                  {outsideSale,
                                   selectedSale,
                                   oldPurchase,
                                   januaryAction,
                                   decemberPurchase,
                                   januaryPurchase,
                                   dividend});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(path)}};

    const auto result = TaxProcessor{}.prepareReportingYear(
        {2024, DeterministicStatementMerger{}.merge(std::move(inputs))});

    ASSERT_TRUE(result.mLedger.has_value());

    EXPECT_EQ(result.mLedger->mHistoricalCapitalEventCount, 2U);
    EXPECT_EQ(result.mLedger->mCapitalEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Trade, 0, 0},
                  {StatementEventKind::Trade, 0, 1},
                  {StatementEventKind::Trade, 0, 2},
              }));
    EXPECT_EQ(result.mLedger->mSelectedYearEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Dividend, 0, 0},
                  {StatementEventKind::Trade, 0, 2},
              }));
    EXPECT_EQ(result.mLedger->mLossAnalysisEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Trade, 0, 1},
                  {StatementEventKind::Trade, 0, 2},
                  {StatementEventKind::CorporateAction, 0, 0},
                  {StatementEventKind::Trade, 0, 3},
              }));
    EXPECT_TRUE(result.mLedger->mHasSelectedYearSales);
    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_TRUE(result.mMerged.mDiagnostics.empty());

    const auto& instrument = result.mMerged.mStatement.mPresentation.mTradeInstruments.at(0);

    EXPECT_EQ(instrument.mIsin, "XX0000000001");
    EXPECT_EQ(instrument.mTransactions.at(0).mUnits, 1'000'000'000);
    EXPECT_EQ(instrument.mTransactions.at(2).mTradeSide, TradeSide::Sell);
    EXPECT_EQ(instrument.mTransactions.at(2).mUnits, 300'000'000);
    EXPECT_EQ(instrument.mTransactions.at(2).mMetadata.mTaxDate, parseCalendarDate("2024-12-31"));
    EXPECT_EQ(instrument.mTransactions.at(2).mMetadata.mSources.at(0).mTransactionId,
              "synthetic-sale");
    EXPECT_EQ(instrument.mTransactions.at(2).mMetadata.mSources.at(0).mFilename.value(),
              "whole.csv");
    EXPECT_EQ(instrument.mTransactions.at(2).mMetadata.mSources.at(0).mSourceRow, 3U);
    EXPECT_EQ(instrument.mTransactions.at(3).mMetadata.mSources.at(0).mTransactionId,
              "synthetic-replacement");
    EXPECT_EQ(instrument.mCorporateActions.at(0).mType, CorporateActionType::UnresolvedSplit);
    EXPECT_FALSE(instrument.mCorporateActions.at(0).mRatio.has_value());
    EXPECT_EQ(instrument.mCorporateActions.at(0).mUnitEvidence.at(0).mSourceText, "12");
}

TEST(ReportingYearIntegrationTest,
     OverlappingFilesAndReversedCompletionPreserveIndependentExpectedViews) {
    constexpr std::string_view oldPurchase =
        R"(2020-01-01T09:00:00Z,2020-01-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-old,,,,)";
    constexpr std::string_view decemberPurchase =
        R"(2023-12-02T09:00:00Z,2023-12-02,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-december,,,,)";
    constexpr std::string_view selectedSale =
        R"(2024-12-31T09:00:00Z,2024-12-31,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-3,10,30,,,EUR,,,,,synthetic-sale,,,,)";
    constexpr std::string_view januaryPurchase =
        R"(2025-01-30T09:00:00Z,2025-01-30,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,3,10,-30,,,EUR,,,,,synthetic-replacement,,,,)";
    constexpr std::string_view outsideSale =
        R"(2025-01-31T09:00:00Z,2025-01-31,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-1,10,10,,,EUR,,,,,synthetic-outside,,,,)";
    constexpr std::string_view januaryAction =
        R"(2025-01-30T08:00:00Z,2025-01-30,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,12,,,,,,,,,,synthetic-action,,,,)";
    constexpr std::string_view dividend =
        R"(2024-01-01T09:00:00Z,2024-01-01,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share,XX0000000001,,,5,,0,EUR,,,,,synthetic-dividend,,,,)";

    const TemporaryReportingYearInputs files;
    const auto firstPath =
        files.write("first.csv", {oldPurchase, decemberPurchase, dividend, selectedSale});
    const auto secondPath =
        files.write("second.csv", {outsideSale, januaryPurchase, januaryAction, selectedSale});
    tr::TradeRepublicParser parser;
    const StatementMergeInput first{0, parser.parse(firstPath)};
    const StatementMergeInput second{1, parser.parse(secondPath, 1)};
    std::vector<StatementMergeInput> requestOrder{first, second};
    std::vector<StatementMergeInput> completionOrder{second, first};

    const auto normal = TaxProcessor{}.prepareReportingYear(
        {2024, DeterministicStatementMerger{}.merge(std::move(requestOrder))});
    const auto reversed = TaxProcessor{}.prepareReportingYear(
        {2024, DeterministicStatementMerger{}.merge(std::move(completionOrder))});

    ASSERT_TRUE(normal.mLedger.has_value());
    ASSERT_TRUE(reversed.mLedger.has_value());

    EXPECT_EQ(normal.mLedger->mCapitalEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Trade, 0, 0},
                  {StatementEventKind::Trade, 0, 1},
                  {StatementEventKind::Trade, 0, 2},
              }));
    EXPECT_EQ(normal.mLedger->mSelectedYearEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Dividend, 0, 0},
                  {StatementEventKind::Trade, 0, 2},
              }));
    EXPECT_EQ(normal.mLedger->mLossAnalysisEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Trade, 0, 1},
                  {StatementEventKind::Trade, 0, 2},
                  {StatementEventKind::CorporateAction, 0, 0},
                  {StatementEventKind::Trade, 0, 3},
              }));
    EXPECT_EQ(reversed.mLedger->mCapitalEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Trade, 0, 0},
                  {StatementEventKind::Trade, 0, 1},
                  {StatementEventKind::Trade, 0, 2},
              }));
    EXPECT_EQ(reversed.mLedger->mSelectedYearEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Dividend, 0, 0},
                  {StatementEventKind::Trade, 0, 2},
              }));
    EXPECT_EQ(reversed.mLedger->mLossAnalysisEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Trade, 0, 1},
                  {StatementEventKind::Trade, 0, 2},
                  {StatementEventKind::CorporateAction, 0, 0},
                  {StatementEventKind::Trade, 0, 3},
              }));
    EXPECT_EQ(normal.mLedger->mHistoricalCapitalEventCount, 2U);
    EXPECT_EQ(reversed.mLedger->mHistoricalCapitalEventCount, 2U);
    EXPECT_TRUE(reversed.mLedger->mHasSelectedYearSales);
    EXPECT_TRUE(normal.mMerged.mDiagnostics.empty());
    EXPECT_TRUE(reversed.mMerged.mDiagnostics.empty());

    const auto& sale =
        reversed.mMerged.mStatement.mPresentation.mTradeInstruments.at(0).mTransactions.at(2);

    EXPECT_EQ(sale.mTradeSide, TradeSide::Sell);
    EXPECT_EQ(sale.mUnits, 300'000'000);
    EXPECT_EQ(sale.mUnitPrice, 1'000'000'000);
    EXPECT_EQ(sale.mMetadata.mTaxDate, parseCalendarDate("2024-12-31"));
    ASSERT_EQ(sale.mMetadata.mSources.size(), 2U);

    EXPECT_EQ(sale.mMetadata.mSources.at(0).mFilename.value(), "first.csv");
    EXPECT_EQ(sale.mMetadata.mSources.at(0).mSourceRow, 5U);
    EXPECT_EQ(sale.mMetadata.mSources.at(0).mInputSequence.mSourceIndex, 0U);
    EXPECT_EQ(sale.mMetadata.mSources.at(1).mFilename.value(), "second.csv");
    EXPECT_EQ(sale.mMetadata.mSources.at(1).mSourceRow, 5U);
    EXPECT_EQ(sale.mMetadata.mSources.at(1).mInputSequence.mSourceIndex, 1U);
    EXPECT_EQ(sale.mMetadata.mSources.at(0).mTransactionId, "synthetic-sale");
    EXPECT_EQ(sale.mMetadata.mSources.at(1).mTransactionId, "synthetic-sale");
}

TEST(ReportingYearIntegrationTest, DisjointFilesPreserveSelectedYearAndHistoricalBoundary) {
    constexpr std::string_view oldPurchase =
        R"(2020-01-01T09:00:00Z,2020-01-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-old,,,,)";
    constexpr std::string_view decemberPurchase =
        R"(2023-12-02T09:00:00Z,2023-12-02,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-december,,,,)";
    constexpr std::string_view selectedSale =
        R"(2024-12-31T09:00:00Z,2024-12-31,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-3,10,30,,,EUR,,,,,synthetic-sale,,,,)";
    constexpr std::string_view januaryPurchase =
        R"(2025-01-30T09:00:00Z,2025-01-30,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,3,10,-30,,,EUR,,,,,synthetic-replacement,,,,)";
    constexpr std::string_view outsideSale =
        R"(2025-01-31T09:00:00Z,2025-01-31,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-1,10,10,,,EUR,,,,,synthetic-outside,,,,)";
    constexpr std::string_view januaryAction =
        R"(2025-01-30T08:00:00Z,2025-01-30,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,12,,,,,,,,,,synthetic-action,,,,)";
    constexpr std::string_view dividend =
        R"(2024-01-01T09:00:00Z,2024-01-01,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share,XX0000000001,,,5,,0,EUR,,,,,synthetic-dividend,,,,)";

    const TemporaryReportingYearInputs files;
    const auto firstPath = files.write("history.csv", {oldPurchase, decemberPurchase});
    const auto secondPath = files.write("selected.csv", {dividend, selectedSale});
    const auto thirdPath = files.write("future.csv", {januaryAction, januaryPurchase, outsideSale});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{
        {2, parser.parse(thirdPath, 2)},
        {0, parser.parse(firstPath)},
        {1, parser.parse(secondPath, 1)},
    };

    const auto result = TaxProcessor{}.prepareReportingYear(
        {2024, DeterministicStatementMerger{}.merge(std::move(inputs))});

    ASSERT_TRUE(result.mLedger.has_value());

    EXPECT_EQ(result.mLedger->mHistoricalCapitalEventCount, 2U);
    EXPECT_EQ(result.mLedger->mCapitalEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Trade, 0, 0},
                  {StatementEventKind::Trade, 0, 1},
                  {StatementEventKind::Trade, 0, 2},
              }));
    EXPECT_EQ(result.mLedger->mSelectedYearEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Dividend, 0, 0},
                  {StatementEventKind::Trade, 0, 2},
              }));
    EXPECT_EQ(result.mLedger->mLossAnalysisEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Trade, 0, 1},
                  {StatementEventKind::Trade, 0, 2},
                  {StatementEventKind::CorporateAction, 0, 0},
                  {StatementEventKind::Trade, 0, 3},
              }));
    EXPECT_TRUE(result.mMerged.mDiagnostics.empty());
    EXPECT_EQ(result.mMerged.mStatement.mPresentation.mTradeInstruments.at(0)
                  .mTransactions.at(2)
                  .mMetadata.mSources.at(0)
                  .mFilename.value(),
              "selected.csv");
}

TEST(ReportingYearIntegrationTest, RejectedSelectedYearSaleStaysVisibleBesideAnEmptyPreparedYear) {
    constexpr std::string_view rejectedSale =
        R"(2024-12-31T09:00:00Z,2024-12-31,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-1,invalid,10,,,EUR,,,,,synthetic-invalid,,,,)";
    constexpr std::string_view futurePurchase =
        R"(2025-01-01T09:00:00Z,2025-01-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1,10,-10,,,EUR,,,,,synthetic-future,,,,)";
    const TemporaryReportingYearInputs files;
    const auto path = files.write("partial.csv", {rejectedSale, futurePurchase});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(path)}};

    const auto result = TaxProcessor{}.prepareReportingYear(
        {2024, DeterministicStatementMerger{}.merge(std::move(inputs))});

    ASSERT_TRUE(result.mLedger.has_value());

    EXPECT_TRUE(result.mLedger->mCapitalEvents.empty());
    EXPECT_TRUE(result.mLedger->mSelectedYearEvents.empty());
    EXPECT_FALSE(result.mLedger->mHasSelectedYearSales);
    EXPECT_EQ(result.mLedger->mLossAnalysisEvents,
              (std::vector<StatementEventReference>{{StatementEventKind::Trade, 0, 0}}));
    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mMerged.mDiagnostics.size(), 1U);

    const auto& diagnostic = std::get<SourcedParseDiagnostic>(result.mMerged.mDiagnostics.front());

    EXPECT_EQ(diagnostic.mSourceIndex, 0U);
    EXPECT_EQ(diagnostic.mBroker, Broker::TradeRepublic);
    EXPECT_EQ(diagnostic.mDiagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mDiagnostic.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(diagnostic.mDiagnostic.mField, "price");
    EXPECT_EQ(diagnostic.mDiagnostic.mSourceFile, "partial.csv");
    EXPECT_EQ(diagnostic.mDiagnostic.mRowIndex, 2U);
    EXPECT_EQ(diagnostic.mDiagnostic.mTransactionId, "synthetic-invalid");
}

TEST(ReportingYearIntegrationTest, HeaderOnlyFilePreparesAnExplicitEmptyYearWithoutErrors) {
    const TemporaryReportingYearInputs files;
    const auto path = files.write("empty.csv", {});
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs{{0, parser.parse(path)}};

    const auto result = TaxProcessor{}.prepareReportingYear(
        {2024, DeterministicStatementMerger{}.merge(std::move(inputs))});

    ASSERT_TRUE(result.mLedger.has_value());

    EXPECT_TRUE(result.mLedger->mCapitalEvents.empty());
    EXPECT_TRUE(result.mLedger->mSelectedYearEvents.empty());
    EXPECT_TRUE(result.mLedger->mLossAnalysisEvents.empty());
    EXPECT_FALSE(result.mLedger->mHasSelectedYearSales);
    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_TRUE(result.mMerged.mDiagnostics.empty());
}

} // namespace
