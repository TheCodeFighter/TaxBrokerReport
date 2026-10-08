#include "../support/statement_merge_assertions.hpp"
#include "parsers/traderepublic_parser.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {
using namespace taxbroker;

// Each test supplies its own synthetic row; unspecified CSV fields are empty.
struct SyntheticCsvRow {
    std::string_view mDatetime;
    std::string_view mDate;
    std::string_view mAccountType;
    std::string_view mCategory;
    std::string_view mType;
    std::string_view mAssetClass;
    std::string_view mName;
    std::string_view mSymbol;
    std::string_view mShares;
    std::string_view mPrice;
    std::string_view mAmount;
    std::string_view mFee;
    std::string_view mTax;
    std::string_view mCurrency;
    std::string_view mOriginalAmount;
    std::string_view mOriginalCurrency;
    std::string_view mFxRate;
    std::string_view mDescription;
    std::string_view mTransactionId;
    std::string_view mCounterpartyName;
    std::string_view mCounterpartyIban;
    std::string_view mPaymentReference;
    std::string_view mMccCode;
};

class TemporaryCsvInputs {
  public:
    TemporaryCsvInputs() {
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
        mDirectory = std::filesystem::temp_directory_path() /
                     ("taxbroker_exact_synthetic_" + std::to_string(token));

        if (!std::filesystem::create_directory(mDirectory))
        {
            throw std::runtime_error{"Unable to create synthetic quantity directory"};
        }
    }

    TemporaryCsvInputs(const TemporaryCsvInputs&) = delete;
    TemporaryCsvInputs& operator=(const TemporaryCsvInputs&) = delete;

    ~TemporaryCsvInputs() {
        std::error_code error;
        std::filesystem::remove_all(mDirectory, error);
    }

    std::filesystem::path write(std::string_view aName, const SyntheticCsvRow& aRow) const {
        const auto path = mDirectory / aName;
        std::ofstream output{path};
        const std::array fields{aRow.mDatetime,
                                aRow.mDate,
                                aRow.mAccountType,
                                aRow.mCategory,
                                aRow.mType,
                                aRow.mAssetClass,
                                aRow.mName,
                                aRow.mSymbol,
                                aRow.mShares,
                                aRow.mPrice,
                                aRow.mAmount,
                                aRow.mFee,
                                aRow.mTax,
                                aRow.mCurrency,
                                aRow.mOriginalAmount,
                                aRow.mOriginalCurrency,
                                aRow.mFxRate,
                                aRow.mDescription,
                                aRow.mTransactionId,
                                aRow.mCounterpartyName,
                                aRow.mCounterpartyIban,
                                aRow.mPaymentReference,
                                aRow.mMccCode};

        output << "datetime,date,account_type,category,type,asset_class,name,symbol,shares,"
                  "price,amount,fee,tax,currency,original_amount,original_currency,fx_rate,"
                  "description,transaction_id,counterparty_name,counterparty_iban,"
                  "payment_reference,mcc_code\n";

        for (std::size_t index = 0; index < fields.size(); ++index)
        {
            if (index != 0)
            {
                output << ',';
            }

            output << '"';

            for (const char character : fields[index])
            {
                if (character == '"')
                {
                    output << '"';
                }

                output << character;
            }

            output << '"';
        }

        output << '\n';
        output.close();

        if (!output)
        {
            throw std::runtime_error{"Unable to write synthetic quantity input"};
        }

        return path;
    }

  private:
    std::filesystem::path mDirectory;
};

