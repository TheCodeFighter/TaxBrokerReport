#include "parsers/traderepublic_parser.hpp"
#include "processors/fifo_matcher.hpp"
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
#include <vector>

namespace {
using namespace taxbroker;

class TemporaryFifoInputs {
  public:
    TemporaryFifoInputs() {
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
        mDirectory = std::filesystem::temp_directory_path() /
                     ("taxbroker_fifo_synthetic_" + std::to_string(token));

        if (!std::filesystem::create_directory(mDirectory))
        {
            throw std::runtime_error{"Unable to create synthetic FIFO directory"};
        }
    }

    TemporaryFifoInputs(const TemporaryFifoInputs&) = delete;
    TemporaryFifoInputs& operator=(const TemporaryFifoInputs&) = delete;

    ~TemporaryFifoInputs() {
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
            throw std::runtime_error{"Unable to write synthetic FIFO CSV"};
        }

        return path;
    }

  private:
    std::filesystem::path mDirectory;
};

/// Reads the source file name, row and transaction ID of one trade of the first instrument.
const SourceReference& tradeSource(const MergedStatement& aStatement,
                                   const StatementEventReference& aEvent) {
    return aStatement.mPresentation.mTradeInstruments.at(0)
        .mTransactions.at(aEvent.mEventIndex)
        .mMetadata.mSources.at(0);
}

