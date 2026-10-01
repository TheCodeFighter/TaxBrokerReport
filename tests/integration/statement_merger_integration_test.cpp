#include "../support/statement_merge_assertions.hpp"
#include "parsers/traderepublic_parser.hpp"
#include "utils/date_utils.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {
using namespace taxbroker;

std::filesystem::path fixturePath(std::string_view aFilename) {
    return std::filesystem::path{__FILE__}.parent_path().parent_path() / "test_data" / "csv" /
           "merger" / aFilename;
}

class TemporaryHistory {
  public:
    TemporaryHistory() {
        std::ifstream input{fixturePath("history.csv")};

        if (!input || !std::getline(input, mHeader))
        {
            throw std::runtime_error{"Unable to read synthetic merger fixture"};
        }

        std::string row;

        while (std::getline(input, row))
        {
            mRows.push_back(row);
        }

        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();

        for (unsigned attempt = 0; attempt < 100; ++attempt)
        {
            const auto candidate =
                std::filesystem::temp_directory_path() /
                ("taxbroker_merger_" + std::to_string(token) + "_" + std::to_string(attempt));

            if (std::filesystem::create_directory(candidate))
            {
                mDirectory = candidate;

                return;
            }
        }

        throw std::runtime_error{"Unable to create synthetic merger directory"};
    }

    TemporaryHistory(const TemporaryHistory&) = delete;
    TemporaryHistory& operator=(const TemporaryHistory&) = delete;

    ~TemporaryHistory() {
        std::error_code error;
        std::filesystem::remove_all(mDirectory, error);
    }

    std::filesystem::path
    writeRows(std::string_view aFilename,
              std::initializer_list<std::size_t> aRows,
              std::optional<std::string_view> aTimestamp = std::nullopt) const {
        const auto path = mDirectory / aFilename;
        std::ofstream output{path};

        output << mHeader << '\n';

        for (const auto index : aRows)
        {
            const auto& row = mRows.at(index);

            if (aTimestamp)
            {
                output << '"' << *aTimestamp << '"' << row.substr(row.find(',')) << '\n';
            }
            else
            {
                output << row << '\n';
            }
        }

        if (!output)
        {
            throw std::runtime_error{"Unable to write synthetic merger input"};
        }

        return path;
    }

  private:
    std::filesystem::path mDirectory;
    std::string mHeader;
    std::vector<std::string> mRows;
};

std::vector<StatementMergeInput> parseInOrder(const std::array<std::filesystem::path, 3>& aFiles,
                                              const std::array<std::size_t, 3>& aOrder) {
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs(aOrder.size());
    const auto parseInput = [&parser, &aFiles](std::size_t aSourceIndex) {
        return StatementMergeInput{.mSourceIndex = aSourceIndex,
                                   .mParseResult =
                                       parser.parse(aFiles[aSourceIndex], aSourceIndex)};
    };

    std::transform(aOrder.begin(), aOrder.end(), inputs.begin(), parseInput);

    return inputs;
}

std::vector<std::string> ledgerIds(const StatementMergeResult& aResult) {
    std::vector<std::string> ids;

    for (const auto& reference : aResult.mStatement.mChronologicalOrder)
    {
        const bool standalone = reference.mKind == StatementEventKind::Benefit ||
                                reference.mKind == StatementEventKind::PrivateMarket;

        EXPECT_EQ(reference.mInstrumentIndex.has_value(), !standalone);
        EXPECT_EQ(std::count(aResult.mStatement.mChronologicalOrder.begin(),
                             aResult.mStatement.mChronologicalOrder.end(),
                             reference),
                  1);

        const auto& metadata = test::referencedMetadata(aResult.mStatement, reference);

        EXPECT_FALSE(metadata.mSources.empty());
        ids.push_back(primarySource(metadata).mTransactionId.value_or(""));
    }

    return ids;
}