TEST(ExactArithmeticIntegrationTest, RetainsEvidenceAcrossOverlapsAndParserCompletionOrders) {
    const TemporaryCsvInputs files;
    const auto firstFile = files.write("first.csv",
                                       {.mDatetime = "2025-01-01T08:00:00.000Z",
                                        .mDate = "2025-01-01",
                                        .mAccountType = "DEFAULT",
                                        .mCategory = "TRADING",
                                        .mType = "BUY",
                                        .mAssetClass = "STOCK",
                                        .mName = "Synthetic Merger Share",
                                        .mSymbol = "XX8000000001",
                                        .mShares = "0.123456784",
                                        .mPrice = "10.000000",
                                        .mAmount = "-10.00",
                                        .mCurrency = "EUR",
                                        .mDescription = "Synthetic future buy",
                                        .mTransactionId = "synthetic-future"});
    const auto secondFile = files.write("second.csv",
                                        {.mDatetime = "2025-01-01T08:00:00.000Z",
                                         .mDate = "2025-01-01",
                                         .mAccountType = "DEFAULT",
                                         .mCategory = "TRADING",
                                         .mType = "BUY",
                                         .mAssetClass = "STOCK",
                                         .mName = "Synthetic Merger Share",
                                         .mSymbol = "XX8000000001",
                                         .mShares = "0.12345678400",
                                         .mPrice = "10.000000",
                                         .mAmount = "-10.00",
                                         .mCurrency = "EUR",
                                         .mDescription = "Synthetic future buy",
                                         .mTransactionId = "synthetic-future"});
    tr::TradeRepublicParser parser;
    std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(firstFile, 0)},
        StatementMergeInput{.mSourceIndex = 1, .mParseResult = parser.parse(secondFile, 1)},
    };
    const DeterministicStatementMerger merger;

    const auto forward = merger.merge(inputs);
    std::reverse(inputs.begin(), inputs.end());
    const auto reversed = merger.merge(inputs);

    EXPECT_TRUE(forward.mDiagnostics.empty());
    EXPECT_TRUE(reversed.mDiagnostics.empty());
    ASSERT_EQ(forward.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = forward.mStatement.mPresentation.mTradeInstruments.front();

    ASSERT_EQ(instrument.mTransactions.size(), 1U);

    const auto& transaction = instrument.mTransactions.front();

    ASSERT_EQ(transaction.mUnitEvidence.size(), 2U);

    // 0.123456784 rounds to 0.12345678; 0.12345678 * 100,000,000 = 12,345,678.
    EXPECT_EQ(transaction.mUnits, 12'345'678);
    EXPECT_EQ(transaction.mUnitEvidence[0].mSourceText, "0.123456784");
    EXPECT_EQ(transaction.mUnitEvidence[1].mDiscardedDigits, "400");
    EXPECT_EQ(transaction.mUnitEvidence[0].mSource.mInputSequence.mSourceIndex, 0U);
    EXPECT_EQ(transaction.mUnitEvidence[1].mSource.mInputSequence.mSourceIndex, 1U);

    test::expectStatementsEqual(forward.mStatement.mPresentation,
                                reversed.mStatement.mPresentation);
    EXPECT_EQ(forward.mStatement.mChronologicalOrder, reversed.mStatement.mChronologicalOrder);
}

TEST(ExactArithmeticIntegrationTest, RejectsDistinctTradeSourceFactsThatRoundToTheSameUnits) {
    const TemporaryCsvInputs files;
    const auto firstFile = files.write("first.csv",
                                       {.mDatetime = "2025-01-01T08:00:00.000Z",
                                        .mDate = "2025-01-01",
                                        .mAccountType = "DEFAULT",
                                        .mCategory = "TRADING",
                                        .mType = "BUY",
                                        .mAssetClass = "STOCK",
                                        .mName = "Synthetic Merger Share",
                                        .mSymbol = "XX8000000001",
                                        .mShares = "0.123456784",
                                        .mPrice = "10.000000",
                                        .mAmount = "-10.00",
                                        .mCurrency = "EUR",
                                        .mDescription = "Synthetic future buy",
                                        .mTransactionId = "synthetic-future"});
    const auto secondFile = files.write("second.csv",
                                        {.mDatetime = "2025-01-01T08:00:00.000Z",
                                         .mDate = "2025-01-01",
                                         .mAccountType = "DEFAULT",
                                         .mCategory = "TRADING",
                                         .mType = "BUY",
                                         .mAssetClass = "STOCK",
                                         .mName = "Synthetic Merger Share",
                                         .mSymbol = "XX8000000001",
                                         .mShares = "0.123456783",
                                         .mPrice = "10.000000",
                                         .mAmount = "-10.00",
                                         .mCurrency = "EUR",
                                         .mDescription = "Synthetic future buy",
                                         .mTransactionId = "synthetic-future"});
    tr::TradeRepublicParser parser;
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(firstFile, 0)},
        StatementMergeInput{.mSourceIndex = 1, .mParseResult = parser.parse(secondFile, 1)},
    };

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    EXPECT_TRUE(result.mStatement.mChronologicalOrder.empty());
    EXPECT_TRUE(result.mStatement.mPresentation.mTradeInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<MergeDiagnostic>(result.mDiagnostics.front()));

    const auto& diagnostic = std::get<MergeDiagnostic>(result.mDiagnostics.front());

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, MergeDiagnosticCode::ConflictingDuplicate);
    ASSERT_EQ(diagnostic.mSources.size(), 2U);

    EXPECT_EQ(diagnostic.mSources[0].mFilename, SourceFilename::fromPath("first.csv"));
    EXPECT_EQ(diagnostic.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(diagnostic.mSources[1].mFilename, SourceFilename::fromPath("second.csv"));
    EXPECT_EQ(diagnostic.mSources[1].mSourceRow, 2U);
    EXPECT_EQ(diagnostic.mMessage.find("0.123"), std::string::npos);
}

