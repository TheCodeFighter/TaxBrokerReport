#include "../support/property_random.hpp"
#include "../support/statement_merge_assertions.hpp"
#include "taxbroker/statement_merger.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace taxbroker;

constexpr std::uint64_t PROPERTY_SEED = 20261101;
constexpr int PROPERTY_ITERATIONS = 100;
constexpr std::size_t INPUT_COUNT = 3;

constexpr std::array kTransactionIds{std::string_view{"id-a"},
                                     std::string_view{"id-b"},
                                     std::string_view{"id-c"},
                                     std::string_view{"id-d"}};
constexpr std::array kIsins{std::string_view{"XX0000000001"}, std::string_view{"XX0000000002"}};
constexpr std::array kNames{std::string_view{"Synthetic Share A"},
                            std::string_view{"Synthetic Share B"}};
constexpr std::array kDates{
    std::string_view{"2024-01-10"}, std::string_view{"2024-02-10"}, std::string_view{"2024-03-10"}};

/// One trade of the transaction id with the given index. Every id has the same isin, date, side
/// and units in every file, so equal ids are exact duplicates unless aUnitsOffset changes them.
TradeTransaction makeTrade(std::size_t aSourceIndex,
                           std::size_t aEventIndex,
                           std::size_t aIdIndex,
                           Units aUnitsOffset) {
    return TradeTransaction{
        .mMetadata =
            {
                .mTaxDate = *parseCalendarDate(kDates[aIdIndex % kDates.size()]),
                .mSources = {SourceReference{
                    .mBroker = Broker::TradeRepublic,
                    .mFilename =
                        SourceFilename::fromPath("file" + std::to_string(aSourceIndex) + ".csv"),
                    .mSourceRow = aEventIndex + 2,
                    .mTransactionId = std::string{kTransactionIds[aIdIndex]},
                    .mInputSequence = {.mSourceIndex = aSourceIndex, .mEventIndex = aEventIndex},
                }},
            },
        .mTradeSide = aIdIndex % 2 == 0 ? TradeSide::Buy : TradeSide::Sell,
        .mUnitPrice = 1'000'000'000, // EUR 10
        .mUnits = static_cast<Units>(1 + aIdIndex % 2) * 100'000'000 + aUnitsOffset,
        .mCurrency = Currency::EUR,
    };
}

/// Three files with one to four trades each. About one trade in four differs in its units from
/// the same id in another file, and a new instrument is a fund in one case out of four, so the
/// inputs hold exact duplicates, conflicts, name conflicts and asset class conflicts.
std::vector<StatementMergeInput> randomInputs(test::PropertyRandom& aRandom) {
    std::vector<StatementMergeInput> inputs;

    for (std::size_t sourceIndex = 0; sourceIndex < INPUT_COUNT; ++sourceIndex)
    {
        StatementMergeInput input{.mSourceIndex = sourceIndex};
        const auto tradeCount = static_cast<std::size_t>(aRandom.between(1, 4));

        for (std::size_t eventIndex = 0; eventIndex < tradeCount; ++eventIndex)
        {
            const auto idIndex = static_cast<std::size_t>(aRandom.below(kTransactionIds.size()));
            const Units unitsOffset = aRandom.below(4) == 0 ? 100'000'000 : 0;
            const auto isin = kIsins[idIndex % kIsins.size()];

            auto instrument = std::find_if(
                input.mParseResult.mStatement.mTradeInstruments.begin(),
                input.mParseResult.mStatement.mTradeInstruments.end(),
                [isin](const TradeInstrument& aInstrument) { return aInstrument.mIsin == isin; });

            if (instrument == input.mParseResult.mStatement.mTradeInstruments.end())
            {
                input.mParseResult.mStatement.mTradeInstruments.push_back(TradeInstrument{
                    .mName = std::string{kNames[aRandom.below(kNames.size())]},
                    .mIsin = std::string{isin},
                    .mAssetClass = aRandom.below(4) == 0 ? AssetClass::Fund : AssetClass::Stock,
                });
                instrument = std::prev(input.mParseResult.mStatement.mTradeInstruments.end());
            }

            instrument->mTransactions.push_back(
                makeTrade(sourceIndex, eventIndex, idIndex, unitsOffset));
        }

        // Parser diagnostics come out of the merge in the order of the request indices.
        const auto diagnosticCount = aRandom.below(3);

        for (std::uint64_t index = 0; index < diagnosticCount; ++index)
        {
            input.mParseResult.mDiagnostics.push_back(ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Warning,
                .mSourceFile = "file" + std::to_string(sourceIndex) + ".csv",
                .mRowIndex = index + 2,
                .mMessage =
                    "note " + std::to_string(index) + " of file " + std::to_string(sourceIndex),
            });
        }

        inputs.push_back(std::move(input));
    }

    return inputs;
}