SourceReference expectedSource(std::size_t aSource, std::size_t aEvent, std::string_view aId) {
    return SourceReference{.mBroker = Broker::TradeRepublic,
                           .mFilename =
                               SourceFilename::fromPath("part-" + std::to_string(aSource) + ".csv"),
                           .mSourceRow = aEvent + 2,
                           .mTransactionId = std::string{aId},
                           .mInputSequence = {aSource, aEvent}};
}

TEST(StatementMergerIntegrationTest,
     DisjointFilesMatchWholeHistoryWithoutChangingTaxDatesOrValues) {
    const TemporaryHistory history;
    const std::array files{history.writeRows("part-0.csv", {0, 1, 2, 3}),
                           history.writeRows("part-1.csv", {4, 5, 6}),
                           history.writeRows("part-2.csv", {7, 8, 9})};
    const StatementMergeInput whole{
        .mSourceIndex = 0,
        .mParseResult = tr::TradeRepublicParser{}.parse(fixturePath("history.csv"), 0)};
    const auto expected = DeterministicStatementMerger{}.merge(std::span{&whole, 1U});
    const std::vector<std::string> ids{"synthetic-older",
                                       "synthetic-dividend",
                                       "synthetic-interest",
                                       "synthetic-tied",
                                       "synthetic-benefit",
                                       "synthetic-action",
                                       "synthetic-private",
                                       "",
                                       "synthetic-future"};
    const std::array<std::pair<std::size_t, std::size_t>, 9> sourcePositions{
        {{0, 1}, {1, 0}, {1, 1}, {0, 2}, {2, 0}, {1, 2}, {2, 1}, {2, 2}, {0, 0}}};
    std::array<std::size_t, 3> order{0, 1, 2};

    ASSERT_TRUE(whole.mParseResult.mDiagnostics.empty());
    EXPECT_EQ(ledgerIds(expected), ids);

    do
    {
        const auto inputs = parseInOrder(files, order);
        const auto result = DeterministicStatementMerger{}.merge(inputs);

        test::expectStatementsEqual(result.mStatement.mPresentation,
                                    expected.mStatement.mPresentation,
                                    false);
        EXPECT_EQ(result.mStatement.mChronologicalOrder, expected.mStatement.mChronologicalOrder);
        EXPECT_EQ(ledgerIds(result), ids);
        EXPECT_TRUE(result.mDiagnostics.empty());

        ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), sourcePositions.size());

        for (std::size_t event = 0; event < sourcePositions.size(); ++event)
        {
            const auto [source, position] = sourcePositions[event];
            auto provenance = expectedSource(source, position, ids[event]);

            if (ids[event].empty())
            {
                provenance.mTransactionId.reset();
            }

            EXPECT_EQ(test::referencedMetadata(result.mStatement,
                                               result.mStatement.mChronologicalOrder[event])
                          .mSources,
                      std::vector{provenance});
        }

        const auto& trades = result.mStatement.mPresentation.mTradeInstruments.at(0).mTransactions;

        ASSERT_EQ(trades.size(), 4U);
        EXPECT_EQ(trades[0].mMetadata.mTaxDate, *parseCalendarDate("2023-12-31"));
        EXPECT_EQ(trades[3].mMetadata.mTaxDate, *parseCalendarDate("2025-01-01"));
        EXPECT_EQ(trades[0].mMetadata.mSourceTimestamp,
                  parseSourceTimestamp("2023-12-31T22:30:00Z"));
        EXPECT_EQ(trades[3].mMetadata.mSourceTimestamp,
                  parseSourceTimestamp("2023-01-01T08:00:00Z"));
        EXPECT_EQ(trades[1].mUnits, 125000000);
        EXPECT_EQ(trades[1].mUnitPrice, 100000);
        EXPECT_EQ(trades[1].mAmount, 125000);
        EXPECT_EQ(trades[1].mFeePaid, 2500);
        EXPECT_EQ(trades[1].mMetadata.mSources,
                  std::vector{expectedSource(0, 2, "synthetic-tied")});

        const auto& presentation = result.mStatement.mPresentation;

        EXPECT_EQ(presentation.mDividendInstruments.at(0).mTransactions.at(0).mGrossAmount, 123400);
        EXPECT_EQ(presentation.mDividendInstruments.at(0).mTransactions.at(0).mTaxPaid, 23400);
        EXPECT_EQ(presentation.mInterestInstruments.at(0).mTransactions.at(0).mGrossAmount, 12300);
        EXPECT_EQ(presentation.mBenefitEvents.at(0).mAmount, 25000);
        EXPECT_EQ(presentation.mPrivateMarketEvents.at(0).mAmount, -250000);
        EXPECT_EQ(presentation.mPrivateMarketEvents.at(0).mFeePaid, 5000);
        EXPECT_EQ(presentation.mTradeInstruments.at(0).mCorporateActions.at(0).mUnitsDelta,
                  200000000);
        EXPECT_FALSE(
            presentation.mTradeInstruments.at(0).mCorporateActions.at(0).mRatio.has_value());
    } while (std::next_permutation(order.begin(), order.end()));
}