TEST(ExactArithmeticIntegrationTest, RejectsDistinctSplitSourceFactsThatRoundToTheSameUnits) {
    const TemporaryCsvInputs files;
    const auto firstFile = files.write("first.csv",
                                       {.mDatetime = "2025-01-01T12:00:00.000Z",
                                        .mDate = "2025-01-01",
                                        .mAccountType = "DEFAULT",
                                        .mCategory = "CORPORATE_ACTION",
                                        .mType = "SPLIT",
                                        .mAssetClass = "STOCK",
                                        .mName = "Synthetic Merger Share",
                                        .mSymbol = "XX8000000001",
                                        .mShares = "0.123456784",
                                        .mPrice = "",
                                        .mAmount = "",
                                        .mCurrency = "",
                                        .mDescription = "Synthetic unresolved split",
                                        .mTransactionId = "synthetic-action"});
    const auto secondFile = files.write("second.csv",
                                        {.mDatetime = "2025-01-01T12:00:00.000Z",
                                         .mDate = "2025-01-01",
                                         .mAccountType = "DEFAULT",
                                         .mCategory = "CORPORATE_ACTION",
                                         .mType = "SPLIT",
                                         .mAssetClass = "STOCK",
                                         .mName = "Synthetic Merger Share",
                                         .mSymbol = "XX8000000001",
                                         .mShares = "0.123456783",
                                         .mPrice = "",
                                         .mAmount = "",
                                         .mCurrency = "",
                                         .mDescription = "Synthetic unresolved split",
                                         .mTransactionId = "synthetic-action"});
    tr::TradeRepublicParser parser;
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(firstFile, 0)},
        StatementMergeInput{.mSourceIndex = 1, .mParseResult = parser.parse(secondFile, 1)},
    };

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    EXPECT_TRUE(result.mStatement.mChronologicalOrder.empty());
    EXPECT_TRUE(result.mStatement.mPresentation.mTradeInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<MergeDiagnostic>(result.mDiagnostics.front()));

    const auto& diagnostic = std::get<MergeDiagnostic>(result.mDiagnostics.front());

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, MergeDiagnosticCode::ConflictingDuplicate);
    ASSERT_EQ(diagnostic.mSources.size(), 2U);

    EXPECT_EQ(diagnostic.mSources[0].mFilename, SourceFilename::fromPath("first.csv"));
    EXPECT_EQ(diagnostic.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(diagnostic.mSources[1].mFilename, SourceFilename::fromPath("second.csv"));
    EXPECT_EQ(diagnostic.mSources[1].mSourceRow, 2U);
    EXPECT_EQ(diagnostic.mMessage.find("0.123"), std::string::npos);
}

