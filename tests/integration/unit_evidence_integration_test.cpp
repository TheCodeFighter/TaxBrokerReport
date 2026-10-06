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

class QuantityInputs {
  public:
    QuantityInputs() {
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
        mDirectory = std::filesystem::temp_directory_path() /
                     ("taxbroker_exact_synthetic_" + std::to_string(token));

        if (!std::filesystem::create_directory(mDirectory))
        {
            throw std::runtime_error{"Unable to create synthetic quantity directory"};
        }

        const auto fixture = std::filesystem::path{__FILE__}.parent_path().parent_path() /
                             "test_data/csv/merger/history.csv";
        std::ifstream source{fixture};

        if (!std::getline(source, mHeader) || !std::getline(source, mRow))
        {
            throw std::runtime_error{"Unable to read synthetic quantity fixture"};
        }

        // Quantity tests need consistent dates, independent of the chronology fixture's warning.
        mRow.replace(1, 10, "2025-01-01");
    }

    QuantityInputs(const QuantityInputs&) = delete;
    QuantityInputs& operator=(const QuantityInputs&) = delete;

    ~QuantityInputs() {
        std::error_code error;
        std::filesystem::remove_all(mDirectory, error);
    }

    StatementMergeInput parse(std::string_view aName,
                              std::string_view aQuantity,
                              std::size_t aSourceIndex,
                              bool aAction = false) const {
        auto row = mRow;
        const auto position = row.find("1.0000000000");
        row.replace(position, std::string_view{"1.0000000000"}.size(), aQuantity);

        if (aAction)
        {
            const auto tradePosition = row.find("\"TRADING\",\"BUY\"");
            row.replace(tradePosition,
                        std::string_view{"\"TRADING\",\"BUY\""}.size(),
                        "\"CORPORATE_ACTION\",\"SPLIT\"");
        }

        const auto path = mDirectory / aName;
        std::ofstream output{path};
        output << mHeader << '\n' << row << '\n';
        output.close();
        tr::TradeRepublicParser parser;

        return StatementMergeInput{aSourceIndex, parser.parse(path, aSourceIndex)};
    }

  private:
    std::filesystem::path mDirectory;
    std::string mHeader;
    std::string mRow;
};

TEST(ExactArithmeticIntegrationTest, RetainsEvidenceAcrossOverlapsAndParserCompletionOrders) {
    const QuantityInputs files;
    std::array inputs{files.parse("first.csv", "0.123456784", 0),
                      files.parse("second.csv", "0.12345678400", 1)};
    const DeterministicStatementMerger merger;
    const auto forward = merger.merge(inputs);
    std::reverse(inputs.begin(), inputs.end());
    const auto reversed = merger.merge(inputs);
    const auto& transaction =
        forward.mStatement.mPresentation.mTradeInstruments.at(0).mTransactions.at(0);

    ASSERT_EQ(transaction.mUnitEvidence.size(), 2U);
    EXPECT_EQ(transaction.mUnits, 12'345'678);
    EXPECT_EQ(transaction.mUnitEvidence[0].mSourceText, "0.123456784");
    EXPECT_EQ(transaction.mUnitEvidence[1].mDiscardedDigits, "400");
    EXPECT_EQ(transaction.mUnitEvidence[0].mSource.mInputSequence.mSourceIndex, 0U);
    EXPECT_EQ(transaction.mUnitEvidence[1].mSource.mInputSequence.mSourceIndex, 1U);
    EXPECT_TRUE(forward.mDiagnostics.empty());
    test::expectStatementsEqual(forward.mStatement.mPresentation,
                                reversed.mStatement.mPresentation);
    EXPECT_EQ(forward.mStatement.mChronologicalOrder, reversed.mStatement.mChronologicalOrder);
}

TEST(ExactArithmeticIntegrationTest, RejectsDistinctExactSourceFactsThatRoundToTheSameUnits) {
    const QuantityInputs files;
    const DeterministicStatementMerger merger;

    for (const bool action : {false, true})
    {
        const std::array inputs{files.parse("first.csv", "0.123456784", 0, action),
                                files.parse("second.csv", "0.123456783", 1, action)};
        const auto merged = merger.merge(inputs);

        EXPECT_TRUE(merged.mStatement.mChronologicalOrder.empty());
        ASSERT_EQ(merged.mDiagnostics.size(), 1U);
        const auto& diagnostic = std::get<MergeDiagnostic>(merged.mDiagnostics[0]);

        EXPECT_EQ(diagnostic.mCode, MergeDiagnosticCode::ConflictingDuplicate);
        EXPECT_EQ(diagnostic.mSources.size(), 2U);
        EXPECT_EQ(diagnostic.mMessage.find("0.123"), std::string::npos);
    }
}

TEST(ExactArithmeticIntegrationTest,
     MissingEvidenceDoesNotHideKnownSourceConflictsOrFabricateProof) {
    const QuantityInputs files;
    std::array inputs{files.parse("first.csv", "0.123456784", 0),
                      files.parse("second.csv", "0.12345678400", 1),
                      files.parse("third.csv", "0.123456783", 2)};
    inputs[0].mParseResult.mStatement.mTradeInstruments[0].mTransactions[0].mUnitEvidence.clear();
    const DeterministicStatementMerger merger;
    const auto conflicting = merger.merge(inputs);

    EXPECT_TRUE(conflicting.mStatement.mChronologicalOrder.empty());
    ASSERT_EQ(conflicting.mDiagnostics.size(), 1U);
    EXPECT_EQ(std::get<MergeDiagnostic>(conflicting.mDiagnostics[0]).mCode,
              MergeDiagnosticCode::ConflictingDuplicate);
    const auto merged = merger.merge(std::span<const StatementMergeInput>{inputs.data(), 2});
    const auto& transaction =
        merged.mStatement.mPresentation.mTradeInstruments.at(0).mTransactions.at(0);

    ASSERT_EQ(transaction.mUnitEvidence.size(), 1U);
    EXPECT_EQ(transaction.mUnitEvidence[0].mSource.mInputSequence.mSourceIndex, 1U);
    EXPECT_EQ(transaction.mMetadata.mSources.size(), 2U);
}

TEST(ExactArithmeticIntegrationTest, CorporateActionEvidenceSurvivesEquivalentOverlappingExports) {
    const QuantityInputs files;
    const std::array inputs{files.parse("first.csv", "0.123456785", 0, true),
                            files.parse("second.csv", "0.1234567850", 1, true)};
    const auto merged = DeterministicStatementMerger{}.merge(inputs);
    const auto& action =
        merged.mStatement.mPresentation.mTradeInstruments.at(0).mCorporateActions.at(0);

    EXPECT_EQ(action.mUnitsDelta, 12'345'679);
    ASSERT_EQ(action.mUnitEvidence.size(), 2U);
    EXPECT_EQ(action.mUnitEvidence[1].mDiscardedDigits, "50");
    EXPECT_EQ(action.mType, CorporateActionType::UnresolvedSplit);
    EXPECT_FALSE(action.mRatio.has_value());
    EXPECT_TRUE(merged.mDiagnostics.empty());
}

} // namespace