TEST(StatementMergerIntegrationTest,
     OverlappingFilesPreserveAllSixKindsAndEverySourceButKeepIdlessRows) {
    const TemporaryHistory history;
    const std::array files{history.writeRows("part-0.csv", {1, 2, 3, 4, 5, 6, 7, 8, 9}),
                           history.writeRows("part-1.csv", {0, 2, 4, 5, 6, 7, 8, 9}),
                           history.writeRows("part-2.csv", {2, 4, 5, 6, 7, 8, 9})};
    std::array<std::size_t, 3> order{0, 1, 2};
    const auto expected = DeterministicStatementMerger{}.merge(parseInOrder(files, order));
    const std::array ids{"synthetic-tied",
                         "synthetic-dividend",
                         "synthetic-interest",
                         "synthetic-action",
                         "synthetic-benefit",
                         "synthetic-private"};
    const std::array<std::size_t, 6> firstEventIndices{1, 3, 4, 5, 6, 7};
    const std::vector<std::string> expectedIds{"synthetic-older",
                                               "synthetic-dividend",
                                               "synthetic-interest",
                                               "synthetic-tied",
                                               "synthetic-benefit",
                                               "synthetic-action",
                                               "synthetic-private",
                                               "",
                                               "",
                                               "",
                                               "synthetic-future"};

    do
    {
        const auto inputs = parseInOrder(files, order);
        const auto result = DeterministicStatementMerger{}.merge(inputs);

        test::expectMergeResultsEqual(result, expected);
        EXPECT_EQ(ledgerIds(result), expectedIds);
        EXPECT_TRUE(result.mDiagnostics.empty());

        for (const auto& reference : result.mStatement.mChronologicalOrder)
        {
            const auto& metadata = test::referencedMetadata(result.mStatement, reference);
            const auto id = primarySource(metadata).mTransactionId.value_or("");
            const auto found = std::find(ids.begin(), ids.end(), id);

            if (found != ids.end())
            {
                const auto index = static_cast<std::size_t>(found - ids.begin());
                const std::vector sources{expectedSource(0, firstEventIndices[index], id),
                                          expectedSource(1, index + 1, id),
                                          expectedSource(2, index, id)};

                EXPECT_EQ(metadata.mSources, sources);
            }
            else
            {
                ASSERT_EQ(metadata.mSources.size(), 1U);

                if (id.empty())
                {
                    const auto source = metadata.mSources.front().mInputSequence.mSourceIndex;
                    const std::array<std::size_t, 3> indices{8, 7, 6};
                    auto expectedReference = expectedSource(source, indices[source], "");
                    expectedReference.mTransactionId.reset();

                    EXPECT_EQ(metadata.mSources.front(), expectedReference);
                }
            }
        }
    } while (std::next_permutation(order.begin(), order.end()));
}