TEST(ExactArithmeticIntegrationTest, KnownTradeSourceConflictSurvivesMissingEvidence) {
    const TemporaryCsvInputs files;
    const auto firstFile = files.write("first.csv",
                                       {.mDatetime = "2025-01-01T08:00:00.000Z",
                                        .mDate = "2025-01-01",
                                        .mAccountType = "DEFAULT",
                                        .mCategory = "TRADING",
                                        .mType = "BUY",
                                        .mAssetClass = "STOCK",
                                        .mName = "Synthetic Merger Share",
                                        .mSymbol = "XX8000000001",
                                        .mShares = "0.123456784",
                                        .mPrice = "10.000000",
                                        .mAmount = "-10.00",
                                        .mCurrency = "EUR",
                                        .mDescription = "Synthetic future buy",
                                        .mTransactionId = "synthetic-future"});
    const auto secondFile = files.write("second.csv",
                                        {.mDatetime = "2025-01-01T08:00:00.000Z",
                                         .mDate = "2025-01-01",
                                         .mAccountType = "DEFAULT",
                                         .mCategory = "TRADING",
                                         .mType = "BUY",
                                         .mAssetClass = "STOCK",
                                         .mName = "Synthetic Merger Share",
                                         .mSymbol = "XX8000000001",
                                         .mShares = "0.12345678400",
                                         .mPrice = "10.000000",
                                         .mAmount = "-10.00",
                                         .mCurrency = "EUR",
                                         .mDescription = "Synthetic future buy",
                                         .mTransactionId = "synthetic-future"});
    const auto thirdFile = files.write("third.csv",
                                       {.mDatetime = "2025-01-01T08:00:00.000Z",
                                        .mDate = "2025-01-01",
                                        .mAccountType = "DEFAULT",
                                        .mCategory = "TRADING",
                                        .mType = "BUY",
                                        .mAssetClass = "STOCK",
                                        .mName = "Synthetic Merger Share",
                                        .mSymbol = "XX8000000001",
                                        .mShares = "0.123456783",
                                        .mPrice = "10.000000",
                                        .mAmount = "-10.00",
                                        .mCurrency = "EUR",
                                        .mDescription = "Synthetic future buy",
                                        .mTransactionId = "synthetic-future"});
    tr::TradeRepublicParser parser;
    std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(firstFile, 0)},
        StatementMergeInput{.mSourceIndex = 1, .mParseResult = parser.parse(secondFile, 1)},
        StatementMergeInput{.mSourceIndex = 2, .mParseResult = parser.parse(thirdFile, 2)},
    };

    // Simulate a parser that supplies rounded units without the original decimal evidence.
    inputs[0].mParseResult.mStatement.mTradeInstruments[0].mTransactions[0].mUnitEvidence.clear();

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    EXPECT_TRUE(result.mStatement.mChronologicalOrder.empty());
    EXPECT_TRUE(result.mStatement.mPresentation.mTradeInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<MergeDiagnostic>(result.mDiagnostics.front()));

    const auto& diagnostic = std::get<MergeDiagnostic>(result.mDiagnostics.front());

    EXPECT_EQ(diagnostic.mCode, MergeDiagnosticCode::ConflictingDuplicate);
}