/// Every field of a parser diagnostic as the merge passes it on.
auto parseDiagnosticFields(const SourcedParseDiagnostic& aDiagnostic) {
    const auto& diagnostic = aDiagnostic.mDiagnostic;

    return std::tuple{aDiagnostic.mSourceIndex,
                      aDiagnostic.mBroker,
                      diagnostic.mSeverity,
                      diagnostic.mCode,
                      diagnostic.mSourceFile,
                      diagnostic.mRowIndex,
                      diagnostic.mTransactionId,
                      diagnostic.mField,
                      diagnostic.mMessage,
                      diagnostic.mMessageKey,
                      diagnostic.mMessageParams,
                      diagnostic.mRowKind};
}

/// Every field of a merge diagnostic, with the name and asset-class variants in their order.
auto mergeDiagnosticFields(const MergeDiagnostic& aDiagnostic) {
    std::vector<std::tuple<std::string, std::vector<SourceReference>>> names;
    std::vector<std::tuple<AssetClass, std::vector<SourceReference>, std::optional<std::string>>>
        assetClasses;

    std::transform(aDiagnostic.mNameVariants.begin(),
                   aDiagnostic.mNameVariants.end(),
                   std::back_inserter(names),
                   [](const InstrumentNameVariant& aVariant) {
                       return std::tuple{aVariant.mName, aVariant.mSources};
                   });
    std::transform(
        aDiagnostic.mAssetClassVariants.begin(),
        aDiagnostic.mAssetClassVariants.end(),
        std::back_inserter(assetClasses),
        [](const InstrumentAssetClassVariant& aVariant) {
            return std::tuple{aVariant.mAssetClass, aVariant.mSources, aVariant.mUnrecognizedLabel};
        });

    return std::tuple{aDiagnostic.mSeverity,
                      aDiagnostic.mCode,
                      aDiagnostic.mMessage,
                      aDiagnostic.mMessageKey,
                      aDiagnostic.mMessageParams,
                      aDiagnostic.mSources,
                      aDiagnostic.mTaxDate,
                      aDiagnostic.mInstrumentName,
                      aDiagnostic.mIsin,
                      aDiagnostic.mSourceIndex,
                      aDiagnostic.mEventKinds,
                      names,
                      assetClasses};
}