TEST(StatementMergerIntegrationTest,
     ConflictsRejectEveryCopyAndKeepPartialParseAndInstrumentDiagnostics) {
    const std::array files{fixturePath("history.csv"),
                           fixturePath("conflict.csv"),
                           fixturePath("partial.csv")};
    std::array<std::size_t, 3> order{0, 1, 2};
    const auto expected = DeterministicStatementMerger{}.merge(parseInOrder(files, order));

    do
    {
        const auto inputs = parseInOrder(files, order);
        const auto result = DeterministicStatementMerger{}.merge(inputs);

        test::expectMergeResultsEqual(result, expected);
        const auto ids = ledgerIds(result);

        EXPECT_EQ(ids.size(), 9U);
        EXPECT_EQ(std::count(ids.begin(), ids.end(), "synthetic-tied"), 0);
        EXPECT_EQ(std::count(ids.begin(), ids.end(), "synthetic-rejected"), 0);
        EXPECT_EQ(std::count(ids.begin(), ids.end(), "synthetic-class"), 1);
        ASSERT_EQ(result.mDiagnostics.size(), 4U);

        const auto& parser = std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]);
        const auto partial = std::find_if(inputs.begin(), inputs.end(), [](const auto& aInput) {
            return aInput.mSourceIndex == 2;
        });

        ASSERT_EQ(partial->mParseResult.mDiagnostics.size(), 1U);
        test::expectDiagnosticsEqual(
            {result.mDiagnostics[0]},
            {SourcedParseDiagnostic{.mSourceIndex = 2,
                                    .mBroker = partial->mParseResult.mBroker,
                                    .mDiagnostic = partial->mParseResult.mDiagnostics[0]}});

        EXPECT_EQ(parser.mSourceIndex, 2U);
        EXPECT_EQ(parser.mBroker, Broker::TradeRepublic);
        EXPECT_EQ(parser.mDiagnostic.mSeverity, DiagnosticSeverity::Error);
        EXPECT_EQ(parser.mDiagnostic.mCode, DiagnosticCode::InvalidValue);
        EXPECT_EQ(parser.mDiagnostic.mSourceFile, "partial.csv");
        EXPECT_EQ(parser.mDiagnostic.mRowIndex, 2U);
        EXPECT_EQ(parser.mDiagnostic.mTransactionId, "synthetic-rejected");
        EXPECT_EQ(parser.mDiagnostic.mField, "price");

        const auto& classConflict = std::get<MergeDiagnostic>(result.mDiagnostics[1]);
        const auto& nameConflict = std::get<MergeDiagnostic>(result.mDiagnostics[2]);
        const auto& transactionConflict = std::get<MergeDiagnostic>(result.mDiagnostics[3]);

        EXPECT_EQ(classConflict.mCode, MergeDiagnosticCode::InstrumentAssetClassConflict);
        EXPECT_EQ(classConflict.mSeverity, DiagnosticSeverity::Error);
        EXPECT_EQ(nameConflict.mCode, MergeDiagnosticCode::InstrumentNameConflict);
        EXPECT_EQ(nameConflict.mSeverity, DiagnosticSeverity::Warning);
        EXPECT_EQ(transactionConflict.mCode, MergeDiagnosticCode::ConflictingDuplicate);
        EXPECT_EQ(transactionConflict.mEventKinds, std::vector{StatementEventKind::Trade});

        ASSERT_EQ(transactionConflict.mSources.size(), 2U);
        EXPECT_EQ(transactionConflict.mSources[0].mInputSequence, (StableInputSequence{0, 2}));
        EXPECT_EQ(transactionConflict.mSources[0].mSourceRow, 4U);
        EXPECT_EQ(transactionConflict.mSources[1].mInputSequence, (StableInputSequence{1, 0}));
        EXPECT_EQ(transactionConflict.mSources[1].mSourceRow, 2U);
        EXPECT_EQ(result.mStatement.mPresentation.mTradeInstruments.at(0).mName,
                  "Synthetic Merger Share");
        EXPECT_EQ(result.mStatement.mPresentation.mTradeInstruments.at(0).mAssetClass,
                  AssetClass::Unknown);

        const auto& trades = result.mStatement.mPresentation.mTradeInstruments.at(0).mTransactions;
        const auto validPartialRow =
            std::find_if(trades.begin(), trades.end(), [](const auto& aTrade) {
                return primarySource(aTrade.mMetadata).mTransactionId == "synthetic-class";
            });

        ASSERT_NE(validPartialRow, trades.end());
        EXPECT_EQ(validPartialRow->mMetadata.mSources,
                  (std::vector{SourceReference{.mBroker = Broker::TradeRepublic,
                                               .mFilename = SourceFilename::fromPath("partial.csv"),
                                               .mSourceRow = 3,
                                               .mTransactionId = "synthetic-class",
                                               .mInputSequence = {2, 1}}}));
    } while (std::next_permutation(order.begin(), order.end()));
}