TEST(ExactArithmeticIntegrationTest, MissingTradeEvidenceDoesNotFabricateProof) {
    const TemporaryCsvInputs files;
    const auto firstFile = files.write("first.csv",
                                       {.mDatetime = "2025-01-01T08:00:00.000Z",
                                        .mDate = "2025-01-01",
                                        .mAccountType = "DEFAULT",
                                        .mCategory = "TRADING",
                                        .mType = "BUY",
                                        .mAssetClass = "STOCK",
                                        .mName = "Synthetic Merger Share",
                                        .mSymbol = "XX8000000001",
                                        .mShares = "0.123456784",
                                        .mPrice = "10.000000",
                                        .mAmount = "-10.00",
                                        .mCurrency = "EUR",
                                        .mDescription = "Synthetic future buy",
                                        .mTransactionId = "synthetic-future"});
    const auto secondFile = files.write("second.csv",
                                        {.mDatetime = "2025-01-01T08:00:00.000Z",
                                         .mDate = "2025-01-01",
                                         .mAccountType = "DEFAULT",
                                         .mCategory = "TRADING",
                                         .mType = "BUY",
                                         .mAssetClass = "STOCK",
                                         .mName = "Synthetic Merger Share",
                                         .mSymbol = "XX8000000001",
                                         .mShares = "0.12345678400",
                                         .mPrice = "10.000000",
                                         .mAmount = "-10.00",
                                         .mCurrency = "EUR",
                                         .mDescription = "Synthetic future buy",
                                         .mTransactionId = "synthetic-future"});
    tr::TradeRepublicParser parser;
    std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(firstFile, 0)},
        StatementMergeInput{.mSourceIndex = 1, .mParseResult = parser.parse(secondFile, 1)},
    };

    // Simulate a parser that supplies rounded units without the original decimal evidence.
    inputs[0].mParseResult.mStatement.mTradeInstruments[0].mTransactions[0].mUnitEvidence.clear();

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 1U);
    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments.front();

    ASSERT_EQ(instrument.mTransactions.size(), 1U);

    const auto& transaction = instrument.mTransactions.front();

    // 0.123456784 rounds to 0.12345678; stored units use a scale of 100,000,000.
    EXPECT_EQ(transaction.mUnits, 12'345'678);
    ASSERT_EQ(transaction.mUnitEvidence.size(), 1U);

    const auto& evidence = transaction.mUnitEvidence.front();

    EXPECT_EQ(evidence.mSourceText, "0.12345678400");
    EXPECT_EQ(evidence.mCanonicalValue, "0.123456784");
    EXPECT_EQ(evidence.mDiscardedDigits, "400");
    EXPECT_FALSE(evidence.mRoundedAwayFromZero);
    EXPECT_EQ(evidence.mSource,
              (SourceReference{.mBroker = Broker::TradeRepublic,
                               .mFilename = SourceFilename::fromPath("second.csv"),
                               .mSourceRow = 2,
                               .mTransactionId = "synthetic-future",
                               .mInputSequence = {1, 0}}));
    EXPECT_EQ(transaction.mMetadata.mSources,
              (std::vector<SourceReference>{{.mBroker = Broker::TradeRepublic,
                                             .mFilename = SourceFilename::fromPath("first.csv"),
                                             .mSourceRow = 2,
                                             .mTransactionId = "synthetic-future",
                                             .mInputSequence = {0, 0}},
                                            {.mBroker = Broker::TradeRepublic,
                                             .mFilename = SourceFilename::fromPath("second.csv"),
                                             .mSourceRow = 2,
                                             .mTransactionId = "synthetic-future",
                                             .mInputSequence = {1, 0}}}));
}

TEST(ExactArithmeticIntegrationTest, CorporateActionEvidenceSurvivesEquivalentOverlappingExports) {
    const TemporaryCsvInputs files;
    const auto firstFile = files.write("first.csv",
                                       {.mDatetime = "2025-01-01T12:00:00.000Z",
                                        .mDate = "2025-01-01",
                                        .mAccountType = "DEFAULT",
                                        .mCategory = "CORPORATE_ACTION",
                                        .mType = "SPLIT",
                                        .mAssetClass = "STOCK",
                                        .mName = "Synthetic Merger Share",
                                        .mSymbol = "XX8000000001",
                                        .mShares = "0.123456785",
                                        .mPrice = "",
                                        .mAmount = "",
                                        .mCurrency = "",
                                        .mDescription = "Synthetic unresolved split",
                                        .mTransactionId = "synthetic-action"});
    const auto secondFile = files.write("second.csv",
                                        {.mDatetime = "2025-01-01T12:00:00.000Z",
                                         .mDate = "2025-01-01",
                                         .mAccountType = "DEFAULT",
                                         .mCategory = "CORPORATE_ACTION",
                                         .mType = "SPLIT",
                                         .mAssetClass = "STOCK",
                                         .mName = "Synthetic Merger Share",
                                         .mSymbol = "XX8000000001",
                                         .mShares = "0.1234567850",
                                         .mPrice = "",
                                         .mAmount = "",
                                         .mCurrency = "",
                                         .mDescription = "Synthetic unresolved split",
                                         .mTransactionId = "synthetic-action"});
    tr::TradeRepublicParser parser;
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(firstFile, 0)},
        StatementMergeInput{.mSourceIndex = 1, .mParseResult = parser.parse(secondFile, 1)},
    };

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 1U);
    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments.front();

    EXPECT_TRUE(instrument.mTransactions.empty());
    ASSERT_EQ(instrument.mCorporateActions.size(), 1U);

    const auto& action = instrument.mCorporateActions.front();

    // 0.123456785 rounds to 0.12345679; 0.12345679 * 100,000,000 = 12,345,679.
    EXPECT_EQ(action.mUnitsDelta, 12'345'679);
    EXPECT_EQ(action.mType, CorporateActionType::UnresolvedSplit);
    EXPECT_FALSE(action.mRatio.has_value());
    ASSERT_EQ(action.mUnitEvidence.size(), 2U);

    const auto& first = action.mUnitEvidence[0];
    const auto& second = action.mUnitEvidence[1];

    EXPECT_EQ(first.mSourceText, "0.123456785");
    EXPECT_EQ(first.mCanonicalValue, "0.123456785");
    EXPECT_EQ(first.mDiscardedDigits, "5");
    EXPECT_TRUE(first.mRoundedAwayFromZero);

    EXPECT_EQ(second.mSourceText, "0.1234567850");
    EXPECT_EQ(second.mCanonicalValue, "0.123456785");
    EXPECT_EQ(second.mDiscardedDigits, "50");
    EXPECT_TRUE(second.mRoundedAwayFromZero);

    EXPECT_EQ(action.mMetadata.mSources,
              (std::vector<SourceReference>{{.mBroker = Broker::TradeRepublic,
                                             .mFilename = SourceFilename::fromPath("first.csv"),
                                             .mSourceRow = 2,
                                             .mTransactionId = "synthetic-action",
                                             .mInputSequence = {0, 0}},
                                            {.mBroker = Broker::TradeRepublic,
                                             .mFilename = SourceFilename::fromPath("second.csv"),
                                             .mSourceRow = 2,
                                             .mTransactionId = "synthetic-action",
                                             .mInputSequence = {1, 0}}}));
    EXPECT_EQ(first.mSource, action.mMetadata.mSources[0]);
    EXPECT_EQ(second.mSource, action.mMetadata.mSources[1]);
}