/// Compares the parts of two merge results that must not depend on the order of the inputs.
/// Returns the first difference, or nothing when the results agree.
std::optional<std::string> mergeResultDifference(const StatementMergeResult& aLeft,
                                                 const StatementMergeResult& aRight) {
    if (aLeft.mStatement.mChronologicalOrder != aRight.mStatement.mChronologicalOrder)
    {
        return "the chronological order differs";
    }

    const auto& leftInstruments = aLeft.mStatement.mPresentation.mTradeInstruments;
    const auto& rightInstruments = aRight.mStatement.mPresentation.mTradeInstruments;

    if (leftInstruments.size() != rightInstruments.size())
    {
        return "the number of instruments differs";
    }

    for (std::size_t index = 0; index < leftInstruments.size(); ++index)
    {
        const auto& left = leftInstruments[index];
        const auto& right = rightInstruments[index];

        if (left.mIsin != right.mIsin || left.mName != right.mName ||
            left.mAssetClass != right.mAssetClass)
        {
            return "instrument " + std::to_string(index) + " differs";
        }

        if (left.mTransactions.size() != right.mTransactions.size())
        {
            return "the number of trades of instrument " + std::to_string(index) + " differs";
        }

        for (std::size_t trade = 0; trade < left.mTransactions.size(); ++trade)
        {
            const auto& leftTrade = left.mTransactions[trade];
            const auto& rightTrade = right.mTransactions[trade];

            if (test::eventFacts(leftTrade) != test::eventFacts(rightTrade) ||
                leftTrade.mMetadata != rightTrade.mMetadata ||
                leftTrade.mInstrumentEvidence != rightTrade.mInstrumentEvidence ||
                leftTrade.mUnitEvidence != rightTrade.mUnitEvidence)
            {
                return "trade " + std::to_string(trade) + " of instrument " +
                       std::to_string(index) + " differs";
            }
        }
    }

    if (aLeft.mDiagnostics.size() != aRight.mDiagnostics.size())
    {
        return "the number of diagnostics differs";
    }

    for (std::size_t index = 0; index < aLeft.mDiagnostics.size(); ++index)
    {
        const auto& right = aRight.mDiagnostics[index];
        const auto isSame = [&right](const auto& aLeftDiagnostic) {
            using Diagnostic = std::decay_t<decltype(aLeftDiagnostic)>;
            const auto* rightDiagnostic = std::get_if<Diagnostic>(&right);

            if (rightDiagnostic == nullptr)
            {
                return false;
            }

            if constexpr (std::is_same_v<Diagnostic, SourcedParseDiagnostic>)
            {
                return parseDiagnosticFields(aLeftDiagnostic) ==
                       parseDiagnosticFields(*rightDiagnostic);
            }
            else if constexpr (std::is_same_v<Diagnostic, MergeDiagnostic>)
            {
                return mergeDiagnosticFields(aLeftDiagnostic) ==
                       mergeDiagnosticFields(*rightDiagnostic);
            }
            else
            {
                static_assert(std::is_same_v<Diagnostic, void>, "Unsupported diagnostic kind");
            }
        };

        if (!std::visit(isSame, aLeft.mDiagnostics[index]))
        {
            return "diagnostic " + std::to_string(index) + " differs";
        }
    }

    return std::nullopt;
}

/// One file with id-a (a buy of 1 unit) and id-c (a buy of aSecondUnits) of Synthetic Share A.
StatementMergeInput twoTradeInput(std::size_t aSourceIndex, Units aSecondUnits) {
    StatementMergeInput input{.mSourceIndex = aSourceIndex};

    input.mParseResult.mStatement.mTradeInstruments.push_back(TradeInstrument{
        .mName = "Synthetic Share A",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions = {makeTrade(aSourceIndex, 0, 0, 0),
                          makeTrade(aSourceIndex, 1, 2, aSecondUnits - 100'000'000)},
    });

    return input;
}

TEST(StatementMergerPropertyTest, TwoFilesWithTheSameTradesMergeToOneResultInEveryOrder) {
    // id-a is a buy of 1 unit on 2024-01-10 and id-c a buy of 1 unit on 2024-03-10, in both files.
    const auto forward = DeterministicStatementMerger{}.merge(
        std::vector{twoTradeInput(0, 100'000'000), twoTradeInput(1, 100'000'000)});
    const auto backward = DeterministicStatementMerger{}.merge(
        std::vector{twoTradeInput(1, 100'000'000), twoTradeInput(0, 100'000'000)});

    ASSERT_EQ(forward.mStatement.mPresentation.mTradeInstruments.size(), 1U);
    ASSERT_EQ(forward.mStatement.mPresentation.mTradeInstruments.front().mTransactions.size(), 2U);
    EXPECT_EQ(forward.mStatement.mPresentation.mTradeInstruments.front()
                  .mTransactions.front()
                  .mMetadata.mSources.size(),
              2U); // the same trade in two files keeps both sources

    EXPECT_EQ(mergeResultDifference(forward, backward), std::nullopt);
}

