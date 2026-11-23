#include "processors/tax_processor.hpp"

#include "parsers/ibkr_parser.hpp"
#include "processors/statement_merger.hpp"
#include "taxbroker/diagnostic_messages.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// The rows that a parser leaves out on purpose (derivatives, margin interest) are no event. The
// merger keeps each of them once, and the processing stage warns once per broker with the count of
// the rows of the selected year. The fixture rows are the ones in tests/test_data/ibkr/README.md.

namespace {

using namespace taxbroker;

Date date(std::string_view aText) {
    return *parseCalendarDate(aText);
}

SourceReference
sourceOf(Broker aBroker, std::size_t aFile, std::size_t aRow, std::optional<std::string> aId) {
    return SourceReference{
        .mBroker = aBroker,
        .mFilename = SourceFilename::fromPath("synthetic.xml"),
        .mSourceRow = aRow,
        .mTransactionId = std::move(aId),
        .mInputSequence = {.mSourceIndex = aFile, .mEventIndex = aRow},
    };
}

std::filesystem::path fixtureFolder() {
    return std::filesystem::path{__FILE__}.parent_path().parent_path() / "test_data" / "ibkr";
}

StatementMergeResult mergeFixtures(const std::vector<std::string>& aFiles) {
    ibkr::IbkrParser parser;
    std::vector<StatementMergeInput> inputs;

    for (std::size_t index = 0; index < aFiles.size(); ++index)
    {
        inputs.push_back({index, parser.parseFile(fixtureFolder() / aFiles[index], index)});
    }

    return DeterministicStatementMerger{}.merge(std::move(inputs));
}

/// A merged statement of rows written in a test. Each row list is one input (file), in order.
StatementMergeResult mergeRows(std::vector<std::pair<Broker, std::vector<LeftOutRow>>> aInputs) {
    std::vector<StatementMergeInput> inputs;

    for (auto& [broker, rows] : aInputs)
    {
        inputs.push_back(
            {inputs.size(),
             ParseResult{.mBroker = broker, .mStatement = {.mLeftOutRows = std::move(rows)}}});
    }

    return DeterministicStatementMerger{}.merge(std::move(inputs));
}

std::vector<ProcessingDiagnostic> marginWarnings(StatementMergeResult aMerged, int aYear) {
    const auto result = TaxProcessor::prepareReportingYear(
        {.mReportingYear = aYear, .mMerged = std::move(aMerged)});
    std::vector<ProcessingDiagnostic> warnings;

    std::copy_if(result.mDiagnostics.begin(),
                 result.mDiagnostics.end(),
                 std::back_inserter(warnings),
                 [](const ProcessingDiagnostic& aDiagnostic) {
                     return aDiagnostic.mCode ==
                            ProcessingDiagnosticCode::MarginOrLeverageRowsLeftOut;
                 });

    return warnings;
}

std::vector<std::string> idsOf(const std::vector<LeftOutRow>& aRows) {
    std::vector<std::string> ids(aRows.size());

    std::transform(aRows.begin(), aRows.end(), ids.begin(), [](const LeftOutRow& aRow) {
        return aRow.mMetadata.mSources.front().mTransactionId.value_or("(none)");
    });

    return ids;
}

// The merger

TEST(LeftOutRowsMergeTest, TheSameFileTwiceKeepsEachRowOnceWithBothSources) {
    const auto merged = mergeFixtures({"ignored.xml", "ignored.xml"});
    const auto& rows = merged.mStatement.mLeftOutRows;

    // Option 1004, futures 1022, CFD 1023 and the margin interest 2008, in file order.
    EXPECT_EQ(idsOf(rows), (std::vector<std::string>{"1004", "1022", "1023", "2008"}));

    for (const auto& row : rows)
    {
        EXPECT_EQ(row.mReason, LeftOutReason::MarginOrLeverage);
        EXPECT_EQ(row.mMetadata.mSources.size(), 2U); // the two files
    }

    EXPECT_TRUE(merged.mStatement.mPresentation.mLeftOutRows.empty());
    EXPECT_TRUE(merged.mStatement.mPresentation.mIncomePaymentRows.empty());
}

TEST(LeftOutRowsMergeTest, RowsWithoutATransactionIdAreNeverMergedWithAnotherRow) {
    const auto merged = mergeRows({
        {Broker::InteractiveBrokers,
         {{.mMetadata = {.mTaxDate = date("2024-04-09"),
                         .mSources = {sourceOf(Broker::InteractiveBrokers, 0, 4, std::nullopt)}},
           .mReason = LeftOutReason::MarginOrLeverage}}},
        {Broker::InteractiveBrokers,
         {{.mMetadata = {.mTaxDate = date("2024-04-09"),
                         .mSources = {sourceOf(Broker::InteractiveBrokers, 1, 4, std::nullopt)}},
           .mReason = LeftOutReason::MarginOrLeverage}}},
    });

    EXPECT_EQ(merged.mStatement.mLeftOutRows.size(), 2U);
}

TEST(LeftOutRowsMergeTest, TheSameIdOfTwoBrokersAreTwoRows) {
    const auto merged = mergeRows({
        {Broker::InteractiveBrokers,
         {{.mMetadata = {.mTaxDate = date("2024-04-09"),
                         .mSources = {sourceOf(Broker::InteractiveBrokers, 0, 4, "1004")}},
           .mReason = LeftOutReason::MarginOrLeverage}}},
        {Broker::TradeRepublic,
         {{.mMetadata = {.mTaxDate = date("2024-04-09"),
                         .mSources = {sourceOf(Broker::TradeRepublic, 1, 4, "1004")}},
           .mReason = LeftOutReason::MarginOrLeverage}}},
    });

    EXPECT_EQ(merged.mStatement.mLeftOutRows.size(), 2U);
}

TEST(LeftOutRowsMergeTest, ARowWithTheSourceOfAnotherFileIsRefused) {
    const auto merged = mergeRows({
        {Broker::InteractiveBrokers,
         {{.mMetadata = {.mTaxDate = date("2024-04-09"),
                         .mSources = {sourceOf(Broker::InteractiveBrokers, 5, 4, "1004")}},
           .mReason = LeftOutReason::MarginOrLeverage}}},
    });

    // The input is file 0, but the row says file 5.
    EXPECT_TRUE(merged.mStatement.mLeftOutRows.empty());

    std::size_t errors = 0;

    for (const auto& diagnostic : merged.mDiagnostics)
    {
        const auto* merge = std::get_if<MergeDiagnostic>(&diagnostic);

        if (merge != nullptr && merge->mCode == MergeDiagnosticCode::InconsistentSourceIndex)
        {
            ++errors;
        }
    }

    EXPECT_EQ(errors, 1U);
}

// The warning of the processing stage

TEST(MarginOrLeverageWarningTest, OverlappingFilesCountEachRowOnce) {
    const auto warnings = marginWarnings(mergeFixtures({"ignored.xml", "ignored.xml"}), 2024);

    ASSERT_EQ(warnings.size(), 1U);

    const auto& warning = warnings.front();

    EXPECT_EQ(warning.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(warning.mMessageKey, MessageKey::MarginOrLeverageRowsLeftOut);
    EXPECT_EQ(warning.mMessageParams,
              (std::vector<MessageParam>{numberParam("count", 4)})); // not 8
    EXPECT_TRUE(warning.mContext.mAffectedReports.empty());
    ASSERT_EQ(warning.mContext.mSources.size(), 1U);
    EXPECT_EQ(warning.mContext.mSources.front().mBroker, Broker::InteractiveBrokers);
}

TEST(MarginOrLeverageWarningTest, RowsOfAnotherYearGiveNoWarning) {
    EXPECT_TRUE(marginWarnings(mergeFixtures({"ignored.xml"}), 2023).empty());
}

TEST(MarginOrLeverageWarningTest, TheLastAndTheFirstDayOfTheYearCountAndTheDaysAroundItDoNot) {
    const auto merged = mergeRows({
        {Broker::InteractiveBrokers,
         {{.mMetadata = {.mTaxDate = date("2023-12-31"),
                         .mSources = {sourceOf(Broker::InteractiveBrokers, 0, 4, "1")}},
           .mReason = LeftOutReason::MarginOrLeverage},
          {.mMetadata = {.mTaxDate = date("2024-01-01"),
                         .mSources = {sourceOf(Broker::InteractiveBrokers, 0, 5, "2")}},
           .mReason = LeftOutReason::MarginOrLeverage},
          {.mMetadata = {.mTaxDate = date("2024-12-31"),
                         .mSources = {sourceOf(Broker::InteractiveBrokers, 0, 6, "3")}},
           .mReason = LeftOutReason::MarginOrLeverage},
          {.mMetadata = {.mTaxDate = date("2025-01-01"),
                         .mSources = {sourceOf(Broker::InteractiveBrokers, 0, 7, "4")}},
           .mReason = LeftOutReason::MarginOrLeverage}}},
    });

    const auto warnings = marginWarnings(merged, 2024);

    ASSERT_EQ(warnings.size(), 1U);
    EXPECT_EQ(warnings.front().mMessageParams,
              (std::vector<MessageParam>{numberParam("count", 2)}));
}

TEST(MarginOrLeverageWarningTest, ARowWhoseDateCouldNotBeReadCountsInEveryYear) {
    const auto merged = mergeRows({
        {Broker::InteractiveBrokers,
         {{.mMetadata = {.mTaxDate = Date{}, // 1970-01-01: the date was not readable
                         .mSources = {sourceOf(Broker::InteractiveBrokers, 0, 4, "1")}},
           .mReason = LeftOutReason::MarginOrLeverage}}},
    });

    const auto warnings = marginWarnings(merged, 2024);

    ASSERT_EQ(warnings.size(), 1U);
    EXPECT_EQ(warnings.front().mMessageParams,
              (std::vector<MessageParam>{numberParam("count", 1)}));
}

TEST(MarginOrLeverageWarningTest, TwoRowsWithoutATransactionIdAreCountedAsTwo) {
    const auto merged = mergeRows({
        {Broker::InteractiveBrokers,
         {{.mMetadata = {.mTaxDate = date("2024-04-09"),
                         .mSources = {sourceOf(Broker::InteractiveBrokers, 0, 4, std::nullopt)}},
           .mReason = LeftOutReason::MarginOrLeverage}}},
        {Broker::InteractiveBrokers,
         {{.mMetadata = {.mTaxDate = date("2024-04-09"),
                         .mSources = {sourceOf(Broker::InteractiveBrokers, 1, 4, std::nullopt)}},
           .mReason = LeftOutReason::MarginOrLeverage}}},
    });

    const auto warnings = marginWarnings(merged, 2024);

    ASSERT_EQ(warnings.size(), 1U);
    EXPECT_EQ(warnings.front().mMessageParams,
              (std::vector<MessageParam>{numberParam("count", 2)}));
}

TEST(MarginOrLeverageWarningTest, EachBrokerGetsItsOwnWarning) {
    const auto merged = mergeRows({
        {Broker::InteractiveBrokers,
         {{.mMetadata = {.mTaxDate = date("2024-04-09"),
                         .mSources = {sourceOf(Broker::InteractiveBrokers, 0, 4, "1")}},
           .mReason = LeftOutReason::MarginOrLeverage}}},
        {Broker::TradeRepublic,
         {{.mMetadata = {.mTaxDate = date("2024-07-10"),
                         .mSources = {sourceOf(Broker::TradeRepublic, 1, 9, "1")}},
           .mReason = LeftOutReason::MarginOrLeverage}}},
    });

    const auto warnings = marginWarnings(merged, 2024);

    ASSERT_EQ(warnings.size(), 2U);
    // The broker order: Trade Republic, then Interactive Brokers.
    EXPECT_EQ(warnings.at(0).mContext.mSources.front().mBroker, Broker::TradeRepublic);
    EXPECT_EQ(warnings.at(0).mMessageParams, (std::vector<MessageParam>{numberParam("count", 1)}));
    EXPECT_EQ(warnings.at(1).mContext.mSources.front().mBroker, Broker::InteractiveBrokers);
    EXPECT_EQ(warnings.at(1).mMessageParams, (std::vector<MessageParam>{numberParam("count", 1)}));
}

TEST(MarginOrLeverageWarningTest, NoLeftOutRowsGiveNoWarning) {
    EXPECT_TRUE(marginWarnings(mergeFixtures({"supported.xml"}), 2024).empty());
}

} // namespace