TEST(ExactArithmeticIntegrationTest, NegativeSplitEvidenceDeduplicatesWithoutInferringDirection) {
    const TemporaryCsvInputs files;
    const auto firstFile = files.write("first.csv",
                                       {.mDatetime = "2025-01-01T12:00:00.000Z",
                                        .mDate = "2025-01-01",
                                        .mAccountType = "DEFAULT",
                                        .mCategory = "CORPORATE_ACTION",
                                        .mType = "SPLIT",
                                        .mAssetClass = "STOCK",
                                        .mName = "Synthetic Merger Share",
                                        .mSymbol = "XX8000000001",
                                        .mShares = "-0.123456785",
                                        .mPrice = "",
                                        .mAmount = "",
                                        .mCurrency = "",
                                        .mDescription = "Synthetic unresolved split",
                                        .mTransactionId = "synthetic-action"});
    const auto secondFile = files.write("second.csv",
                                        {.mDatetime = "2025-01-01T12:00:00.000Z",
                                         .mDate = "2025-01-01",
                                         .mAccountType = "DEFAULT",
                                         .mCategory = "CORPORATE_ACTION",
                                         .mType = "SPLIT",
                                         .mAssetClass = "STOCK",
                                         .mName = "Synthetic Merger Share",
                                         .mSymbol = "XX8000000001",
                                         .mShares = "-0.12345678500",
                                         .mPrice = "",
                                         .mAmount = "",
                                         .mCurrency = "",
                                         .mDescription = "Synthetic unresolved split",
                                         .mTransactionId = "synthetic-action"});
    tr::TradeRepublicParser parser;
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(firstFile, 0)},
        StatementMergeInput{.mSourceIndex = 1, .mParseResult = parser.parse(secondFile, 1)},
    };

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 1U);
    EXPECT_EQ(result.mStatement.mChronologicalOrder.front().mKind,
              StatementEventKind::CorporateAction);
    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments.front();

    EXPECT_TRUE(instrument.mTransactions.empty());
    ASSERT_EQ(instrument.mCorporateActions.size(), 1U);

    const auto& action = instrument.mCorporateActions.front();

    EXPECT_EQ(action.mType, CorporateActionType::UnresolvedSplit);
    // -0.123456785 rounds to -0.12345679; -0.12345679 * 100,000,000 = -12,345,679.
    EXPECT_EQ(action.mUnitsDelta, -12'345'679);
    EXPECT_FALSE(action.mRatio.has_value());
    EXPECT_EQ(action.mMetadata.mTaxDate,
              Date{std::chrono::sys_days{std::chrono::year{2025} / 1 / 1}.time_since_epoch()});
    ASSERT_EQ(action.mUnitEvidence.size(), 2U);

    const auto& first = action.mUnitEvidence[0];
    const auto& second = action.mUnitEvidence[1];

    EXPECT_EQ(first.mSourceText, "-0.123456785");
    EXPECT_EQ(first.mDiscardedDigits, "5");
    EXPECT_EQ(first.mCanonicalValue, "-0.123456785");
    EXPECT_TRUE(first.mRoundedAwayFromZero);

    EXPECT_EQ(second.mSourceText, "-0.12345678500");
    EXPECT_EQ(second.mDiscardedDigits, "500");
    EXPECT_EQ(second.mCanonicalValue, "-0.123456785");
    EXPECT_TRUE(second.mRoundedAwayFromZero);

    EXPECT_EQ(action.mMetadata.mSources,
              (std::vector<SourceReference>{{.mBroker = Broker::TradeRepublic,
                                             .mFilename = SourceFilename::fromPath("first.csv"),
                                             .mSourceRow = 2,
                                             .mTransactionId = "synthetic-action",
                                             .mInputSequence = {0, 0}},
                                            {.mBroker = Broker::TradeRepublic,
                                             .mFilename = SourceFilename::fromPath("second.csv"),
                                             .mSourceRow = 2,
                                             .mTransactionId = "synthetic-action",
                                             .mInputSequence = {1, 0}}}));
    EXPECT_EQ(first.mSource, action.mMetadata.mSources[0]);
    EXPECT_EQ(second.mSource, action.mMetadata.mSources[1]);
}