TEST(StatementMergerPropertyTest, TheMergeResultDoesNotDependOnTheOrderOfTheInputs) {
    std::size_t casesWithADuplicate = 0;
    std::size_t casesWithAConflict = 0;
    std::size_t casesWithANameConflict = 0;
    std::size_t casesWithAnAssetClassConflict = 0;

    for (int iteration = 0; iteration < PROPERTY_ITERATIONS; ++iteration)
    {
        const auto seed = PROPERTY_SEED + static_cast<std::uint64_t>(iteration);
        SCOPED_TRACE("seed " + std::to_string(seed));
        test::PropertyRandom random{seed};
        const auto inputs = randomInputs(random);

        std::array<std::size_t, INPUT_COUNT> order{0, 1, 2};
        std::optional<StatementMergeResult> expected;

        do
        {
            std::vector<StatementMergeInput> arrival;

            std::transform(order.begin(),
                           order.end(),
                           std::back_inserter(arrival),
                           [&inputs](std::size_t aIndex) { return inputs[aIndex]; });

            auto actual = DeterministicStatementMerger{}.merge(std::move(arrival));

            if (!expected)
            {
                expected = std::move(actual);
                continue;
            }

            EXPECT_EQ(mergeResultDifference(actual, *expected), std::nullopt)
                << "arrival order " << order[0] << order[1] << order[2];
        } while (std::next_permutation(order.begin(), order.end()));

        for (const auto& instrument : expected->mStatement.mPresentation.mTradeInstruments)
        {
            casesWithADuplicate += static_cast<std::size_t>(
                std::count_if(instrument.mTransactions.begin(),
                              instrument.mTransactions.end(),
                              [](const TradeTransaction& aTrade) {
                                  return aTrade.mMetadata.mSources.size() > 1;
                              }));
        }

        for (const auto& diagnostic : expected->mDiagnostics)
        {
            const auto* merge = std::get_if<MergeDiagnostic>(&diagnostic);

            casesWithAConflict +=
                merge != nullptr && merge->mCode == MergeDiagnosticCode::ConflictingDuplicate ? 1
                                                                                              : 0;
            casesWithANameConflict +=
                merge != nullptr && merge->mCode == MergeDiagnosticCode::InstrumentNameConflict ? 1
                                                                                                : 0;
            casesWithAnAssetClassConflict +=
                merge != nullptr &&
                        merge->mCode == MergeDiagnosticCode::InstrumentAssetClassConflict
                    ? 1
                    : 0;
        }
    }

    // The generator must really produce exact duplicates and conflicts, or the property is empty.
    EXPECT_GT(casesWithADuplicate, 0U);
    EXPECT_GT(casesWithAConflict, 0U);
    EXPECT_GT(casesWithANameConflict, 0U);
    EXPECT_GT(casesWithAnAssetClassConflict, 0U);
}

TEST(StatementMergerPropertyTest, TheCheckReportsAChangedTradeOfOneResult) {
    const auto left = DeterministicStatementMerger{}.merge(
        std::vector{twoTradeInput(0, 100'000'000), twoTradeInput(1, 100'000'000)});
    auto right = DeterministicStatementMerger{}.merge(
        std::vector{twoTradeInput(0, 100'000'000), twoTradeInput(1, 100'000'000)});

    // The second trade becomes 2 units instead of 1.
    right.mStatement.mPresentation.mTradeInstruments.front().mTransactions.back().mUnits =
        200'000'000;

    EXPECT_EQ(mergeResultDifference(left, right), "trade 1 of instrument 0 differs");
}