TEST(FifoMatchingIntegrationTest, PurchasesInOneFileAreMatchedToASaleInAnotherFile) {
    constexpr std::string_view firstPurchase =
        R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)";
    constexpr std::string_view secondPurchase =
        R"(2024-02-10T09:00:00Z,2024-02-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,20,20,-400,,,EUR,,,,,synthetic-buy-2,,,,)";
    constexpr std::string_view sale =
        R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-15,30,450,,,EUR,,,,,synthetic-sell,,,,)";

    const TemporaryFifoInputs files;
    const auto purchasesPath = files.write("purchases.csv", {firstPurchase, secondPurchase});
    const auto salesPath = files.write("sales.csv", {sale});
    tr::TradeRepublicParser parser;
    const std::vector<StatementMergeInput> inputs{{0, parser.parse(purchasesPath)},
                                                  {1, parser.parse(salesPath, 1)}};

    const auto prepared =
        TaxProcessor{}.prepareReportingYear({2024, DeterministicStatementMerger{}.merge(inputs)});

    ASSERT_TRUE(prepared.mLedger.has_value());
    EXPECT_TRUE(prepared.mMerged.mDiagnostics.empty());

    const auto result = FifoMatcher{}.match(prepared.mMerged.mStatement, *prepared.mLedger);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mIsin, "XX0000000001");
    EXPECT_EQ(isin.mName, "Synthetic Share");
    EXPECT_EQ(isin.mStatus, FifoIsinStatus::Matched);
    ASSERT_EQ(isin.mMatches.size(), 2U);

    const auto& statement = prepared.mMerged.mStatement;
    const auto& first = isin.mMatches.at(0);

    EXPECT_EQ(tradeSource(statement, first.mPurchase).mFilename.value(), "purchases.csv");
    EXPECT_EQ(tradeSource(statement, first.mPurchase).mSourceRow, 2U);
    EXPECT_EQ(tradeSource(statement, first.mPurchase).mTransactionId, "synthetic-buy-1");
    EXPECT_EQ(tradeSource(statement, first.mSale).mFilename.value(), "sales.csv");
    EXPECT_EQ(tradeSource(statement, first.mSale).mSourceRow, 2U);
    EXPECT_EQ(tradeSource(statement, first.mSale).mTransactionId, "synthetic-sell");
    EXPECT_EQ(first.mUnits, 1'000'000'000); // 10 units
    EXPECT_EQ(first.mBasis.mTicks,
              ExactRational{WideInteger{1'000'000'000'000'000'000}}); // 10 * 10
    EXPECT_EQ(first.mSaleValueTicks,
              ExactRational{WideInteger{3'000'000'000'000'000'000}});                   // 10 * 30
    EXPECT_EQ(first.mGainTicks, ExactRational{WideInteger{2'000'000'000'000'000'000}}); // 300 - 100

    const auto& second = isin.mMatches.at(1);

    EXPECT_EQ(tradeSource(statement, second.mPurchase).mFilename.value(), "purchases.csv");
    EXPECT_EQ(tradeSource(statement, second.mPurchase).mSourceRow, 3U);
    EXPECT_EQ(tradeSource(statement, second.mPurchase).mTransactionId, "synthetic-buy-2");
    EXPECT_EQ(second.mUnits, 500'000'000); // 5 units
    EXPECT_EQ(second.mBasis.mTicks,
              ExactRational{WideInteger{1'000'000'000'000'000'000}}); // 5 * 20
    EXPECT_EQ(second.mSaleValueTicks,
              ExactRational{WideInteger{1'500'000'000'000'000'000}});                  // 5 * 30
    EXPECT_EQ(second.mGainTicks, ExactRational{WideInteger{500'000'000'000'000'000}}); // 150 - 100

    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 1'500'000'000); // 20 - 5 = 15 units
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingBasis.mTicks,
              ExactRational{WideInteger{3'000'000'000'000'000'000}}); // 15 * 20 = EUR 300
}

TEST(FifoMatchingIntegrationTest, SaleInAnEarlierYearFileChangesTheOpeningInventory) {
    constexpr std::string_view purchase =
        R"(2022-03-01T09:00:00Z,2022-03-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,15,10,-150,,,EUR,,,,,synthetic-buy,,,,)";
    constexpr std::string_view earlierSale =
        R"(2023-05-10T09:00:00Z,2023-05-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-4,12,48,,,EUR,,,,,synthetic-sell-2023,,,,)";
    constexpr std::string_view selectedSale =
        R"(2024-06-20T09:00:00Z,2024-06-20,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-6,14,84,,,EUR,,,,,synthetic-sell-2024,,,,)";

    const TemporaryFifoInputs files;
    const auto purchasePath = files.write("purchase.csv", {purchase});
    const auto salesPath = files.write("sales.csv", {earlierSale, selectedSale});
    tr::TradeRepublicParser parser;
    const std::vector<StatementMergeInput> inputs{{0, parser.parse(purchasePath)},
                                                  {1, parser.parse(salesPath, 1)}};

    const auto prepared =
        TaxProcessor{}.prepareReportingYear({2024, DeterministicStatementMerger{}.merge(inputs)});

    ASSERT_TRUE(prepared.mLedger.has_value());

    const auto result = FifoMatcher{}.match(prepared.mMerged.mStatement, *prepared.mLedger);

    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::Matched);
    ASSERT_EQ(isin.mMatches.size(), 1U);

    const auto& statement = prepared.mMerged.mStatement;
    const auto& match = isin.mMatches.at(0);

    EXPECT_EQ(tradeSource(statement, match.mSale).mTransactionId, "synthetic-sell-2024");
    EXPECT_EQ(match.mSaleDate, parseCalendarDate("2024-06-20"));
    EXPECT_EQ(match.mPurchaseDate, parseCalendarDate("2022-03-01"));
    EXPECT_EQ(match.mUnits, 600'000'000);                                                // 6 units
    EXPECT_EQ(match.mBasis.mTicks, ExactRational{WideInteger{600'000'000'000'000'000}}); // 6 * 10
    EXPECT_EQ(match.mSaleValueTicks, ExactRational{WideInteger{840'000'000'000'000'000}}); // 6 * 14
    EXPECT_EQ(match.mGainTicks, ExactRational{WideInteger{240'000'000'000'000'000}}); // 84 - 60

    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 500'000'000); // 15 - 4 - 6 = 5 units
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingBasis.mTicks,
              ExactRational{WideInteger{500'000'000'000'000'000}}); // 5 * 10 = EUR 50
}

// Both files hold purchases and sales, and the later file is supplied first. Matching follows
// the dates, not the file order: in file order the March sale would take all 15 units from the
// 20 @ 20 lot (gain 150).
TEST(FifoMatchingIntegrationTest, MixedFilesInReverseOrderAreMatchedByDate) {
    constexpr std::string_view earlyPurchase =
        R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)";
    constexpr std::string_view earlySale =
        R"(2024-01-20T09:00:00Z,2024-01-20,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-3,12,36,,,EUR,,,,,synthetic-sell-1,,,,)";
    constexpr std::string_view latePurchase =
        R"(2024-02-10T09:00:00Z,2024-02-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,20,20,-400,,,EUR,,,,,synthetic-buy-2,,,,)";
    constexpr std::string_view lateSale =
        R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-15,30,450,,,EUR,,,,,synthetic-sell-2,,,,)";

    const TemporaryFifoInputs files;
    const auto laterPath = files.write("later.csv", {latePurchase, lateSale});
    const auto earlierPath = files.write("earlier.csv", {earlyPurchase, earlySale});
    tr::TradeRepublicParser parser;
    const std::vector<StatementMergeInput> inputs{{0, parser.parse(laterPath)},
                                                  {1, parser.parse(earlierPath, 1)}};

    const auto prepared =
        TaxProcessor{}.prepareReportingYear({2024, DeterministicStatementMerger{}.merge(inputs)});

    ASSERT_TRUE(prepared.mLedger.has_value());
    EXPECT_TRUE(prepared.mMerged.mDiagnostics.empty());

    const auto result = FifoMatcher{}.match(prepared.mMerged.mStatement, *prepared.mLedger);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::Matched);
    ASSERT_EQ(isin.mMatches.size(), 3U);

    const auto& statement = prepared.mMerged.mStatement;
    const auto& first = isin.mMatches.at(0);

    EXPECT_EQ(tradeSource(statement, first.mSale).mFilename.value(), "earlier.csv");
    EXPECT_EQ(tradeSource(statement, first.mSale).mSourceRow, 3U);
    EXPECT_EQ(tradeSource(statement, first.mPurchase).mFilename.value(), "earlier.csv");
    EXPECT_EQ(tradeSource(statement, first.mPurchase).mSourceRow, 2U);
    EXPECT_EQ(first.mUnits, 300'000'000);                                                // 3 units
    EXPECT_EQ(first.mBasis.mTicks, ExactRational{WideInteger{300'000'000'000'000'000}}); // 3 * 10
    EXPECT_EQ(first.mSaleValueTicks, ExactRational{WideInteger{360'000'000'000'000'000}}); // 3 * 12
    EXPECT_EQ(first.mGainTicks, ExactRational{WideInteger{60'000'000'000'000'000}}); // 36 - 30

    const auto& second = isin.mMatches.at(1);

    EXPECT_EQ(tradeSource(statement, second.mSale).mFilename.value(), "later.csv");
    EXPECT_EQ(tradeSource(statement, second.mSale).mSourceRow, 3U);
    EXPECT_EQ(tradeSource(statement, second.mPurchase).mFilename.value(), "earlier.csv");
    EXPECT_EQ(tradeSource(statement, second.mPurchase).mSourceRow, 2U);
    EXPECT_EQ(second.mUnits, 700'000'000); // 10 - 3 = 7 units left in the January lot
    EXPECT_EQ(second.mBasis.mTicks, ExactRational{WideInteger{700'000'000'000'000'000}}); // 7 * 10
    EXPECT_EQ(second.mSaleValueTicks,
              ExactRational{WideInteger{2'100'000'000'000'000'000}});                    // 7 * 30
    EXPECT_EQ(second.mGainTicks, ExactRational{WideInteger{1'400'000'000'000'000'000}}); // 210 - 70

    const auto& third = isin.mMatches.at(2);

    EXPECT_EQ(tradeSource(statement, third.mSale).mFilename.value(), "later.csv");
    EXPECT_EQ(tradeSource(statement, third.mSale).mSourceRow, 3U);
    EXPECT_EQ(tradeSource(statement, third.mPurchase).mFilename.value(), "later.csv");
    EXPECT_EQ(tradeSource(statement, third.mPurchase).mSourceRow, 2U);
    EXPECT_EQ(third.mUnits, 800'000'000); // 15 - 7 = 8 units
    EXPECT_EQ(third.mBasis.mTicks, ExactRational{WideInteger{1'600'000'000'000'000'000}}); // 8 * 20
    EXPECT_EQ(third.mSaleValueTicks,
              ExactRational{WideInteger{2'400'000'000'000'000'000}});                 // 8 * 30
    EXPECT_EQ(third.mGainTicks, ExactRational{WideInteger{800'000'000'000'000'000}}); // 240 - 160

    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(tradeSource(statement, isin.mOpenLots.at(0).mPurchase).mFilename.value(),
              "later.csv");
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 1'200'000'000); // 20 - 8 = 12 units
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingBasis.mTicks,
              ExactRational{WideInteger{2'400'000'000'000'000'000}}); // 12 * 20 = EUR 240
}

// Overlapping exports: the 02-10 purchase is in both files with the same transaction ID. The
// merger keeps one event with both sources, so FIFO creates one lot, not two.
TEST(FifoMatchingIntegrationTest, PurchaseRepeatedInOverlappingFilesCreatesOneLot) {
    constexpr std::string_view earlyPurchase =
        R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)";
    constexpr std::string_view earlySale =
        R"(2024-01-20T09:00:00Z,2024-01-20,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-3,12,36,,,EUR,,,,,synthetic-sell-1,,,,)";
    constexpr std::string_view latePurchase =
        R"(2024-02-10T09:00:00Z,2024-02-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,20,20,-400,,,EUR,,,,,synthetic-buy-2,,,,)";
    constexpr std::string_view lateSale =
        R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-15,30,450,,,EUR,,,,,synthetic-sell-2,,,,)";

    const TemporaryFifoInputs files;
    const auto laterPath = files.write("later.csv", {latePurchase, lateSale});
    const auto earlierPath = files.write("earlier.csv", {earlyPurchase, earlySale, latePurchase});
    tr::TradeRepublicParser parser;
    const std::vector<StatementMergeInput> inputs{{0, parser.parse(laterPath)},
                                                  {1, parser.parse(earlierPath, 1)}};

    const auto prepared =
        TaxProcessor{}.prepareReportingYear({2024, DeterministicStatementMerger{}.merge(inputs)});

    ASSERT_TRUE(prepared.mLedger.has_value());
    // A copy in another file is expected and adds no warning.
    EXPECT_TRUE(prepared.mMerged.mDiagnostics.empty());

    const auto result = FifoMatcher{}.match(prepared.mMerged.mStatement, *prepared.mLedger);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mIsins.size(), 1U);

    const auto& isin = result.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::Matched);
    ASSERT_EQ(isin.mMatches.size(), 3U);

    const auto& statement = prepared.mMerged.mStatement;
    const auto& third = isin.mMatches.at(2);
    const auto& repeatedPurchaseSources = statement.mPresentation.mTradeInstruments.at(0)
                                              .mTransactions.at(third.mPurchase.mEventIndex)
                                              .mMetadata.mSources;

    ASSERT_EQ(repeatedPurchaseSources.size(), 2U);
    EXPECT_EQ(repeatedPurchaseSources.at(0).mFilename.value(), "later.csv");
    EXPECT_EQ(repeatedPurchaseSources.at(0).mSourceRow, 2U);
    EXPECT_EQ(repeatedPurchaseSources.at(1).mFilename.value(), "earlier.csv");
    EXPECT_EQ(repeatedPurchaseSources.at(1).mSourceRow, 4U);

    EXPECT_EQ(isin.mMatches.at(0).mUnits, 300'000'000); // 3 units from the January lot
    EXPECT_EQ(isin.mMatches.at(1).mUnits, 700'000'000); // 7 units from the January lot
    EXPECT_EQ(third.mUnits, 800'000'000);               // 8 units from the February lot
    EXPECT_EQ(third.mGainTicks, ExactRational{WideInteger{800'000'000'000'000'000}}); // 240 - 160

    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits,
              1'200'000'000); // 20 - 8 = 12 units, not 32
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingBasis.mTicks,
              ExactRational{WideInteger{2'400'000'000'000'000'000}}); // 12 * 20 = EUR 240
}

TEST(FifoMatchingIntegrationTest, ShortSaleIsReportedWithItsSourceWhileIncomeEventsStayInTheYear) {
    // Example "Incomplete-history example" in fifo.md: 0.6 bought, 0.25 sold in 2023, then
    // 0.5 sold in 2024 while only 0.35 is open.
    constexpr std::string_view oldPurchase =
        R"(2022-03-01T09:00:00Z,2022-03-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,0.6,10,-6,,,EUR,,,,,synthetic-buy,,,,)";
    constexpr std::string_view oldSale =
        R"(2023-08-01T09:00:00Z,2023-08-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-0.25,12,3,,,EUR,,,,,synthetic-sell-2023,,,,)";
    constexpr std::string_view shortSale =
        R"(2024-05-01T09:00:00Z,2024-05-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-0.5,12,6,,,EUR,,,,,synthetic-sell-2024,,,,)";
    constexpr std::string_view dividend =
        R"(2024-06-01T09:00:00Z,2024-06-01,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share,XX0000000001,,,5,,0,EUR,,,,,synthetic-dividend,,,,)";
    constexpr std::string_view interest =
        R"(2024-07-01T09:00:00Z,2024-07-01,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,2,,0,EUR,,,,,synthetic-interest,,,,)";

    const TemporaryFifoInputs files;
    const auto oldPath = files.write("old.csv", {oldPurchase, oldSale});
    const auto newPath = files.write("new.csv", {shortSale, dividend, interest});
    tr::TradeRepublicParser parser;
    const std::vector<StatementMergeInput> inputs{{0, parser.parse(oldPath)},
                                                  {1, parser.parse(newPath, 1)}};

    const auto prepared =
        TaxProcessor{}.prepareReportingYear({2024, DeterministicStatementMerger{}.merge(inputs)});

    ASSERT_TRUE(prepared.mLedger.has_value());
    EXPECT_TRUE(prepared.mMerged.mDiagnostics.empty());

    const auto result = FifoMatcher{}.match(prepared.mMerged.mStatement, *prepared.mLedger);

    ASSERT_EQ(result.mIsins.size(), 1U);
    EXPECT_EQ(result.mIsins.at(0).mStatus, FifoIsinStatus::InsufficientUnits);
    EXPECT_EQ(result.mIsins.at(0).mAvailableUnits, 35'000'000); // 0.6 - 0.25 = 0.35 units
    EXPECT_TRUE(result.mIsins.at(0).mMatches.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.at(0);

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::IncompleteHistory);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::IncompleteHistory);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::HistoricalValidation);
    EXPECT_EQ(diagnostic.mContext.mAffectedReports,
              (std::vector<ReportType>{ReportType::CapitalGains}));
    EXPECT_EQ(diagnostic.mContext.mIsin, "XX0000000001");
    EXPECT_EQ(diagnostic.mContext.mInstrumentName, "Synthetic Share");
    EXPECT_EQ(diagnostic.mContext.mTaxDate, *parseCalendarDate("2024-05-01"));

    ASSERT_EQ(diagnostic.mContext.mSources.size(), 1U);

    EXPECT_EQ(diagnostic.mContext.mSources.at(0).mFilename.value(), "new.csv");
    EXPECT_EQ(diagnostic.mContext.mSources.at(0).mSourceRow, 2U);
    EXPECT_EQ(diagnostic.mContext.mSources.at(0).mTransactionId, "synthetic-sell-2024");

    // The dividend and the interest stay in the selected year and have no error.
    EXPECT_EQ(prepared.mLedger->mSelectedYearEvents,
              (std::vector<StatementEventReference>{
                  {StatementEventKind::Trade, 0, 2},
                  {StatementEventKind::Dividend, 0, 0},
                  {StatementEventKind::Interest, 0, 0},
              }));
}

TEST(FifoMatchingIntegrationTest,
     ShortSaleRepeatedInOverlappingFilesIsReportedOnceWithBothSources) {
    constexpr std::string_view shortSale =
        R"(2024-02-01T09:00:00Z,2024-02-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-1,12,12,,,EUR,,,,,synthetic-sell,,,,)";
    constexpr std::string_view otherSale =
        R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-2,12,24,,,EUR,,,,,synthetic-sell-other,,,,)";

    const TemporaryFifoInputs files;
    const auto firstPath = files.write("first.csv", {otherSale, shortSale});
    const auto secondPath = files.write("second.csv", {shortSale});
    tr::TradeRepublicParser parser;
    const std::vector<StatementMergeInput> inputs{{0, parser.parse(firstPath)},
                                                  {1, parser.parse(secondPath, 1)}};

    const auto prepared =
        TaxProcessor{}.prepareReportingYear({2024, DeterministicStatementMerger{}.merge(inputs)});

    ASSERT_TRUE(prepared.mLedger.has_value());
    EXPECT_TRUE(prepared.mMerged.mDiagnostics.empty());

    const auto result = FifoMatcher{}.match(prepared.mMerged.mStatement, *prepared.mLedger);

    ASSERT_EQ(result.mIsins.size(), 1U);
    EXPECT_EQ(result.mIsins.at(0).mStatus, FifoIsinStatus::InsufficientUnits);

    // Nothing was bought. The February sale comes first in the ledger, so only it is reported,
    // once, although two files contain it.
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.at(0);

    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::IncompleteHistory);
    EXPECT_EQ(diagnostic.mContext.mTaxDate, *parseCalendarDate("2024-02-01"));

    ASSERT_EQ(diagnostic.mContext.mSources.size(), 2U);

    EXPECT_EQ(diagnostic.mContext.mSources.at(0).mFilename.value(), "first.csv");
    EXPECT_EQ(diagnostic.mContext.mSources.at(0).mSourceRow, 3U);
    EXPECT_EQ(diagnostic.mContext.mSources.at(1).mFilename.value(), "second.csv");
    EXPECT_EQ(diagnostic.mContext.mSources.at(1).mSourceRow, 2U);
}

} // namespace