TEST(ExactArithmeticIntegrationTest, RejectsOppositeSignedSplitEvidenceWithTheSameIdentity) {
    const TemporaryCsvInputs files;
    const auto positiveFile = files.write("positive.csv",
                                          {.mDatetime = "2025-01-01T12:00:00.000Z",
                                           .mDate = "2025-01-01",
                                           .mAccountType = "DEFAULT",
                                           .mCategory = "CORPORATE_ACTION",
                                           .mType = "SPLIT",
                                           .mAssetClass = "STOCK",
                                           .mName = "Synthetic Merger Share",
                                           .mSymbol = "XX8000000001",
                                           .mShares = "0.123456785",
                                           .mPrice = "",
                                           .mAmount = "",
                                           .mCurrency = "",
                                           .mDescription = "Synthetic unresolved split",
                                           .mTransactionId = "synthetic-action"});
    const auto negativeFile = files.write("negative.csv",
                                          {.mDatetime = "2025-01-01T12:00:00.000Z",
                                           .mDate = "2025-01-01",
                                           .mAccountType = "DEFAULT",
                                           .mCategory = "CORPORATE_ACTION",
                                           .mType = "SPLIT",
                                           .mAssetClass = "STOCK",
                                           .mName = "Synthetic Merger Share",
                                           .mSymbol = "XX8000000001",
                                           .mShares = "-0.123456785",
                                           .mPrice = "",
                                           .mAmount = "",
                                           .mCurrency = "",
                                           .mDescription = "Synthetic unresolved split",
                                           .mTransactionId = "synthetic-action"});
    tr::TradeRepublicParser parser;
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(positiveFile, 0)},
        StatementMergeInput{.mSourceIndex = 1, .mParseResult = parser.parse(negativeFile, 1)},
    };

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    EXPECT_TRUE(result.mStatement.mChronologicalOrder.empty());
    EXPECT_TRUE(result.mStatement.mPresentation.mTradeInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<MergeDiagnostic>(result.mDiagnostics.front()));

    const auto& diagnostic = std::get<MergeDiagnostic>(result.mDiagnostics.front());

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, MergeDiagnosticCode::ConflictingDuplicate);
    EXPECT_EQ(diagnostic.mSources,
              (std::vector<SourceReference>{{.mBroker = Broker::TradeRepublic,
                                             .mFilename = SourceFilename::fromPath("positive.csv"),
                                             .mSourceRow = 2,
                                             .mTransactionId = "synthetic-action",
                                             .mInputSequence = {0, 0}},
                                            {.mBroker = Broker::TradeRepublic,
                                             .mFilename = SourceFilename::fromPath("negative.csv"),
                                             .mSourceRow = 2,
                                             .mTransactionId = "synthetic-action",
                                             .mInputSequence = {1, 0}}}));
}

} // namespace