TEST(StatementMergerPropertyTest, TheCheckReportsAMissingInstrument) {
    const auto left = DeterministicStatementMerger{}.merge(
        std::vector{twoTradeInput(0, 100'000'000), twoTradeInput(1, 100'000'000)});
    auto right = DeterministicStatementMerger{}.merge(
        std::vector{twoTradeInput(0, 100'000'000), twoTradeInput(1, 100'000'000)});

    // Synthetic Share A is gone from one result.
    right.mStatement.mPresentation.mTradeInstruments.clear();

    EXPECT_EQ(mergeResultDifference(left, right), "the number of instruments differs");
}

TEST(StatementMergerPropertyTest, TheCheckReportsAnInstrumentWithAnotherName) {
    const auto left = DeterministicStatementMerger{}.merge(
        std::vector{twoTradeInput(0, 100'000'000), twoTradeInput(1, 100'000'000)});
    auto right = DeterministicStatementMerger{}.merge(
        std::vector{twoTradeInput(0, 100'000'000), twoTradeInput(1, 100'000'000)});

    // Synthetic Share A is shown as Synthetic Share B in one result.
    right.mStatement.mPresentation.mTradeInstruments.front().mName = "Synthetic Share B";

    EXPECT_EQ(mergeResultDifference(left, right), "instrument 0 differs");
}

TEST(StatementMergerPropertyTest, TheCheckReportsAMissingTrade) {
    const auto left = DeterministicStatementMerger{}.merge(
        std::vector{twoTradeInput(0, 100'000'000), twoTradeInput(1, 100'000'000)});
    auto right = DeterministicStatementMerger{}.merge(
        std::vector{twoTradeInput(0, 100'000'000), twoTradeInput(1, 100'000'000)});

    // id-c (the second of the two trades) is gone from one result.
    right.mStatement.mPresentation.mTradeInstruments.front().mTransactions.pop_back();

    EXPECT_EQ(mergeResultDifference(left, right), "the number of trades of instrument 0 differs");
}

TEST(StatementMergerPropertyTest, TheCheckReportsAChangedChronologicalOrder) {
    const auto left = DeterministicStatementMerger{}.merge(
        std::vector{twoTradeInput(0, 100'000'000), twoTradeInput(1, 100'000'000)});
    auto right = DeterministicStatementMerger{}.merge(
        std::vector{twoTradeInput(0, 100'000'000), twoTradeInput(1, 100'000'000)});

    // The two trades (id-a on 2024-01-10, id-c on 2024-03-10) swap places in one result.
    std::swap(right.mStatement.mChronologicalOrder.front(),
              right.mStatement.mChronologicalOrder.back());

    EXPECT_EQ(mergeResultDifference(left, right), "the chronological order differs");
}

TEST(StatementMergerPropertyTest, TheCheckReportsParserDiagnosticsInAnotherOrder) {
    auto firstFile = twoTradeInput(0, 100'000'000);
    auto secondFile = twoTradeInput(1, 100'000'000);

    firstFile.mParseResult.mDiagnostics.push_back(
        ParseDiagnostic{.mSeverity = DiagnosticSeverity::Warning, .mMessage = "note of file 0"});
    secondFile.mParseResult.mDiagnostics.push_back(
        ParseDiagnostic{.mSeverity = DiagnosticSeverity::Warning, .mMessage = "note of file 1"});

    const auto left = DeterministicStatementMerger{}.merge(std::vector{firstFile, secondFile});
    auto right = DeterministicStatementMerger{}.merge(std::vector{firstFile, secondFile});

    // Two parser diagnostics in the order of the request indices: file 0, then file 1.
    ASSERT_EQ(left.mDiagnostics.size(), 2U);

    std::swap(right.mDiagnostics.front(), right.mDiagnostics.back());

    EXPECT_EQ(mergeResultDifference(left, right), "diagnostic 0 differs");
}