TEST(StatementMergerIntegrationTest, EmptyAndFailedFilesPreserveHealthyDataAndAllFailures) {
    const TemporaryHistory history;
    const auto empty = history.writeRows("empty.csv", {});
    const std::array files{empty, fixturePath("invalid.csv"), fixturePath("history.csv")};
    std::array<std::size_t, 3> order{0, 1, 2};
    const auto expected = DeterministicStatementMerger{}.merge(parseInOrder(files, order));

    do
    {
        const auto inputs = parseInOrder(files, order);
        const auto result = DeterministicStatementMerger{}.merge(inputs);

        test::expectMergeResultsEqual(result, expected);
        EXPECT_EQ(ledgerIds(result).size(), 9U);
        ASSERT_EQ(result.mDiagnostics.size(), 1U);

        const auto& diagnostic = std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]);

        EXPECT_EQ(diagnostic.mSourceIndex, 1U);
        EXPECT_EQ(diagnostic.mDiagnostic.mCode, DiagnosticCode::ParseError);
        EXPECT_EQ(diagnostic.mDiagnostic.mSourceFile, "invalid.csv");
        EXPECT_FALSE(diagnostic.mDiagnostic.mRowIndex.has_value());
    } while (std::next_permutation(order.begin(), order.end()));

    const std::array failedFiles{empty,
                                 fixturePath("invalid.csv"),
                                 empty.parent_path() / "missing.csv"};
    const auto failed = DeterministicStatementMerger{}.merge(parseInOrder(failedFiles, {2, 0, 1}));

    EXPECT_TRUE(failed.mStatement.mChronologicalOrder.empty());
    test::expectStatementsEqual(failed.mStatement.mPresentation, BrokerStatement{});
    ASSERT_EQ(failed.mDiagnostics.size(), 2U);
    EXPECT_EQ(std::get<SourcedParseDiagnostic>(failed.mDiagnostics[0]).mSourceIndex, 1U);
    EXPECT_EQ(std::get<SourcedParseDiagnostic>(failed.mDiagnostics[1]).mSourceIndex, 2U);
    EXPECT_EQ(std::get<SourcedParseDiagnostic>(failed.mDiagnostics[1]).mDiagnostic.mSourceFile,
              "missing.csv");

    const std::array emptyFiles{empty, empty, empty};
    const auto allEmpty = DeterministicStatementMerger{}.merge(parseInOrder(emptyFiles, {2, 1, 0}));

    test::expectStatementsEqual(allEmpty.mStatement.mPresentation, BrokerStatement{});
    EXPECT_TRUE(allEmpty.mStatement.mChronologicalOrder.empty());
    EXPECT_TRUE(allEmpty.mDiagnostics.empty());
}