TEST(StatementMergerPropertyTest, TheCheckReportsAParserDiagnosticOnAnotherRow) {
    auto firstFile = twoTradeInput(0, 100'000'000);

    firstFile.mParseResult.mDiagnostics.push_back(
        ParseDiagnostic{.mSeverity = DiagnosticSeverity::Warning,
                        .mRowIndex = 2,
                        .mMessage = "note of file 0"});

    const auto left = DeterministicStatementMerger{}.merge(std::vector{firstFile});
    auto right = DeterministicStatementMerger{}.merge(std::vector{firstFile});

    ASSERT_EQ(left.mDiagnostics.size(), 1U);

    // The same note, moved from row 2 to row 3.
    std::get<SourcedParseDiagnostic>(right.mDiagnostics.front()).mDiagnostic.mRowIndex = 3;

    EXPECT_EQ(mergeResultDifference(left, right), "diagnostic 0 differs");
}

TEST(StatementMergerPropertyTest, TheCheckReportsNameVariantsInAnotherOrder) {
    // id-a (a buy of 1 unit of XX0000000001) in two files, under two names: a name conflict.
    const StatementMergeInput firstFile{
        .mSourceIndex = 0,
        .mParseResult = {.mStatement = {.mTradeInstruments = {TradeInstrument{
                                            .mName = "Synthetic Share A",
                                            .mIsin = "XX0000000001",
                                            .mAssetClass = AssetClass::Stock,
                                            .mTransactions = {makeTrade(0, 0, 0, 0)},
                                        }}}},
    };
    const StatementMergeInput secondFile{
        .mSourceIndex = 1,
        .mParseResult = {.mStatement = {.mTradeInstruments = {TradeInstrument{
                                            .mName = "Synthetic Share B",
                                            .mIsin = "XX0000000001",
                                            .mAssetClass = AssetClass::Stock,
                                            .mTransactions = {makeTrade(1, 0, 0, 0)},
                                        }}}},
    };

    const auto left = DeterministicStatementMerger{}.merge(std::vector{firstFile, secondFile});
    auto right = DeterministicStatementMerger{}.merge(std::vector{firstFile, secondFile});

    ASSERT_EQ(left.mDiagnostics.size(), 1U);

    auto& conflict = std::get<MergeDiagnostic>(right.mDiagnostics.front());

    ASSERT_EQ(conflict.mCode, MergeDiagnosticCode::InstrumentNameConflict);
    ASSERT_EQ(conflict.mNameVariants.size(), 2U);

    // A broken merger that lists the names in arrival order: file 1 first.
    std::swap(conflict.mNameVariants.front(), conflict.mNameVariants.back());

    EXPECT_EQ(mergeResultDifference(left, right), "diagnostic 0 differs");
}

TEST(StatementMergerPropertyTest, TheCheckReportsADifferentNumberOfDiagnostics) {
    // The second file has id-c with 2 units instead of 1: a conflict with the first file.
    const auto conflicting = DeterministicStatementMerger{}.merge(
        std::vector{twoTradeInput(0, 100'000'000), twoTradeInput(1, 200'000'000)});
    const auto agreeing = DeterministicStatementMerger{}.merge(
        std::vector{twoTradeInput(0, 100'000'000), twoTradeInput(1, 100'000'000)});

    ASSERT_EQ(agreeing.mDiagnostics.size(), 0U);
    ASSERT_GT(conflicting.mDiagnostics.size(), 0U);

    // The trades of the two results already differ, so compare the diagnostics on their own.
    const auto left = agreeing;
    auto right = agreeing;
    right.mDiagnostics = conflicting.mDiagnostics;

    EXPECT_EQ(mergeResultDifference(left, right), "the number of diagnostics differs");
}

} // namespace