TEST(StatementMergerIntegrationTest, RequestOrderChangesEqualAndMissingTimestampTiesButNotDates) {
    const TemporaryHistory history;

    for (const auto timestamp : {"2024-01-02T09:00:00.000Z", ""})
    {
        SCOPED_TRACE(timestamp);
        const auto trade = history.writeRows("trade.csv", {2}, timestamp);
        const auto benefit = history.writeRows("benefit.csv", {7}, timestamp);
        const auto older = history.writeRows("older.csv", {1});
        const std::array firstFiles{trade, benefit, older};
        const std::array reversedFiles{benefit, trade, older};
        const auto first =
            DeterministicStatementMerger{}.merge(parseInOrder(firstFiles, {2, 1, 0}));
        const auto reversed =
            DeterministicStatementMerger{}.merge(parseInOrder(reversedFiles, {1, 2, 0}));

        EXPECT_EQ(
            ledgerIds(first),
            (std::vector<std::string>{"synthetic-older", "synthetic-tied", "synthetic-benefit"}));
        EXPECT_EQ(
            ledgerIds(reversed),
            (std::vector<std::string>{"synthetic-older", "synthetic-benefit", "synthetic-tied"}));
        test::expectStatementsEqual(first.mStatement.mPresentation,
                                    reversed.mStatement.mPresentation,
                                    false);
        EXPECT_TRUE(first.mDiagnostics.empty());
        EXPECT_TRUE(reversed.mDiagnostics.empty());
    }
}

TEST(StatementMergerIntegrationTest, InvalidAndMissingTimestampsKeepEventsAndParserWarnings) {
    const TemporaryHistory history;
    const std::array files{history.writeRows("invalid-time.csv", {2}, "invalid-time"),
                           history.writeRows("missing-time.csv", {7}, ""),
                           history.writeRows("timed.csv", {5})};
    std::array<std::size_t, 3> order{0, 1, 2};
    const auto expected = DeterministicStatementMerger{}.merge(parseInOrder(files, order));

    do
    {
        const auto inputs = parseInOrder(files, order);
        const auto result = DeterministicStatementMerger{}.merge(inputs);

        test::expectMergeResultsEqual(result, expected);
        EXPECT_EQ(ledgerIds(result),
                  (std::vector<std::string>{"synthetic-interest",
                                            "synthetic-tied",
                                            "synthetic-benefit"}));
        ASSERT_EQ(result.mDiagnostics.size(), 1U);

        const auto& diagnostic = std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]);
        const auto invalid = std::find_if(inputs.begin(), inputs.end(), [](const auto& aInput) {
            return aInput.mSourceIndex == 0;
        });

        ASSERT_EQ(invalid->mParseResult.mDiagnostics.size(), 1U);
        test::expectDiagnosticsEqual(
            {result.mDiagnostics[0]},
            {SourcedParseDiagnostic{.mSourceIndex = 0,
                                    .mBroker = invalid->mParseResult.mBroker,
                                    .mDiagnostic = invalid->mParseResult.mDiagnostics[0]}});

        EXPECT_EQ(diagnostic.mSourceIndex, 0U);
        EXPECT_EQ(diagnostic.mDiagnostic.mSeverity, DiagnosticSeverity::Warning);
        EXPECT_EQ(diagnostic.mDiagnostic.mCode, DiagnosticCode::InvalidValue);
        EXPECT_EQ(diagnostic.mDiagnostic.mSourceFile, "invalid-time.csv");
        EXPECT_EQ(diagnostic.mDiagnostic.mRowIndex, 2U);
        EXPECT_EQ(diagnostic.mDiagnostic.mField, "datetime");
        EXPECT_FALSE(
            test::referencedMetadata(result.mStatement, result.mStatement.mChronologicalOrder[1])
                .mSourceTimestamp.has_value());
        EXPECT_FALSE(
            test::referencedMetadata(result.mStatement, result.mStatement.mChronologicalOrder[2])
                .mSourceTimestamp.has_value());
    } while (std::next_permutation(order.begin(), order.end()));
}

} // namespace
