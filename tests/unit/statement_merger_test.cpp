#include "taxbroker/statement_merger.hpp"
#include "../support/statement_merge_assertions.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <iterator>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {
using namespace taxbroker;

// These unit tests build parsed records in memory. CSV filenames are source labels only;
// no files are read here. Parser-to-merger tests live in statement_merger_integration_test.cpp.
SourceReference makeSource(Broker aBroker,
                           std::string_view aFilename,
                           std::size_t aRow,
                           std::string_view aTransactionId,
                           StableInputSequence aSequence) {
    return SourceReference{
        .mBroker = aBroker,
        .mFilename = SourceFilename::fromPath(aFilename),
        .mSourceRow = aRow,
        .mTransactionId = std::string{aTransactionId},
        .mInputSequence = aSequence,
    };
}

Date makeDate(int aYear, unsigned aMonth, unsigned aDay) {
    const auto calendarDate =
        std::chrono::year{aYear} / std::chrono::month{aMonth} / std::chrono::day{aDay};
    return Date{std::chrono::sys_days{calendarDate}.time_since_epoch()};
}

SourceTimestamp makeTimestamp(int aYear, unsigned aMonth, unsigned aDay, int aHour) {
    const auto calendarDate =
        std::chrono::year{aYear} / std::chrono::month{aMonth} / std::chrono::day{aDay};
    return SourceTimestamp{std::chrono::sys_days{calendarDate}.time_since_epoch() +
                           std::chrono::hours{aHour}};
}

EventMetadata makeMetadata(std::size_t aSourceIndex,
                           std::size_t aEventIndex,
                           Date aDate,
                           Broker aBroker = Broker::TradeRepublic,
                           std::string_view aFilename = "statement.csv",
                           std::optional<SourceTimestamp> aTimestamp = std::nullopt,
                           std::string_view aTransactionId = {}) {
    return EventMetadata{
        .mTaxDate = aDate,
        .mSourceTimestamp = aTimestamp,
        .mSources = {makeSource(aBroker,
                                aFilename,
                                aEventIndex + 2,
                                aTransactionId,
                                {.mSourceIndex = aSourceIndex, .mEventIndex = aEventIndex})},
    };
}

TradeTransaction makeTrade(std::size_t aSourceIndex,
                           std::size_t aEventIndex,
                           Date aDate,
                           Broker aBroker = Broker::TradeRepublic,
                           std::string_view aFilename = "statement.csv",
                           std::string_view aTransactionId = {}) {
    return TradeTransaction{
        .mMetadata = makeMetadata(aSourceIndex,
                                  aEventIndex,
                                  aDate,
                                  aBroker,
                                  aFilename,
                                  std::nullopt,
                                  aTransactionId),
        .mTradeSide = TradeSide::Buy,
        .mUnitPrice = 10000,
        .mUnits = UNITS_SCALE,
        .mCurrency = Currency::EUR,
    };
}

template <typename Diagnostic>
std::vector<const Diagnostic*> diagnosticsOf(const StatementMergeResult& aResult) {
    std::vector<const Diagnostic*> diagnostics;

    for (const auto& diagnostic : aResult.mDiagnostics)
    {
        if (const auto* value = std::get_if<Diagnostic>(&diagnostic))
        {
            diagnostics.push_back(value);
        }
    }

    return diagnostics;
}

const MergeDiagnostic* findMergeDiagnostic(const StatementMergeResult& aResult,
                                           MergeDiagnosticCode aCode) {
    const auto diagnostics = diagnosticsOf<MergeDiagnostic>(aResult);
    const auto found = std::find_if(diagnostics.begin(), diagnostics.end(), [&](const auto* value) {
        return value->mCode == aCode;
    });

    return found == diagnostics.end() ? nullptr : *found;
}

const EventMetadata& referencedMetadata(const MergedStatement& aStatement,
                                        const StatementEventReference& aReference) {
    // Chronological entries point to records stored in the presentation collections.
    const auto& presentation = aStatement.mPresentation;

    switch (aReference.mKind)
    {
    case StatementEventKind::Trade:
        return presentation.mTradeInstruments.at(aReference.mInstrumentIndex.value())
            .mTransactions.at(aReference.mEventIndex)
            .mMetadata;
    case StatementEventKind::CorporateAction:
        return presentation.mTradeInstruments.at(aReference.mInstrumentIndex.value())
            .mCorporateActions.at(aReference.mEventIndex)
            .mMetadata;
    case StatementEventKind::Dividend:
        return presentation.mDividendInstruments.at(aReference.mInstrumentIndex.value())
            .mTransactions.at(aReference.mEventIndex)
            .mMetadata;
    case StatementEventKind::Interest:
        return presentation.mInterestInstruments.at(aReference.mInstrumentIndex.value())
            .mTransactions.at(aReference.mEventIndex)
            .mMetadata;
    case StatementEventKind::Benefit:
        return presentation.mBenefitEvents.at(aReference.mEventIndex).mMetadata;
    case StatementEventKind::PrivateMarket:
        return presentation.mPrivateMarketEvents.at(aReference.mEventIndex).mMetadata;
    }

    throw std::logic_error{"Unsupported event reference"};
}

static_assert(std::is_abstract_v<StatementMerger>);
static_assert(std::is_base_of_v<StatementMerger, DeterministicStatementMerger>);
static_assert(!std::is_abstract_v<DeterministicStatementMerger>);
static_assert(
    std::is_same_v<decltype(StatementMergeResult{}.mStatement.mPresentation), BrokerStatement>);

TEST(StatementMergeContractTest, EmptyResultHasNoEventsOrDiagnostics) {
    const StatementMergeResult result;

    EXPECT_TRUE(result.mStatement.mPresentation.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mPresentation.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mPresentation.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mPresentation.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPresentation.mPrivateMarketEvents.empty());
    EXPECT_TRUE(result.mStatement.mChronologicalOrder.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(StatementMergeContractTest, ParsedAndMergedEventsCanRetainEverySource) {
    EventMetadata metadata{
        .mSources =
            {
                makeSource(Broker::TradeRepublic,
                           "later.csv",
                           7,
                           "shared-id",
                           {.mSourceIndex = 2, .mEventIndex = 5}),
                makeSource(Broker::TradeRepublic,
                           "earlier.csv",
                           4,
                           "shared-id",
                           {.mSourceIndex = 0, .mEventIndex = 2}),
            },
    };

    ASSERT_EQ(metadata.mSources.size(), 2U);
    EXPECT_EQ(primarySource(metadata).mFilename.value(), "earlier.csv");
    ASSERT_TRUE(transactionIdentity(metadata).has_value());
    EXPECT_EQ(transactionIdentity(metadata)->mBroker, Broker::TradeRepublic);
    EXPECT_EQ(transactionIdentity(metadata)->mTransactionId, "shared-id");

    std::sort(metadata.mSources.begin(), metadata.mSources.end(), StableSourceOrder{});
    EXPECT_EQ(metadata.mSources.front().mFilename.value(), "earlier.csv");
    EXPECT_EQ(metadata.mSources.back().mFilename.value(), "later.csv");
}

TEST(StatementMergeContractTest, InconsistentSourcesDoNotExposeOneTransactionIdentity) {
    const EventMetadata metadata{
        .mSources =
            {
                makeSource(Broker::TradeRepublic,
                           "tr.csv",
                           2,
                           "shared-id",
                           {.mSourceIndex = 0, .mEventIndex = 0}),
                makeSource(Broker::InteractiveBrokers,
                           "ibkr.csv",
                           2,
                           "shared-id",
                           {.mSourceIndex = 1, .mEventIndex = 0}),
            },
    };

    EXPECT_FALSE(transactionIdentity(metadata).has_value());
}

TEST(StatementMergeContractTest, ChronologicalViewReferencesPresentationStorage) {
    MergedStatement statement;
    statement.mPresentation.mTradeInstruments.push_back(TradeInstrument{
        .mName = "Synthetic Shares",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions = {TradeTransaction{}},
    });
    statement.mChronologicalOrder.push_back(StatementEventReference{
        .mKind = StatementEventKind::Trade,
        .mInstrumentIndex = 0,
        .mEventIndex = 0,
    });

    ASSERT_EQ(statement.mChronologicalOrder.size(), 1U);
    const auto& reference = statement.mChronologicalOrder.front();
    ASSERT_TRUE(reference.mInstrumentIndex.has_value());
    EXPECT_EQ(reference.mKind, StatementEventKind::Trade);
    EXPECT_EQ(statement.mPresentation.mTradeInstruments[*reference.mInstrumentIndex].mName,
              "Synthetic Shares");
    EXPECT_EQ(statement.mPresentation.mTradeInstruments[*reference.mInstrumentIndex]
                  .mTransactions[reference.mEventIndex]
                  .mUnits,
              0);
}

TEST(StatementMergeContractTest, ParserDiagnosticsRetainSourceContextAndOriginalFields) {
    StatementMergeResult result;
    result.mDiagnostics.emplace_back(SourcedParseDiagnostic{
        .mSourceIndex = 3,
        .mBroker = Broker::TradeRepublic,
        .mDiagnostic =
            ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = DiagnosticCode::ParseError,
                .mSourceFile = "failed.csv",
                .mMessage = "Synthetic parser failure.",
            },
    });
    result.mDiagnostics.emplace_back(MergeDiagnostic{
        .mCode = MergeDiagnosticCode::ConflictingDuplicate,
        .mMessage = "Synthetic merge conflict.",
    });

    ASSERT_EQ(result.mDiagnostics.size(), 2U);
    EXPECT_EQ(diagnosticStage(result.mDiagnostics[0]), StatementDiagnosticStage::Parsing);
    EXPECT_EQ(diagnosticStage(result.mDiagnostics[1]), StatementDiagnosticStage::Merging);

    const auto& parserDiagnostic = std::get<SourcedParseDiagnostic>(result.mDiagnostics.front());
    EXPECT_EQ(parserDiagnostic.mSourceIndex, 3U);
    EXPECT_EQ(parserDiagnostic.mBroker, Broker::TradeRepublic);
    EXPECT_EQ(parserDiagnostic.mDiagnostic.mCode, DiagnosticCode::ParseError);
    EXPECT_EQ(parserDiagnostic.mDiagnostic.mSourceFile, "failed.csv");
}

TEST(StatementMergeContractTest, PartialFailureAndMixedBrokerDataCanCoexist) {
    StatementMergeResult result;
    result.mStatement.mPresentation.mBenefitEvents.push_back(BenefitEvent{
        .mMetadata =
            EventMetadata{
                .mSources = {makeSource(Broker::InteractiveBrokers,
                                        "valid.csv",
                                        2,
                                        "benefit-id",
                                        {.mSourceIndex = 1, .mEventIndex = 0})},
            },
        .mType = BenefitType::StockPerk,
        .mName = "Synthetic Benefit",
    });
    result.mStatement.mChronologicalOrder.push_back(StatementEventReference{
        .mKind = StatementEventKind::Benefit,
        .mEventIndex = 0,
    });
    result.mDiagnostics.emplace_back(SourcedParseDiagnostic{
        .mSourceIndex = 0,
        .mBroker = Broker::TradeRepublic,
        .mDiagnostic =
            ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = DiagnosticCode::ParseError,
                .mSourceFile = "failed.csv",
                .mMessage = "Synthetic parser failure.",
            },
    });

    EXPECT_EQ(result.mStatement.mPresentation.mBenefitEvents.size(), 1U);
    EXPECT_EQ(result.mStatement.mChronologicalOrder.size(), 1U);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(std::get<SourcedParseDiagnostic>(result.mDiagnostics.front()).mBroker,
              Broker::TradeRepublic);
    EXPECT_EQ(
        primarySource(result.mStatement.mPresentation.mBenefitEvents.front().mMetadata).mBroker,
        Broker::InteractiveBrokers);
}

TEST(StatementMergeContractTest, InputsCarryRequestOrderIndependentlyOfArrivalOrder) {
    const std::array inputs{
        StatementMergeInput{
            .mSourceIndex = 5,
            .mParseResult = ParseResult{.mBroker = Broker::TradeRepublic},
        },
        StatementMergeInput{
            .mSourceIndex = 1,
            .mParseResult = ParseResult{.mBroker = Broker::InteractiveBrokers},
        },
    };
    const std::span<const StatementMergeInput> inputView{inputs};

    ASSERT_EQ(inputView.size(), inputs.size());
    EXPECT_EQ(inputView.front().mSourceIndex, 5U);
    EXPECT_EQ(inputView.back().mSourceIndex, 1U);
    EXPECT_EQ(inputView.front().mParseResult.mBroker, Broker::TradeRepublic);
    EXPECT_EQ(inputView.back().mParseResult.mBroker, Broker::InteractiveBrokers);
}

TEST(DeterministicStatementMergerTest, EmptyInputProducesEmptyResult) {
    const DeterministicStatementMerger merger;

    const auto result = merger.merge({});

    EXPECT_TRUE(result.mStatement.mPresentation.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mPresentation.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mPresentation.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mPresentation.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPresentation.mPrivateMarketEvents.empty());
    EXPECT_TRUE(result.mStatement.mChronologicalOrder.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(DeterministicStatementMergerTest, GroupsTradesByIsinAcrossBrokersAndOrdersEvents) {
    StatementMergeInput laterInput{
        .mSourceIndex = 1,
        .mParseResult = ParseResult{.mBroker = Broker::InteractiveBrokers},
    };
    laterInput.mParseResult.mStatement.mTradeInstruments.push_back(TradeInstrument{
        .mName = "Later broker name",
        .mIsin = "XX0000000002",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {makeTrade(1, 2, makeDate(2024, 2, 1), Broker::InteractiveBrokers, "ibkr.csv")},
    });
    laterInput.mParseResult.mStatement.mTradeInstruments.push_back(TradeInstrument{
        .mName = "Second instrument",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Fund,
        .mTransactions =
            {makeTrade(1, 1, makeDate(2024, 1, 10), Broker::InteractiveBrokers, "ibkr.csv")},
    });

    StatementMergeInput earlierInput{
        .mSourceIndex = 0,
        .mParseResult = ParseResult{.mBroker = Broker::TradeRepublic},
    };
    earlierInput.mParseResult.mStatement.mTradeInstruments.push_back(TradeInstrument{
        .mName = "Earlier source name",
        .mIsin = "XX0000000002",
        .mAssetClass = AssetClass::Stock,
        .mTransactions = {makeTrade(0, 3, makeDate(2023, 12, 1))},
        .mCorporateActions = {CorporateAction{
            .mMetadata = makeMetadata(0, 4, makeDate(2024, 1, 15)),
            .mType = CorporateActionType::Split,
            .mUnitsDelta = UNITS_SCALE,
        }},
    });
    const std::array inputs{laterInput, earlierInput};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    const auto& instruments = result.mStatement.mPresentation.mTradeInstruments;
    ASSERT_EQ(instruments.size(), 2U);
    EXPECT_EQ(instruments[0].mIsin, "XX0000000001");
    EXPECT_EQ(instruments[1].mIsin, "XX0000000002");
    EXPECT_EQ(instruments[1].mName, "Earlier source name");
    EXPECT_EQ(instruments[1].mTransactions.size(), 2U);
    EXPECT_EQ(instruments[1].mTransactions[0].mMetadata.mTaxDate, makeDate(2023, 12, 1));
    EXPECT_EQ(instruments[1].mTransactions[1].mMetadata.mTaxDate, makeDate(2024, 2, 1));
    ASSERT_EQ(instruments[1].mCorporateActions.size(), 1U);
    EXPECT_EQ(primarySource(instruments[1].mTransactions[1].mMetadata).mBroker,
              Broker::InteractiveBrokers);

    const auto* conflict = findMergeDiagnostic(result, MergeDiagnosticCode::InstrumentNameConflict);
    ASSERT_NE(conflict, nullptr);
    EXPECT_EQ(conflict->mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(conflict->mInstrumentName, "Earlier source name");
    EXPECT_EQ(conflict->mNameVariants.size(), 2U);
    EXPECT_EQ(conflict->mSources.size(), 3U);
    EXPECT_EQ(result.mStatement.mChronologicalOrder.size(), 4U);
}

TEST(DeterministicStatementMergerTest, AssetClassConflictIsErrorAndUsesUnknown) {
    StatementMergeInput first{.mSourceIndex = 0};
    first.mParseResult.mStatement.mTradeInstruments.push_back(TradeInstrument{
        .mName = "Synthetic instrument",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions = {makeTrade(0, 0, makeDate(2024, 1, 1))},
    });
    StatementMergeInput second{.mSourceIndex = 1};
    second.mParseResult.mStatement.mTradeInstruments.push_back(TradeInstrument{
        .mName = "Synthetic instrument",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Fund,
        .mTransactions = {makeTrade(1, 0, makeDate(2024, 1, 2))},
    });
    const std::array inputs{second, first};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);
    EXPECT_EQ(result.mStatement.mPresentation.mTradeInstruments.front().mAssetClass,
              AssetClass::Unknown);
    const auto* conflict =
        findMergeDiagnostic(result, MergeDiagnosticCode::InstrumentAssetClassConflict);
    ASSERT_NE(conflict, nullptr);
    EXPECT_EQ(conflict->mSeverity, DiagnosticSeverity::Error);
    ASSERT_EQ(conflict->mAssetClassVariants.size(), 2U);
    EXPECT_EQ(conflict->mAssetClassVariants[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(conflict->mAssetClassVariants[1].mAssetClass, AssetClass::Fund);
    EXPECT_EQ(conflict->mSources.size(), 2U);
}

TEST(DeterministicStatementMergerTest, DeduplicatesExactTradesAndRetainsEverySource) {
    StatementMergeInput first{.mSourceIndex = 0};
    first.mParseResult.mStatement.mTradeInstruments.push_back(TradeInstrument{
        .mName = "Synthetic instrument",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions = {makeTrade(0,
                                    0,
                                    makeDate(2024, 1, 1),
                                    Broker::TradeRepublic,
                                    "first.csv",
                                    "shared-id")},
    });
    StatementMergeInput second{.mSourceIndex = 1};
    second.mParseResult.mStatement.mTradeInstruments.push_back(TradeInstrument{
        .mName = "Synthetic instrument",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions = {makeTrade(1,
                                    0,
                                    makeDate(2024, 1, 1),
                                    Broker::TradeRepublic,
                                    "overlap.csv",
                                    "shared-id")},
    });
    const std::array inputs{first, second};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);
    const auto& transactions =
        result.mStatement.mPresentation.mTradeInstruments.front().mTransactions;
    ASSERT_EQ(transactions.size(), 1U);
    ASSERT_EQ(transactions.front().mMetadata.mSources.size(), 2U);
    EXPECT_EQ(transactions.front().mMetadata.mSources[0].mFilename.value(), "first.csv");
    EXPECT_EQ(transactions.front().mMetadata.mSources[1].mFilename.value(), "overlap.csv");
    EXPECT_TRUE(diagnosticsOf<MergeDiagnostic>(result).empty());
}

// Build one event of each supported kind, with fixed values and transaction IDs.
// Calls with different source indices model overlapping exports of the same six events.
StatementMergeInput makeDuplicateTestInput(std::size_t aSourceIndex, std::string_view aFilename) {
    StatementMergeInput input{.mSourceIndex = aSourceIndex};

    input.mParseResult.mStatement.mTradeInstruments.push_back(TradeInstrument{
        .mName = "Security",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions = {TradeTransaction{
            .mMetadata = makeMetadata(aSourceIndex,
                                      0,
                                      makeDate(2024, 1, 1),
                                      Broker::TradeRepublic,
                                      aFilename,
                                      makeTimestamp(2024, 1, 1, 9),
                                      "trade-id"),
            .mTradeSide = TradeSide::Sell,
            .mUnitPrice = 12000,
            .mUnits = 2 * UNITS_SCALE,
            .mAmount = 24000,
            .mFeePaid = 100,
            .mExchangeRate = 110000000,
            .mCurrency = Currency::USD,
        }},
        .mCorporateActions = {CorporateAction{
            .mMetadata = makeMetadata(aSourceIndex,
                                      1,
                                      makeDate(2024, 1, 2),
                                      Broker::TradeRepublic,
                                      aFilename,
                                      std::nullopt,
                                      "action-id"),
            .mType = CorporateActionType::ReverseSplit,
            .mUnitsDelta = -UNITS_SCALE,
            .mRatio = 50000000,
        }},
    });
    input.mParseResult.mStatement.mDividendInstruments.push_back(DividendInstrument{
        .mName = "Security",
        .mIsin = "XX0000000001",
        .mTransactions = {DividendTransaction{
            .mMetadata = makeMetadata(aSourceIndex,
                                      2,
                                      makeDate(2024, 1, 3),
                                      Broker::TradeRepublic,
                                      aFilename,
                                      std::nullopt,
                                      "dividend-id"),
            .mGrossAmount = 5000,
            .mTaxPaid = 1000,
            .mExchangeRate = 110000000,
            .mCurrency = Currency::USD,
            .mTaxCurrency = Currency::EUR,
        }},
    });
    input.mParseResult.mStatement.mInterestInstruments.push_back(InterestInstrument{
        .mName = "Cash account",
        .mInterestType = InterestType::BrokerInterest,
        .mTransactions = {InterestTransaction{
            .mMetadata = makeMetadata(aSourceIndex,
                                      3,
                                      makeDate(2024, 1, 4),
                                      Broker::TradeRepublic,
                                      aFilename,
                                      std::nullopt,
                                      "interest-id"),
            .mGrossAmount = 6000,
            .mTaxPaid = 1200,
            .mExchangeRate = 120000000,
            .mCurrency = Currency::GBP,
            .mTaxCurrency = Currency::EUR,
        }},
    });
    input.mParseResult.mStatement.mBenefitEvents.push_back(BenefitEvent{
        .mMetadata = makeMetadata(aSourceIndex,
                                  4,
                                  makeDate(2024, 1, 5),
                                  Broker::TradeRepublic,
                                  aFilename,
                                  std::nullopt,
                                  "benefit-id"),
        .mType = BenefitType::StockPerk,
        .mName = "Reward",
        .mIsin = "XX0000000002",
        .mAssetClass = AssetClass::Stock,
        .mAmount = 7000,
        .mCurrency = Currency::EUR,
    });
    input.mParseResult.mStatement.mPrivateMarketEvents.push_back(PrivateMarketEvent{
        .mMetadata = makeMetadata(aSourceIndex,
                                  5,
                                  makeDate(2024, 1, 6),
                                  Broker::TradeRepublic,
                                  aFilename,
                                  std::nullopt,
                                  "private-id"),
        .mType = PrivateMarketEventType::Sell,
        .mName = "Private fund",
        .mIsin = "XX0000000003",
        .mAssetClass = AssetClass::PrivateFund,
        .mAmount = 8000,
        .mFeePaid = 200,
        .mCurrency = Currency::EUR,
        .mDescription = "Synthetic sale",
    });

    return input;
}

TEST(DeterministicStatementMergerTest, DeduplicatesEverySupportedEventKind) {
    const std::array inputs{makeDuplicateTestInput(1, "overlap.csv"),
                            makeDuplicateTestInput(0, "complete.csv")};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);
    const auto& tradeInstrument = result.mStatement.mPresentation.mTradeInstruments.front();
    ASSERT_EQ(tradeInstrument.mTransactions.size(), 1U);
    ASSERT_EQ(tradeInstrument.mCorporateActions.size(), 1U);
    EXPECT_EQ(tradeInstrument.mTransactions.front().mMetadata.mSources.size(), 2U);
    EXPECT_EQ(tradeInstrument.mCorporateActions.front().mMetadata.mSources.size(), 2U);

    ASSERT_EQ(result.mStatement.mPresentation.mDividendInstruments.size(), 1U);
    EXPECT_EQ(result.mStatement.mPresentation.mDividendInstruments.front()
                  .mTransactions.front()
                  .mMetadata.mSources.size(),
              2U);

    ASSERT_EQ(result.mStatement.mPresentation.mInterestInstruments.size(), 1U);
    EXPECT_EQ(result.mStatement.mPresentation.mInterestInstruments.front()
                  .mTransactions.front()
                  .mMetadata.mSources.size(),
              2U);

    ASSERT_EQ(result.mStatement.mPresentation.mBenefitEvents.size(), 1U);
    EXPECT_EQ(result.mStatement.mPresentation.mBenefitEvents.front().mMetadata.mSources.size(), 2U);
    ASSERT_EQ(result.mStatement.mPresentation.mPrivateMarketEvents.size(), 1U);
    EXPECT_EQ(
        result.mStatement.mPresentation.mPrivateMarketEvents.front().mMetadata.mSources.size(),
        2U);
    EXPECT_TRUE(diagnosticsOf<MergeDiagnostic>(result).empty());
}

std::size_t mergedEventCount(const BrokerStatement& aStatement) {
    const auto countTransactions = [](std::size_t aCount, const auto& aInstrument) {
        return aCount + aInstrument.mTransactions.size();
    };
    const auto tradeCount = std::accumulate(
        aStatement.mTradeInstruments.begin(),
        aStatement.mTradeInstruments.end(),
        std::size_t{},
        [](std::size_t aCount, const TradeInstrument& aInstrument) {
            return aCount + aInstrument.mTransactions.size() + aInstrument.mCorporateActions.size();
        });

    return tradeCount + aStatement.mBenefitEvents.size() + aStatement.mPrivateMarketEvents.size() +
           std::accumulate(aStatement.mDividendInstruments.begin(),
                           aStatement.mDividendInstruments.end(),
                           std::size_t{},
                           countTransactions) +
           std::accumulate(aStatement.mInterestInstruments.begin(),
                           aStatement.mInterestInstruments.end(),
                           std::size_t{},
                           countTransactions);
}

template <typename Record> struct FieldChange {
    std::string_view mField;
    void (*mApply)(Record&);
};

template <typename Record, std::size_t Count, typename SelectRecord>
void expectFieldConflicts(StatementEventKind aKind,
                          const std::array<FieldChange<Record>, Count>& aChanges,
                          SelectRecord aSelectRecord) {
    for (const auto& change : aChanges)
    {
        SCOPED_TRACE(change.mField);

        auto first = makeDuplicateTestInput(0, "first.csv");
        auto second = makeDuplicateTestInput(1, "overlap.csv");

        // Separate the trade and split so an instrument-level change affects only the tested kind.
        if (aKind == StatementEventKind::Trade || aKind == StatementEventKind::CorporateAction)
        {
            auto& instruments = second.mParseResult.mStatement.mTradeInstruments;
            auto separateInstrument = instruments.front();

            if (aKind == StatementEventKind::Trade)
            {
                separateInstrument.mTransactions.clear();
                instruments.front().mCorporateActions.clear();
            }
            else
            {
                separateInstrument.mCorporateActions.clear();
                instruments.front().mTransactions.clear();
            }

            instruments.push_back(std::move(separateInstrument));
        }

        // Change one fact in the second copy; its transaction ID still matches the first.
        change.mApply(aSelectRecord(second.mParseResult.mStatement));
        const std::array inputs{second, first};

        const auto result = DeterministicStatementMerger{}.merge(inputs);

        auto expectedStatement = first.mParseResult.mStatement;
        std::vector<std::string> expectedSurvivingIds{"trade-id",
                                                      "action-id",
                                                      "dividend-id",
                                                      "interest-id",
                                                      "benefit-id",
                                                      "private-id"};

        switch (aKind)
        {
        case StatementEventKind::Trade:
            expectedStatement.mTradeInstruments.front().mTransactions.clear();
            std::erase(expectedSurvivingIds, "trade-id");
            break;
        case StatementEventKind::CorporateAction:
            expectedStatement.mTradeInstruments.front().mCorporateActions.clear();
            std::erase(expectedSurvivingIds, "action-id");
            break;
        case StatementEventKind::Dividend:
            expectedStatement.mDividendInstruments.clear();
            std::erase(expectedSurvivingIds, "dividend-id");
            break;
        case StatementEventKind::Interest:
            expectedStatement.mInterestInstruments.clear();
            std::erase(expectedSurvivingIds, "interest-id");
            break;
        case StatementEventKind::Benefit:
            expectedStatement.mBenefitEvents.clear();
            std::erase(expectedSurvivingIds, "benefit-id");
            break;
        case StatementEventKind::PrivateMarket:
            expectedStatement.mPrivateMarketEvents.clear();
            std::erase(expectedSurvivingIds, "private-id");
            break;
        }

        test::expectStatementsEqual(result.mStatement.mPresentation, expectedStatement, false);

        std::vector<std::string> survivingIds;

        for (const auto& event : result.mStatement.mChronologicalOrder)
        {
            EXPECT_NE(event.mKind, aKind);
            const auto& metadata = test::referencedMetadata(result.mStatement, event);

            // Every unaffected transaction keeps both files as its sources.
            switch (event.mKind)
            {
            case StatementEventKind::Trade: {
                const std::vector expectedSources{
                    SourceReference{
                        .mBroker = Broker::TradeRepublic,
                        .mFilename = SourceFilename::fromPath("first.csv"),
                        .mSourceRow = 2,
                        .mTransactionId = "trade-id",
                        .mInputSequence = {.mSourceIndex = 0, .mEventIndex = 0},
                    },
                    SourceReference{
                        .mBroker = Broker::TradeRepublic,
                        .mFilename = SourceFilename::fromPath("overlap.csv"),
                        .mSourceRow = 2,
                        .mTransactionId = "trade-id",
                        .mInputSequence = {.mSourceIndex = 1, .mEventIndex = 0},
                    },
                };

                EXPECT_EQ(metadata.mSources, expectedSources);
                break;
            }
            case StatementEventKind::CorporateAction: {
                const std::vector expectedSources{
                    SourceReference{
                        .mBroker = Broker::TradeRepublic,
                        .mFilename = SourceFilename::fromPath("first.csv"),
                        .mSourceRow = 3,
                        .mTransactionId = "action-id",
                        .mInputSequence = {.mSourceIndex = 0, .mEventIndex = 1},
                    },
                    SourceReference{
                        .mBroker = Broker::TradeRepublic,
                        .mFilename = SourceFilename::fromPath("overlap.csv"),
                        .mSourceRow = 3,
                        .mTransactionId = "action-id",
                        .mInputSequence = {.mSourceIndex = 1, .mEventIndex = 1},
                    },
                };

                EXPECT_EQ(metadata.mSources, expectedSources);
                break;
            }
            case StatementEventKind::Dividend: {
                const std::vector expectedSources{
                    SourceReference{
                        .mBroker = Broker::TradeRepublic,
                        .mFilename = SourceFilename::fromPath("first.csv"),
                        .mSourceRow = 4,
                        .mTransactionId = "dividend-id",
                        .mInputSequence = {.mSourceIndex = 0, .mEventIndex = 2},
                    },
                    SourceReference{
                        .mBroker = Broker::TradeRepublic,
                        .mFilename = SourceFilename::fromPath("overlap.csv"),
                        .mSourceRow = 4,
                        .mTransactionId = "dividend-id",
                        .mInputSequence = {.mSourceIndex = 1, .mEventIndex = 2},
                    },
                };

                EXPECT_EQ(metadata.mSources, expectedSources);
                break;
            }
            case StatementEventKind::Interest: {
                const std::vector expectedSources{
                    SourceReference{
                        .mBroker = Broker::TradeRepublic,
                        .mFilename = SourceFilename::fromPath("first.csv"),
                        .mSourceRow = 5,
                        .mTransactionId = "interest-id",
                        .mInputSequence = {.mSourceIndex = 0, .mEventIndex = 3},
                    },
                    SourceReference{
                        .mBroker = Broker::TradeRepublic,
                        .mFilename = SourceFilename::fromPath("overlap.csv"),
                        .mSourceRow = 5,
                        .mTransactionId = "interest-id",
                        .mInputSequence = {.mSourceIndex = 1, .mEventIndex = 3},
                    },
                };

                EXPECT_EQ(metadata.mSources, expectedSources);
                break;
            }
            case StatementEventKind::Benefit: {
                const std::vector expectedSources{
                    SourceReference{
                        .mBroker = Broker::TradeRepublic,
                        .mFilename = SourceFilename::fromPath("first.csv"),
                        .mSourceRow = 6,
                        .mTransactionId = "benefit-id",
                        .mInputSequence = {.mSourceIndex = 0, .mEventIndex = 4},
                    },
                    SourceReference{
                        .mBroker = Broker::TradeRepublic,
                        .mFilename = SourceFilename::fromPath("overlap.csv"),
                        .mSourceRow = 6,
                        .mTransactionId = "benefit-id",
                        .mInputSequence = {.mSourceIndex = 1, .mEventIndex = 4},
                    },
                };

                EXPECT_EQ(metadata.mSources, expectedSources);
                break;
            }
            case StatementEventKind::PrivateMarket: {
                const std::vector expectedSources{
                    SourceReference{
                        .mBroker = Broker::TradeRepublic,
                        .mFilename = SourceFilename::fromPath("first.csv"),
                        .mSourceRow = 7,
                        .mTransactionId = "private-id",
                        .mInputSequence = {.mSourceIndex = 0, .mEventIndex = 5},
                    },
                    SourceReference{
                        .mBroker = Broker::TradeRepublic,
                        .mFilename = SourceFilename::fromPath("overlap.csv"),
                        .mSourceRow = 7,
                        .mTransactionId = "private-id",
                        .mInputSequence = {.mSourceIndex = 1, .mEventIndex = 5},
                    },
                };

                EXPECT_EQ(metadata.mSources, expectedSources);
                break;
            }
            }

            survivingIds.push_back(primarySource(metadata).mTransactionId.value_or(""));
        }

        EXPECT_EQ(survivingIds, expectedSurvivingIds);

        const auto diagnostics = diagnosticsOf<MergeDiagnostic>(result);
        ASSERT_EQ(diagnostics.size(), 1U);
        EXPECT_EQ(diagnostics.front()->mCode, MergeDiagnosticCode::ConflictingDuplicate);
        EXPECT_EQ(diagnostics.front()->mSeverity, DiagnosticSeverity::Error);
        EXPECT_EQ(diagnostics.front()->mEventKinds, std::vector{aKind});
        ASSERT_EQ(diagnostics.front()->mSources.size(), 2U);
        EXPECT_EQ(diagnostics.front()->mSources[0].mInputSequence.mSourceIndex, 0U);
        EXPECT_EQ(diagnostics.front()->mSources[1].mInputSequence.mSourceIndex, 1U);
    }
}

TEST(DeterministicStatementMergerTest, RejectsEachChangedTradeFact) {
    const std::array<FieldChange<TradeInstrument>, 13> changes{{
        {"tax date",
         [](TradeInstrument& aValue) {
             aValue.mTransactions.front().mMetadata.mTaxDate += DayDuration{1};
         }},
        {"timestamp",
         [](TradeInstrument& aValue) {
             *aValue.mTransactions.front().mMetadata.mSourceTimestamp += std::chrono::seconds{1};
         }},
        {"missing timestamp",
         [](TradeInstrument& aValue) {
             aValue.mTransactions.front().mMetadata.mSourceTimestamp.reset();
         }},
        {"ISIN", [](TradeInstrument& aValue) { aValue.mIsin = "XX0000000099"; }},
        {"asset class", [](TradeInstrument& aValue) { aValue.mAssetClass = AssetClass::Fund; }},
        {"side",
         [](TradeInstrument& aValue) { aValue.mTransactions.front().mTradeSide = TradeSide::Buy; }},
        {"unit price", [](TradeInstrument& aValue) { ++aValue.mTransactions.front().mUnitPrice; }},
        {"units", [](TradeInstrument& aValue) { ++aValue.mTransactions.front().mUnits; }},
        {"amount", [](TradeInstrument& aValue) { ++*aValue.mTransactions.front().mAmount; }},
        {"missing amount",
         [](TradeInstrument& aValue) { aValue.mTransactions.front().mAmount.reset(); }},
        {"fee", [](TradeInstrument& aValue) { ++aValue.mTransactions.front().mFeePaid; }},
        {"exchange rate",
         [](TradeInstrument& aValue) { ++aValue.mTransactions.front().mExchangeRate; }},
        {"currency",
         [](TradeInstrument& aValue) { aValue.mTransactions.front().mCurrency = Currency::GBP; }},
    }};

    expectFieldConflicts(
        StatementEventKind::Trade,
        changes,
        [](BrokerStatement& aStatement) -> auto& { return aStatement.mTradeInstruments.front(); });
}

TEST(DeterministicStatementMergerTest, RejectsEachChangedCorporateActionFact) {
    const std::array<FieldChange<TradeInstrument>, 7> changes{{
        {"ISIN", [](TradeInstrument& aValue) { aValue.mIsin = "XX0000000099"; }},
        {"asset class", [](TradeInstrument& aValue) { aValue.mAssetClass = AssetClass::Fund; }},
        {"type",
         [](TradeInstrument& aValue) {
             aValue.mCorporateActions.front().mType = CorporateActionType::Split;
         }},
        {"units delta",
         [](TradeInstrument& aValue) { ++aValue.mCorporateActions.front().mUnitsDelta; }},
        {"ratio", [](TradeInstrument& aValue) { ++*aValue.mCorporateActions.front().mRatio; }},
        {"missing ratio",
         [](TradeInstrument& aValue) { aValue.mCorporateActions.front().mRatio.reset(); }},
        {"timestamp",
         [](TradeInstrument& aValue) {
             aValue.mCorporateActions.front().mMetadata.mSourceTimestamp =
                 makeTimestamp(2024, 1, 2, 9);
         }},
    }};

    expectFieldConflicts(
        StatementEventKind::CorporateAction,
        changes,
        [](BrokerStatement& aStatement) -> auto& { return aStatement.mTradeInstruments.front(); });
}

TEST(DeterministicStatementMergerTest, RejectsEachChangedDividendFact) {
    const std::array<FieldChange<DividendInstrument>, 8> changes{{
        {"ISIN", [](DividendInstrument& aValue) { aValue.mIsin = "XX0000000099"; }},
        {"gross", [](DividendInstrument& aValue) { ++aValue.mTransactions.front().mGrossAmount; }},
        {"tax", [](DividendInstrument& aValue) { ++*aValue.mTransactions.front().mTaxPaid; }},
        {"missing tax",
         [](DividendInstrument& aValue) { aValue.mTransactions.front().mTaxPaid.reset(); }},
        {"missing exchange rate",
         [](DividendInstrument& aValue) { aValue.mTransactions.front().mExchangeRate.reset(); }},
        {"exchange rate",
         [](DividendInstrument& aValue) { ++*aValue.mTransactions.front().mExchangeRate; }},
        {"currency",
         [](DividendInstrument& aValue) {
             aValue.mTransactions.front().mCurrency = Currency::GBP;
         }},
        {"tax currency",
         [](DividendInstrument& aValue) {
             aValue.mTransactions.front().mTaxCurrency = Currency::USD;
         }},
    }};

    expectFieldConflicts(StatementEventKind::Dividend,
                         changes,
                         [](BrokerStatement& aStatement) -> auto& {
                             return aStatement.mDividendInstruments.front();
                         });
}

TEST(DeterministicStatementMergerTest, RejectsEachChangedInterestFact) {
    const std::array<FieldChange<InterestInstrument>, 10> changes{{
        {"name without ISIN", [](InterestInstrument& aValue) { aValue.mName = "Other payer"; }},
        {"ISIN presence", [](InterestInstrument& aValue) { aValue.mIsin = "XX0000000099"; }},
        {"type",
         [](InterestInstrument& aValue) { aValue.mInterestType = InterestType::OtherInterest; }},
        {"gross", [](InterestInstrument& aValue) { ++aValue.mTransactions.front().mGrossAmount; }},
        {"tax", [](InterestInstrument& aValue) { ++*aValue.mTransactions.front().mTaxPaid; }},
        {"missing tax",
         [](InterestInstrument& aValue) { aValue.mTransactions.front().mTaxPaid.reset(); }},
        {"missing exchange rate",
         [](InterestInstrument& aValue) { aValue.mTransactions.front().mExchangeRate.reset(); }},
        {"exchange rate",
         [](InterestInstrument& aValue) { ++*aValue.mTransactions.front().mExchangeRate; }},
        {"currency",
         [](InterestInstrument& aValue) {
             aValue.mTransactions.front().mCurrency = Currency::USD;
         }},
        {"tax currency",
         [](InterestInstrument& aValue) {
             aValue.mTransactions.front().mTaxCurrency = Currency::USD;
         }},
    }};

    expectFieldConflicts(StatementEventKind::Interest,
                         changes,
                         [](BrokerStatement& aStatement) -> auto& {
                             return aStatement.mInterestInstruments.front();
                         });
}

TEST(DeterministicStatementMergerTest, UnknownDividendWithholdingConflictsWithExplicitZero) {
    auto first = makeDuplicateTestInput(0, "first.csv");
    auto overlap = makeDuplicateTestInput(1, "overlap.csv");
    first.mParseResult.mStatement.mDividendInstruments.front().mTransactions.front().mTaxPaid = 0;
    overlap.mParseResult.mStatement.mDividendInstruments.front()
        .mTransactions.front()
        .mTaxPaid.reset();
    const std::array inputs{first, overlap};

    const auto result = DeterministicStatementMerger{}.merge(inputs);
    const auto* conflict = findMergeDiagnostic(result, MergeDiagnosticCode::ConflictingDuplicate);

    ASSERT_NE(conflict, nullptr);
    EXPECT_EQ(conflict->mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(conflict->mEventKinds, std::vector{StatementEventKind::Dividend});
    ASSERT_EQ(conflict->mSources.size(), 2U);
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 5U);

    std::vector<std::string> survivingIds;

    for (const auto& event : result.mStatement.mChronologicalOrder)
    {
        EXPECT_NE(event.mKind, StatementEventKind::Dividend);
        const auto& metadata = test::referencedMetadata(result.mStatement, event);

        survivingIds.push_back(primarySource(metadata).mTransactionId.value_or(""));
    }

    const std::vector<std::string> expectedIds{
        "trade-id",
        "action-id",
        "interest-id",
        "benefit-id",
        "private-id",
    };

    EXPECT_EQ(survivingIds, expectedIds);
}

TEST(DeterministicStatementMergerTest, UnknownInterestWithholdingConflictsWithExplicitZero) {
    auto first = makeDuplicateTestInput(0, "first.csv");
    auto overlap = makeDuplicateTestInput(1, "overlap.csv");
    first.mParseResult.mStatement.mInterestInstruments.front().mTransactions.front().mTaxPaid = 0;
    overlap.mParseResult.mStatement.mInterestInstruments.front()
        .mTransactions.front()
        .mTaxPaid.reset();
    const std::array inputs{first, overlap};

    const auto result = DeterministicStatementMerger{}.merge(inputs);
    const auto* conflict = findMergeDiagnostic(result, MergeDiagnosticCode::ConflictingDuplicate);

    ASSERT_NE(conflict, nullptr);
    EXPECT_EQ(conflict->mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(conflict->mEventKinds, std::vector{StatementEventKind::Interest});
    ASSERT_EQ(conflict->mSources.size(), 2U);
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 5U);

    std::vector<std::string> survivingIds;

    for (const auto& event : result.mStatement.mChronologicalOrder)
    {
        EXPECT_NE(event.mKind, StatementEventKind::Interest);
        const auto& metadata = test::referencedMetadata(result.mStatement, event);

        survivingIds.push_back(primarySource(metadata).mTransactionId.value_or(""));
    }

    const std::vector<std::string> expectedIds{
        "trade-id",
        "action-id",
        "dividend-id",
        "benefit-id",
        "private-id",
    };

    EXPECT_EQ(survivingIds, expectedIds);
}

TEST(DeterministicStatementMergerTest, RejectsEachChangedBenefitFact) {
    const std::array<FieldChange<BenefitEvent>, 6> changes{{
        {"type", [](BenefitEvent& aValue) { aValue.mType = BenefitType::Saveback; }},
        {"name", [](BenefitEvent& aValue) { aValue.mName = "Other reward"; }},
        {"ISIN", [](BenefitEvent& aValue) { aValue.mIsin.reset(); }},
        {"asset class", [](BenefitEvent& aValue) { aValue.mAssetClass = AssetClass::Fund; }},
        {"amount", [](BenefitEvent& aValue) { ++aValue.mAmount; }},
        {"currency", [](BenefitEvent& aValue) { aValue.mCurrency = Currency::USD; }},
    }};

    expectFieldConflicts(
        StatementEventKind::Benefit,
        changes,
        [](BrokerStatement& aStatement) -> auto& { return aStatement.mBenefitEvents.front(); });
}

TEST(DeterministicStatementMergerTest, RejectsEachChangedPrivateMarketFact) {
    const std::array<FieldChange<PrivateMarketEvent>, 8> changes{{
        {"type", [](PrivateMarketEvent& aValue) { aValue.mType = PrivateMarketEventType::Buy; }},
        {"name", [](PrivateMarketEvent& aValue) { aValue.mName = "Other fund"; }},
        {"ISIN", [](PrivateMarketEvent& aValue) { aValue.mIsin.reset(); }},
        {"asset class", [](PrivateMarketEvent& aValue) { aValue.mAssetClass = AssetClass::Fund; }},
        {"amount", [](PrivateMarketEvent& aValue) { ++aValue.mAmount; }},
        {"fee", [](PrivateMarketEvent& aValue) { ++aValue.mFeePaid; }},
        {"currency", [](PrivateMarketEvent& aValue) { aValue.mCurrency = Currency::USD; }},
        {"description",
         [](PrivateMarketEvent& aValue) { aValue.mDescription = "Different description"; }},
    }};

    expectFieldConflicts(StatementEventKind::PrivateMarket,
                         changes,
                         [](BrokerStatement& aStatement) -> auto& {
                             return aStatement.mPrivateMarketEvents.front();
                         });
}

TEST(DeterministicStatementMergerTest, DuplicateAndConflictResultsIgnoreInputArrivalOrder) {
    std::array inputs{makeDuplicateTestInput(0, "first.csv"),
                      makeDuplicateTestInput(1, "second.csv"),
                      makeDuplicateTestInput(2, "third.csv")};
    inputs[1].mParseResult.mStatement.mTradeInstruments.front().mName = "Different display name";
    inputs[2].mParseResult.mStatement.mPrivateMarketEvents.front().mFeePaid += 1;
    const auto originalFirst = inputs.front()
                                   .mParseResult.mStatement.mTradeInstruments.front()
                                   .mTransactions.front()
                                   .mMetadata;
    const auto expected = DeterministicStatementMerger{}.merge(inputs);

    do
    {
        const auto actual = DeterministicStatementMerger{}.merge(inputs);

        test::expectMergeResultsEqual(actual, expected);

        EXPECT_EQ(actual.mStatement.mChronologicalOrder, expected.mStatement.mChronologicalOrder);
        ASSERT_EQ(actual.mStatement.mChronologicalOrder.size(), 5U);

        for (const auto& reference : actual.mStatement.mChronologicalOrder)
        {
            EXPECT_EQ(referencedMetadata(actual.mStatement, reference),
                      referencedMetadata(expected.mStatement, reference));
        }

        EXPECT_EQ(mergedEventCount(actual.mStatement.mPresentation), 5U);
        EXPECT_TRUE(actual.mStatement.mPresentation.mPrivateMarketEvents.empty());
        ASSERT_EQ(actual.mStatement.mPresentation.mTradeInstruments.size(), 1U);
        const auto& instrument = actual.mStatement.mPresentation.mTradeInstruments.front();

        EXPECT_EQ(instrument.mName, "Security");
        ASSERT_EQ(instrument.mTransactions.size(), 1U);
        EXPECT_EQ(instrument.mTransactions.front().mMetadata,
                  expected.mStatement.mPresentation.mTradeInstruments.front()
                      .mTransactions.front()
                      .mMetadata);
        EXPECT_EQ(instrument.mTransactions.front().mMetadata.mSources.size(), 3U);

        const auto diagnostics = diagnosticsOf<MergeDiagnostic>(actual);
        const auto expectedDiagnostics = diagnosticsOf<MergeDiagnostic>(expected);
        ASSERT_EQ(diagnostics.size(), expectedDiagnostics.size());

        for (std::size_t index = 0; index < diagnostics.size(); ++index)
        {
            EXPECT_EQ(diagnostics[index]->mCode, expectedDiagnostics[index]->mCode);
            EXPECT_EQ(diagnostics[index]->mSources, expectedDiagnostics[index]->mSources);
            EXPECT_EQ(diagnostics[index]->mEventKinds, expectedDiagnostics[index]->mEventKinds);
        }
    } while (std::next_permutation(inputs.begin(),
                                   inputs.end(),
                                   [](const auto& aLeft, const auto& aRight) {
                                       return aLeft.mSourceIndex < aRight.mSourceIndex;
                                   }));

    EXPECT_EQ(inputs.front()
                  .mParseResult.mStatement.mTradeInstruments.front()
                  .mTransactions.front()
                  .mMetadata,
              originalFirst);
}

TEST(DeterministicStatementMergerTest, KeepsIncomeNameSourcesAfterDeduplicationByIsin) {
    auto first = makeDuplicateTestInput(0, "first.csv");
    auto second = makeDuplicateTestInput(1, "second.csv");
    first.mParseResult.mStatement.mInterestInstruments.front().mIsin = "XX0000000042";
    second.mParseResult.mStatement.mInterestInstruments.front().mIsin = "XX0000000042";
    second.mParseResult.mStatement.mInterestInstruments.front().mName = "Other bond name";
    second.mParseResult.mStatement.mDividendInstruments.front().mName = "Other issuer name";
    const std::array inputs{second, first};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    const auto& dividends = result.mStatement.mPresentation.mDividendInstruments;
    const auto& interest = result.mStatement.mPresentation.mInterestInstruments;
    ASSERT_EQ(dividends.size(), 1U);
    ASSERT_EQ(interest.size(), 1U);
    ASSERT_EQ(dividends.front().mTransactions.size(), 1U);
    ASSERT_EQ(interest.front().mTransactions.size(), 1U);
    EXPECT_EQ(dividends.front().mName, "Security");
    EXPECT_EQ(interest.front().mName, "Cash account");
    EXPECT_EQ(dividends.front().mTransactions.front().mMetadata.mSources.size(), 2U);
    EXPECT_EQ(interest.front().mTransactions.front().mMetadata.mSources.size(), 2U);

    const auto diagnostics = diagnosticsOf<MergeDiagnostic>(result);
    ASSERT_EQ(diagnostics.size(), 2U);

    for (const auto* diagnostic : diagnostics)
    {
        EXPECT_EQ(diagnostic->mCode, MergeDiagnosticCode::InstrumentNameConflict);
        ASSERT_EQ(diagnostic->mNameVariants.size(), 2U);

        for (const auto& variant : diagnostic->mNameVariants)
        {
            ASSERT_EQ(variant.mSources.size(), 1U);
            EXPECT_EQ(variant.mSources.front().mInputSequence.mSourceIndex,
                      variant.mName.starts_with("Other") ? 1U : 0U);
        }
    }
}

TEST(DeterministicStatementMergerTest, NormalizesOverlappingSourceSetsOnExactCopies) {
    auto first = makeTrade(0, 0, makeDate(2024, 1, 1), Broker::TradeRepublic, "same.csv", "id");
    auto second = makeTrade(0, 1, makeDate(2024, 1, 1), Broker::TradeRepublic, "same.csv", "id");
    const auto shared = makeSource(Broker::TradeRepublic, "same.csv", 4, "id", {0, 2});
    first.mMetadata.mSources.push_back(shared);
    second.mMetadata.mSources.push_back(shared);
    StatementMergeInput input{.mSourceIndex = 0};
    input.mParseResult.mStatement.mTradeInstruments.push_back(TradeInstrument{
        .mName = "Security",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions = {second, first},
    });

    const auto result = DeterministicStatementMerger{}.merge(std::span{&input, 1U});

    const auto& transactions =
        result.mStatement.mPresentation.mTradeInstruments.front().mTransactions;

    ASSERT_EQ(transactions.size(), 1U);
    const auto& sources = transactions.front().mMetadata.mSources;

    ASSERT_EQ(sources.size(), 3U);
    EXPECT_EQ(sources[0].mInputSequence.mEventIndex, 0U);
    EXPECT_EQ(sources[1].mInputSequence.mEventIndex, 1U);
    EXPECT_EQ(sources[2], shared);
}

TEST(DeterministicStatementMergerTest, DeduplicatesWithinOneFileButNotEventsWithoutIds) {
    const auto date = makeDate(2024, 2, 1);
    StatementMergeInput input{.mSourceIndex = 0};
    input.mParseResult.mStatement.mTradeInstruments.push_back(TradeInstrument{
        .mName = "Security",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                makeTrade(0, 0, date, Broker::TradeRepublic, "one.csv", "duplicate-id"),
                makeTrade(0, 1, date, Broker::TradeRepublic, "one.csv", "duplicate-id"),
                makeTrade(0, 2, date),
                makeTrade(0, 3, date),
            },
    });

    const auto result = DeterministicStatementMerger{}.merge(std::span{&input, 1U});

    const auto& transactions =
        result.mStatement.mPresentation.mTradeInstruments.front().mTransactions;
    ASSERT_EQ(transactions.size(), 3U);
    EXPECT_EQ(transactions.front().mMetadata.mSources.size(), 2U);
    EXPECT_EQ(transactions[1].mMetadata.mSources.size(), 1U);
    EXPECT_EQ(transactions[2].mMetadata.mSources.size(), 1U);
}

TEST(DeterministicStatementMergerTest, ScopesTransactionIdsByBroker) {
    StatementMergeInput input{.mSourceIndex = 0};
    input.mParseResult.mStatement.mTradeInstruments.push_back(TradeInstrument{
        .mName = "Security",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {
                makeTrade(0, 0, makeDate(2024, 2, 1), Broker::TradeRepublic, "tr.csv", "shared-id"),
                makeTrade(0,
                          1,
                          makeDate(2024, 2, 1),
                          Broker::InteractiveBrokers,
                          "ibkr.csv",
                          "shared-id"),
            },
    });

    const auto result = DeterministicStatementMerger{}.merge(std::span{&input, 1U});

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);
    EXPECT_EQ(result.mStatement.mPresentation.mTradeInstruments.front().mTransactions.size(), 2U);
    EXPECT_EQ(findMergeDiagnostic(result, MergeDiagnosticCode::ConflictingDuplicate), nullptr);
}

TEST(DeterministicStatementMergerTest, RejectsEveryCandidateForAConflictingTransactionId) {
    StatementMergeInput input{.mSourceIndex = 0};
    auto first =
        makeTrade(0, 0, makeDate(2024, 3, 1), Broker::TradeRepublic, "one.csv", "conflict-id");
    auto exactCopy =
        makeTrade(0, 1, makeDate(2024, 3, 1), Broker::TradeRepublic, "one.csv", "conflict-id");
    auto conflicting =
        makeTrade(0, 2, makeDate(2024, 3, 1), Broker::TradeRepublic, "one.csv", "conflict-id");
    conflicting.mFeePaid = 100;
    input.mParseResult.mStatement.mTradeInstruments.push_back(TradeInstrument{
        .mName = "Security",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions = {first, exactCopy, conflicting},
    });

    const auto result = DeterministicStatementMerger{}.merge(std::span{&input, 1U});

    EXPECT_TRUE(result.mStatement.mPresentation.mTradeInstruments.empty());
    const auto* diagnostic = findMergeDiagnostic(result, MergeDiagnosticCode::ConflictingDuplicate);
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_EQ(diagnostic->mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic->mTaxDate, makeDate(2024, 3, 1));
    EXPECT_EQ(diagnostic->mInstrumentName, "Security");
    EXPECT_EQ(diagnostic->mIsin, "XX0000000001");
    ASSERT_EQ(diagnostic->mEventKinds.size(), 1U);
    EXPECT_EQ(diagnostic->mEventKinds.front(), StatementEventKind::Trade);
    EXPECT_EQ(diagnostic->mSources.size(), 3U);
}

TEST(DeterministicStatementMergerTest, RejectsCrossKindTransactionIdConflicts) {
    StatementMergeInput input{.mSourceIndex = 0};
    input.mParseResult.mStatement.mTradeInstruments.push_back(TradeInstrument{
        .mName = "Security",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions = {makeTrade(0,
                                    0,
                                    makeDate(2024, 4, 1),
                                    Broker::TradeRepublic,
                                    "one.csv",
                                    "cross-kind-id")},
    });
    input.mParseResult.mStatement.mDividendInstruments.push_back(DividendInstrument{
        .mName = "Other issuer",
        .mIsin = "XX0000000002",
        .mTransactions = {DividendTransaction{
            .mMetadata = makeMetadata(0,
                                      1,
                                      makeDate(2024, 4, 2),
                                      Broker::TradeRepublic,
                                      "one.csv",
                                      std::nullopt,
                                      "cross-kind-id"),
            .mGrossAmount = 1000,
        }},
    });

    const auto result = DeterministicStatementMerger{}.merge(std::span{&input, 1U});

    EXPECT_TRUE(result.mStatement.mPresentation.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mPresentation.mDividendInstruments.empty());
    const auto* diagnostic = findMergeDiagnostic(result, MergeDiagnosticCode::ConflictingDuplicate);
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_FALSE(diagnostic->mTaxDate.has_value());
    EXPECT_FALSE(diagnostic->mInstrumentName.has_value());
    EXPECT_FALSE(diagnostic->mIsin.has_value());
    ASSERT_EQ(diagnostic->mEventKinds.size(), 2U);
    EXPECT_EQ(diagnostic->mEventKinds[0], StatementEventKind::Trade);
    EXPECT_EQ(diagnostic->mEventKinds[1], StatementEventKind::Dividend);
    EXPECT_EQ(diagnostic->mSources.size(), 2U);
}

TEST(DeterministicStatementMergerTest, KeepsNameProvenanceWhenExactCopiesUseDifferentNames) {
    StatementMergeInput first{.mSourceIndex = 0};
    first.mParseResult.mStatement.mTradeInstruments.push_back(TradeInstrument{
        .mName = "Earlier name",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {makeTrade(0, 0, makeDate(2024, 5, 1), Broker::TradeRepublic, "first.csv", "same-id")},
    });
    StatementMergeInput second{.mSourceIndex = 1};
    second.mParseResult.mStatement.mTradeInstruments.push_back(TradeInstrument{
        .mName = "Later name",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions =
            {makeTrade(1, 0, makeDate(2024, 5, 1), Broker::TradeRepublic, "second.csv", "same-id")},
    });
    const std::array inputs{second, first};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments.front();
    EXPECT_EQ(instrument.mName, "Earlier name");
    ASSERT_EQ(instrument.mTransactions.size(), 1U);
    EXPECT_EQ(instrument.mTransactions.front().mMetadata.mSources.size(), 2U);
    const auto* warning = findMergeDiagnostic(result, MergeDiagnosticCode::InstrumentNameConflict);
    ASSERT_NE(warning, nullptr);
    ASSERT_EQ(warning->mNameVariants.size(), 2U);
    EXPECT_EQ(warning->mNameVariants[0].mSources.size(), 1U);
    EXPECT_EQ(warning->mNameVariants[1].mSources.size(), 1U);
}

TEST(DeterministicStatementMergerTest, DoesNotUseTransactionIdsForCorporateActionEconomics) {
    StatementMergeInput input{.mSourceIndex = 0};
    input.mParseResult.mStatement.mTradeInstruments.push_back(TradeInstrument{
        .mName = "Security",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mCorporateActions =
            {
                CorporateAction{
                    .mMetadata = makeMetadata(0,
                                              0,
                                              makeDate(2024, 6, 1),
                                              Broker::TradeRepublic,
                                              "one.csv",
                                              std::nullopt,
                                              "first-action"),
                    .mType = CorporateActionType::Split,
                    .mUnitsDelta = UNITS_SCALE,
                    .mRatio = 200000000,
                },
                CorporateAction{
                    .mMetadata = makeMetadata(0,
                                              1,
                                              makeDate(2024, 6, 1),
                                              Broker::TradeRepublic,
                                              "one.csv",
                                              std::nullopt,
                                              "second-action"),
                    .mType = CorporateActionType::Split,
                    .mUnitsDelta = UNITS_SCALE,
                    .mRatio = 200000000,
                },
            },
    });

    const auto result = DeterministicStatementMerger{}.merge(std::span{&input, 1U});

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);
    EXPECT_EQ(result.mStatement.mPresentation.mTradeInstruments.front().mCorporateActions.size(),
              2U);
    EXPECT_EQ(findMergeDiagnostic(result, MergeDiagnosticCode::ConflictingDuplicate), nullptr);
}

TEST(DeterministicStatementMergerTest, MergesDividendsAndPreservesNameConflictProvenance) {
    StatementMergeInput later{.mSourceIndex = 1};
    later.mParseResult.mStatement.mDividendInstruments.push_back(DividendInstrument{
        .mName = "Issuer PLC",
        .mIsin = "XX0000000001",
        .mTransactions = {DividendTransaction{
            .mMetadata = makeMetadata(1, 0, makeDate(2024, 6, 2)),
            .mGrossAmount = 10000,
        }},
    });
    StatementMergeInput earlier{.mSourceIndex = 0};
    earlier.mParseResult.mStatement.mDividendInstruments.push_back(DividendInstrument{
        .mName = "Issuer",
        .mIsin = "XX0000000001",
        .mTransactions = {DividendTransaction{
            .mMetadata = makeMetadata(0, 0, makeDate(2024, 6, 1)),
            .mGrossAmount = 20000,
        }},
    });
    const std::array inputs{later, earlier};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    const auto& instruments = result.mStatement.mPresentation.mDividendInstruments;
    ASSERT_EQ(instruments.size(), 1U);
    EXPECT_EQ(instruments.front().mName, "Issuer");
    ASSERT_EQ(instruments.front().mTransactions.size(), 2U);
    EXPECT_EQ(instruments.front().mTransactions.front().mGrossAmount, 20000);
    const auto* conflict = findMergeDiagnostic(result, MergeDiagnosticCode::InstrumentNameConflict);
    ASSERT_NE(conflict, nullptr);
    EXPECT_EQ(conflict->mSources.size(), 2U);
}

TEST(DeterministicStatementMergerTest, UsesBrokerNeutralInterestIdentitiesAndOrdering) {
    StatementMergeInput input{.mSourceIndex = 0};
    auto& instruments = input.mParseResult.mStatement.mInterestInstruments;
    instruments.push_back(InterestInstrument{
        .mName = "Bank B",
        .mInterestType = InterestType::BrokerInterest,
        .mTransactions = {InterestTransaction{
            .mMetadata = makeMetadata(0, 4, makeDate(2024, 5, 2)),
        }},
    });
    instruments.push_back(InterestInstrument{
        .mName = "Bond Z",
        .mIsin = "XX0000000002",
        .mInterestType = InterestType::BondInterest,
        .mTransactions = {InterestTransaction{
            .mMetadata = makeMetadata(0, 3, makeDate(2024, 4, 2)),
        }},
    });
    instruments.push_back(InterestInstrument{
        .mName = "Bank A",
        .mInterestType = InterestType::BrokerInterest,
        .mTransactions = {InterestTransaction{
            .mMetadata = makeMetadata(0, 2, makeDate(2024, 5, 1)),
        }},
    });
    instruments.push_back(InterestInstrument{
        .mName = "Bond A",
        .mIsin = "XX0000000001",
        .mInterestType = InterestType::BondInterest,
        .mTransactions = {InterestTransaction{
            .mMetadata = makeMetadata(0, 1, makeDate(2024, 4, 1)),
        }},
    });
    instruments.push_back(InterestInstrument{
        .mName = "Other income",
        .mInterestType = InterestType::OtherInterest,
        .mTransactions = {InterestTransaction{
            .mMetadata = makeMetadata(0, 5, makeDate(2024, 7, 1)),
        }},
    });
    instruments.push_back(InterestInstrument{
        .mName = "Unclassified income",
        .mInterestType = InterestType::UnknownInterest,
        .mTransactions = {InterestTransaction{
            .mMetadata = makeMetadata(0, 6, makeDate(2024, 8, 1)),
        }},
    });
    instruments.push_back(InterestInstrument{
        .mName = "Bond without ISIN",
        .mInterestType = InterestType::BondInterest,
        .mTransactions = {InterestTransaction{
            .mMetadata = makeMetadata(0, 7, makeDate(2024, 4, 3)),
        }},
    });

    const auto result = DeterministicStatementMerger{}.merge(std::span{&input, 1U});

    const auto& merged = result.mStatement.mPresentation.mInterestInstruments;
    ASSERT_EQ(merged.size(), 7U);
    EXPECT_EQ(merged[0].mIsin, "XX0000000001");
    EXPECT_EQ(merged[1].mIsin, "XX0000000002");
    EXPECT_EQ(merged[2].mName, "Bond without ISIN");
    EXPECT_EQ(merged[3].mName, "Bank A");
    EXPECT_EQ(merged[4].mName, "Bank B");
    EXPECT_EQ(merged[5].mInterestType, InterestType::OtherInterest);
    EXPECT_EQ(merged[6].mInterestType, InterestType::UnknownInterest);
}

TEST(DeterministicStatementMergerTest, MergesBondInterestByIsinAndWarnsOnNames) {
    StatementMergeInput first{.mSourceIndex = 0};
    first.mParseResult.mStatement.mInterestInstruments.push_back(InterestInstrument{
        .mName = "First bond name",
        .mIsin = "XX0000000001",
        .mInterestType = InterestType::BondInterest,
        .mTransactions = {InterestTransaction{
            .mMetadata = makeMetadata(0, 0, makeDate(2024, 3, 2)),
        }},
    });
    StatementMergeInput second{.mSourceIndex = 1};
    second.mParseResult.mStatement.mInterestInstruments.push_back(InterestInstrument{
        .mName = "Second bond name",
        .mIsin = "XX0000000001",
        .mInterestType = InterestType::BondInterest,
        .mTransactions = {InterestTransaction{
            .mMetadata = makeMetadata(1, 0, makeDate(2024, 3, 1)),
        }},
    });
    const std::array inputs{second, first};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mInterestInstruments.size(), 1U);
    EXPECT_EQ(result.mStatement.mPresentation.mInterestInstruments.front().mName,
              "First bond name");
    EXPECT_EQ(result.mStatement.mPresentation.mInterestInstruments.front().mTransactions.size(),
              2U);
    EXPECT_NE(findMergeDiagnostic(result, MergeDiagnosticCode::InstrumentNameConflict), nullptr);
}

TEST(DeterministicStatementMergerTest, CombinesAndChronologicallyOrdersStandaloneEvents) {
    StatementMergeInput later{.mSourceIndex = 1};
    later.mParseResult.mBroker = Broker::InteractiveBrokers;
    later.mParseResult.mStatement.mBenefitEvents.push_back(BenefitEvent{
        .mMetadata =
            makeMetadata(1, 1, makeDate(2024, 2, 1), Broker::InteractiveBrokers, "ibkr.csv"),
        .mType = BenefitType::StockPerk,
        .mName = "Later benefit",
    });
    later.mParseResult.mStatement.mPrivateMarketEvents.push_back(PrivateMarketEvent{
        .mMetadata =
            makeMetadata(1, 0, makeDate(2024, 4, 1), Broker::InteractiveBrokers, "ibkr.csv"),
        .mType = PrivateMarketEventType::Sell,
        .mName = "Later private event",
    });
    StatementMergeInput earlier{.mSourceIndex = 0};
    earlier.mParseResult.mStatement.mBenefitEvents.push_back(BenefitEvent{
        .mMetadata = makeMetadata(0, 1, makeDate(2024, 1, 1)),
        .mType = BenefitType::Saveback,
        .mName = "Earlier benefit",
    });
    earlier.mParseResult.mStatement.mPrivateMarketEvents.push_back(PrivateMarketEvent{
        .mMetadata = makeMetadata(0, 0, makeDate(2024, 3, 1)),
        .mType = PrivateMarketEventType::Buy,
        .mName = "Earlier private event",
    });
    const std::array inputs{later, earlier};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    const auto& benefits = result.mStatement.mPresentation.mBenefitEvents;
    const auto& privateEvents = result.mStatement.mPresentation.mPrivateMarketEvents;
    ASSERT_EQ(benefits.size(), 2U);
    ASSERT_EQ(privateEvents.size(), 2U);
    EXPECT_EQ(benefits[0].mName, "Earlier benefit");
    EXPECT_EQ(benefits[1].mName, "Later benefit");
    EXPECT_EQ(privateEvents[0].mName, "Earlier private event");
    EXPECT_EQ(privateEvents[1].mName, "Later private event");
}

TEST(DeterministicStatementMergerTest, PreservesAndNormalizesExistingSourceReferences) {
    auto event = makeTrade(0, 2, makeDate(2024, 1, 1));
    const auto earlierSource = makeSource(Broker::TradeRepublic,
                                          "earlier.csv",
                                          2,
                                          "event-id",
                                          {.mSourceIndex = 0, .mEventIndex = 0});
    event.mMetadata.mSources.insert(event.mMetadata.mSources.begin(), earlierSource);
    event.mMetadata.mSources.push_back(earlierSource);
    StatementMergeInput input{.mSourceIndex = 0};
    input.mParseResult.mStatement.mTradeInstruments.push_back(TradeInstrument{
        .mName = "Synthetic instrument",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions = {event},
    });

    const auto result = DeterministicStatementMerger{}.merge(std::span{&input, 1U});

    const auto& sources = result.mStatement.mPresentation.mTradeInstruments.front()
                              .mTransactions.front()
                              .mMetadata.mSources;
    ASSERT_EQ(sources.size(), 2U);
    EXPECT_EQ(sources[0].mInputSequence.mEventIndex, 0U);
    EXPECT_EQ(sources[1].mInputSequence.mEventIndex, 2U);
}

TEST(DeterministicStatementMergerTest, RejectsEveryInputWithDuplicateSourceIndex) {
    StatementMergeInput first{.mSourceIndex = 0};
    first.mParseResult.mStatement.mBenefitEvents.push_back(BenefitEvent{
        .mMetadata = makeMetadata(0, 0, makeDate(2024, 1, 1)),
        .mName = "First",
    });
    first.mParseResult.mStatement.mTradeInstruments.push_back(TradeInstrument{
        .mName = "Trade",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions = {makeTrade(0, 2, makeDate(2024, 1, 3))},
        .mCorporateActions = {CorporateAction{
            .mMetadata = makeMetadata(0, 3, makeDate(2024, 1, 4)),
            .mType = CorporateActionType::Split,
            .mUnitsDelta = UNITS_SCALE,
        }},
    });
    first.mParseResult.mStatement.mDividendInstruments.push_back(DividendInstrument{
        .mName = "Dividend",
        .mIsin = "XX0000000002",
        .mTransactions = {DividendTransaction{
            .mMetadata = makeMetadata(0, 4, makeDate(2024, 1, 5)),
        }},
    });
    StatementMergeInput second{.mSourceIndex = 0};
    second.mParseResult.mStatement.mPrivateMarketEvents.push_back(PrivateMarketEvent{
        .mMetadata = makeMetadata(0, 1, makeDate(2024, 1, 2)),
        .mName = "Second",
    });
    second.mParseResult.mStatement.mInterestInstruments.push_back(InterestInstrument{
        .mName = "Interest",
        .mInterestType = InterestType::BrokerInterest,
        .mTransactions = {InterestTransaction{
            .mMetadata = makeMetadata(0, 5, makeDate(2024, 1, 6)),
        }},
    });
    const std::array inputs{first, second};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    EXPECT_TRUE(result.mStatement.mPresentation.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPresentation.mPrivateMarketEvents.empty());
    const auto* diagnostic = findMergeDiagnostic(result, MergeDiagnosticCode::DuplicateSourceIndex);
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_EQ(diagnostic->mSourceIndex, 0U);
    EXPECT_EQ(diagnostic->mSources.size(), 6U);
}

TEST(DeterministicStatementMergerTest, RejectsOnlyEventsWithMissingOrMismatchedSources) {
    StatementMergeInput input{.mSourceIndex = 0};
    auto valid = makeTrade(0, 0, makeDate(2024, 1, 1));
    auto mismatched = makeTrade(1, 1, makeDate(2024, 1, 2));
    TradeTransaction missingSource{
        .mMetadata = EventMetadata{.mTaxDate = makeDate(2024, 1, 3)},
    };
    input.mParseResult.mStatement.mTradeInstruments.push_back(TradeInstrument{
        .mName = "Synthetic instrument",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions = {valid, mismatched, missingSource},
        .mCorporateActions = {CorporateAction{
            .mMetadata = makeMetadata(1, 2, makeDate(2024, 1, 4)),
            .mType = CorporateActionType::Split,
            .mUnitsDelta = UNITS_SCALE,
        }},
    });
    input.mParseResult.mStatement.mDividendInstruments.push_back(DividendInstrument{
        .mName = "Synthetic dividend",
        .mIsin = "XX0000000002",
        .mTransactions = {DividendTransaction{
            .mMetadata = makeMetadata(1, 3, makeDate(2024, 1, 5)),
        }},
    });
    input.mParseResult.mStatement.mInterestInstruments.push_back(InterestInstrument{
        .mName = "Synthetic interest",
        .mInterestType = InterestType::BrokerInterest,
        .mTransactions = {InterestTransaction{
            .mMetadata = makeMetadata(1, 4, makeDate(2024, 1, 6)),
        }},
    });

    const auto result = DeterministicStatementMerger{}.merge(std::span{&input, 1U});

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);
    EXPECT_EQ(result.mStatement.mPresentation.mTradeInstruments.front().mTransactions.size(), 1U);
    EXPECT_TRUE(
        result.mStatement.mPresentation.mTradeInstruments.front().mCorporateActions.empty());
    EXPECT_TRUE(result.mStatement.mPresentation.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mPresentation.mInterestInstruments.empty());
    const auto diagnostics = diagnosticsOf<MergeDiagnostic>(result);
    ASSERT_EQ(diagnostics.size(), 5U);
    EXPECT_TRUE(std::all_of(diagnostics.begin(), diagnostics.end(), [](const auto* diagnostic) {
        return diagnostic->mCode == MergeDiagnosticCode::InconsistentSourceIndex;
    }));
    EXPECT_EQ(std::count_if(diagnostics.begin(),
                            diagnostics.end(),
                            [](const auto* diagnostic) { return diagnostic->mSources.empty(); }),
              1);
}

TEST(DeterministicStatementMergerTest, PreservesParserDiagnosticsBeforeMergeDiagnostics) {
    StatementMergeInput first{.mSourceIndex = 1};
    first.mParseResult.mBroker = Broker::InteractiveBrokers;
    first.mParseResult.mDiagnostics.push_back(ParseDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = DiagnosticCode::ParseError,
        .mSourceFile = "later.csv",
        .mMessage = "Synthetic later failure.",
    });
    StatementMergeInput second{.mSourceIndex = 0};
    second.mParseResult.mBroker = Broker::TradeRepublic;
    second.mParseResult.mDiagnostics.push_back(ParseDiagnostic{
        .mSeverity = DiagnosticSeverity::Warning,
        .mCode = DiagnosticCode::UnsupportedRowType,
        .mSourceFile = "earlier.csv",
        .mMessage = "Synthetic earlier warning.",
    });
    second.mParseResult.mStatement.mBenefitEvents.push_back(BenefitEvent{
        .mMetadata = EventMetadata{.mTaxDate = makeDate(2024, 1, 1)},
        .mName = "Missing source",
    });
    const std::array inputs{first, second};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mDiagnostics.size(), 3U);
    ASSERT_TRUE(std::holds_alternative<SourcedParseDiagnostic>(result.mDiagnostics[0]));
    ASSERT_TRUE(std::holds_alternative<SourcedParseDiagnostic>(result.mDiagnostics[1]));
    EXPECT_EQ(std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]).mSourceIndex, 0U);
    EXPECT_EQ(std::get<SourcedParseDiagnostic>(result.mDiagnostics[1]).mSourceIndex, 1U);
    EXPECT_TRUE(std::holds_alternative<MergeDiagnostic>(result.mDiagnostics[2]));
}

TEST(DeterministicStatementMergerTest, OmitsEmptyInstrumentContainers) {
    StatementMergeInput input{.mSourceIndex = 0};
    input.mParseResult.mStatement.mTradeInstruments.push_back(TradeInstrument{
        .mName = "Empty trade",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
    });
    input.mParseResult.mStatement.mDividendInstruments.push_back(DividendInstrument{
        .mName = "Empty dividend",
        .mIsin = "XX0000000002",
    });
    input.mParseResult.mStatement.mInterestInstruments.push_back(InterestInstrument{
        .mName = "Empty interest",
        .mInterestType = InterestType::BrokerInterest,
    });

    const auto result = DeterministicStatementMerger{}.merge(std::span{&input, 1U});

    EXPECT_TRUE(result.mStatement.mPresentation.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mPresentation.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mPresentation.mInterestInstruments.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(DeterministicStatementMergerTest, TimestampOrderingDoesNotUseCollectionArrivalOrder) {
    StatementMergeInput input{.mSourceIndex = 0};
    auto later = makeTrade(0, 0, makeDate(2024, 1, 1));
    later.mMetadata.mSourceTimestamp = makeTimestamp(2024, 1, 1, 12);
    auto earlier = makeTrade(0, 1, makeDate(2024, 1, 1));
    earlier.mMetadata.mSourceTimestamp = makeTimestamp(2024, 1, 1, 8);
    input.mParseResult.mStatement.mTradeInstruments.push_back(TradeInstrument{
        .mName = "Synthetic instrument",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions = {later, earlier},
    });

    const auto result = DeterministicStatementMerger{}.merge(std::span{&input, 1U});

    const auto& transactions =
        result.mStatement.mPresentation.mTradeInstruments.front().mTransactions;
    ASSERT_EQ(transactions.size(), 2U);
    EXPECT_EQ(primarySource(transactions[0].mMetadata).mInputSequence.mEventIndex, 1U);
    EXPECT_EQ(primarySource(transactions[1].mMetadata).mInputSequence.mEventIndex, 0U);
}

TEST(DeterministicStatementMergerTest, BuildsChronologyAcrossEveryCollectionAndInstrument) {
    StatementMergeInput first{.mSourceIndex = 0};
    auto futureTrade = makeTrade(0, 0, makeDate(2025, 1, 1));
    futureTrade.mMetadata.mSourceTimestamp = makeTimestamp(2023, 1, 1, 8);
    auto morningTrade = makeTrade(0, 1, makeDate(2024, 1, 2));
    morningTrade.mMetadata.mSourceTimestamp = makeTimestamp(2024, 1, 2, 8);
    first.mParseResult.mStatement.mTradeInstruments = {
        TradeInstrument{
            .mName = "Second ISIN",
            .mIsin = "XX0000000002",
            .mAssetClass = AssetClass::Stock,
            .mTransactions = {futureTrade, morningTrade},
        },
        TradeInstrument{
            .mName = "First ISIN",
            .mIsin = "XX0000000001",
            .mAssetClass = AssetClass::Stock,
            .mTransactions = {makeTrade(0, 2, makeDate(2023, 12, 31))},
            .mCorporateActions = {CorporateAction{
                .mMetadata = makeMetadata(0,
                                          3,
                                          makeDate(2024, 1, 2),
                                          Broker::TradeRepublic,
                                          "first.csv",
                                          makeTimestamp(2024, 1, 2, 12)),
                .mType = CorporateActionType::Split,
            }},
        },
    };

    StatementMergeInput second{.mSourceIndex = 1};
    second.mParseResult.mBroker = Broker::InteractiveBrokers;
    second.mParseResult.mStatement.mDividendInstruments = {DividendInstrument{
        .mName = "Dividend",
        .mIsin = "XX0000000003",
        .mTransactions = {DividendTransaction{
            .mMetadata = makeMetadata(1, 0, makeDate(2024, 1, 1), Broker::InteractiveBrokers),
        }},
    }};
    second.mParseResult.mStatement.mInterestInstruments = {InterestInstrument{
        .mName = "Account",
        .mInterestType = InterestType::BrokerInterest,
        .mTransactions = {InterestTransaction{
            .mMetadata = makeMetadata(1,
                                      1,
                                      makeDate(2024, 1, 2),
                                      Broker::InteractiveBrokers,
                                      "second.csv",
                                      makeTimestamp(2024, 1, 2, 7)),
        }},
    }};
    second.mParseResult.mStatement.mPrivateMarketEvents = {PrivateMarketEvent{
        .mMetadata = makeMetadata(1, 2, makeDate(2024, 1, 2), Broker::InteractiveBrokers),
        .mName = "Private fund",
    }};

    StatementMergeInput third{.mSourceIndex = 2};
    third.mParseResult.mStatement.mBenefitEvents = {BenefitEvent{
        .mMetadata = makeMetadata(2, 0, makeDate(2024, 1, 2)),
        .mName = "Reward",
    }};
    std::array inputs{first, second, third};
    const auto baseline = DeterministicStatementMerger{}.merge(inputs);
    const std::vector<StatementEventReference> expected{
        {StatementEventKind::Trade, 0, 0},
        {StatementEventKind::Dividend, 0, 0},
        {StatementEventKind::Interest, 0, 0},
        {StatementEventKind::Trade, 1, 0},
        {StatementEventKind::CorporateAction, 0, 0},
        {StatementEventKind::PrivateMarket, std::nullopt, 0},
        {StatementEventKind::Benefit, std::nullopt, 0},
        {StatementEventKind::Trade, 1, 1},
    };

    do
    {
        const auto result = DeterministicStatementMerger{}.merge(inputs);

        test::expectMergeResultsEqual(result, baseline);

        EXPECT_EQ(result.mStatement.mChronologicalOrder, expected);
        EXPECT_EQ(result.mStatement.mChronologicalOrder.size(),
                  mergedEventCount(result.mStatement.mPresentation));
        EXPECT_TRUE(result.mDiagnostics.empty());

        for (const auto& reference : result.mStatement.mChronologicalOrder)
        {
            EXPECT_NO_THROW((void)referencedMetadata(result.mStatement, reference));
        }

        EXPECT_EQ(result.mStatement.mPresentation.mTradeInstruments[0].mIsin, "XX0000000001");
        EXPECT_EQ(result.mStatement.mPresentation.mTradeInstruments[1].mTransactions[0].mMetadata,
                  morningTrade.mMetadata);
        EXPECT_EQ(result.mStatement.mPresentation.mTradeInstruments[1].mTransactions[1].mMetadata,
                  futureTrade.mMetadata);
    } while (std::next_permutation(inputs.begin(),
                                   inputs.end(),
                                   [](const auto& aLeft, const auto& aRight) {
                                       return aLeft.mSourceIndex < aRight.mSourceIndex;
                                   }));
}

TEST(DeterministicStatementMergerTest, EqualTimesUseOnlyStableSourceAndEventSequence) {
    const auto date = makeDate(2024, 1, 1);

    for (const auto timestamp :
         {std::optional<SourceTimestamp>{}, std::optional{makeTimestamp(2024, 1, 1, 8)}})
    {
        StatementMergeInput first{.mSourceIndex = 0};
        auto metadata =
            makeMetadata(0, 3, date, Broker::InteractiveBrokers, "z.csv", timestamp, "z-id");
        metadata.mSources.front().mSourceRow = 99;
        first.mParseResult.mStatement.mBenefitEvents = {BenefitEvent{
            .mMetadata = metadata,
            .mName = "First source benefit",
        }};
        first.mParseResult.mStatement.mPrivateMarketEvents = {PrivateMarketEvent{
            .mMetadata =
                makeMetadata(0, 1, date, Broker::InteractiveBrokers, "z.csv", timestamp, "y-id"),
            .mName = "First source private event",
        }};

        StatementMergeInput second{.mSourceIndex = 1};
        second.mParseResult.mStatement.mBenefitEvents = {BenefitEvent{
            .mMetadata =
                makeMetadata(1, 0, date, Broker::TradeRepublic, "a.csv", timestamp, "a-id"),
            .mName = "Second source benefit",
        }};
        const std::array inputs{second, first};

        const auto result = DeterministicStatementMerger{}.merge(inputs);
        const std::vector<StatementEventReference> expected{
            {StatementEventKind::PrivateMarket, std::nullopt, 0},
            {StatementEventKind::Benefit, std::nullopt, 0},
            {StatementEventKind::Benefit, std::nullopt, 1},
        };

        EXPECT_EQ(result.mStatement.mChronologicalOrder, expected);
        EXPECT_EQ(primarySource(referencedMetadata(result.mStatement, expected[1])).mSourceRow,
                  99U);
    }
}

TEST(DeterministicStatementMergerTest, RequestReorderingChangesOnlyChronologicalTies) {
    const auto date = makeDate(2024, 1, 1);
    StatementMergeInput first{.mSourceIndex = 0};
    first.mParseResult.mStatement.mBenefitEvents = {
        BenefitEvent{.mMetadata = makeMetadata(0, 0, date), .mName = "First tied event"},
        BenefitEvent{.mMetadata = makeMetadata(0, 1, makeDate(2023, 12, 31)),
                     .mName = "Older event"},
    };
    StatementMergeInput second{.mSourceIndex = 1};
    second.mParseResult.mStatement.mPrivateMarketEvents = {PrivateMarketEvent{
        .mMetadata = makeMetadata(1, 0, date),
        .mName = "Second tied event",
    }};
    std::array inputs{first, second};
    const auto original = DeterministicStatementMerger{}.merge(inputs);

    inputs[0].mSourceIndex = 1;
    inputs[1].mSourceIndex = 0;

    for (auto& event : inputs[0].mParseResult.mStatement.mBenefitEvents)
    {
        event.mMetadata.mSources.front().mInputSequence.mSourceIndex = 1;
    }

    inputs[1]
        .mParseResult.mStatement.mPrivateMarketEvents.front()
        .mMetadata.mSources.front()
        .mInputSequence.mSourceIndex = 0;
    const auto reordered = DeterministicStatementMerger{}.merge(inputs);
    const std::vector<StatementEventReference> originalOrder{
        {StatementEventKind::Benefit, std::nullopt, 0},
        {StatementEventKind::Benefit, std::nullopt, 1},
        {StatementEventKind::PrivateMarket, std::nullopt, 0},
    };
    const std::vector<StatementEventReference> reorderedOrder{
        originalOrder[0],
        originalOrder[2],
        originalOrder[1],
    };

    EXPECT_EQ(original.mStatement.mChronologicalOrder, originalOrder);
    EXPECT_EQ(reordered.mStatement.mChronologicalOrder, reorderedOrder);
}

TEST(DeterministicStatementMergerTest, MergerDoesNotPrioritizeSameDaySplitsOverTrades) {
    StatementMergeInput input{.mSourceIndex = 0};
    const auto date = makeDate(2024, 1, 1);
    auto trade = makeTrade(0, 0, date);
    trade.mMetadata.mSourceTimestamp = makeTimestamp(2024, 1, 1, 8);
    input.mParseResult.mStatement.mTradeInstruments = {TradeInstrument{
        .mName = "Security",
        .mIsin = "XX0000000001",
        .mAssetClass = AssetClass::Stock,
        .mTransactions = {trade},
        .mCorporateActions = {CorporateAction{
            .mMetadata = makeMetadata(0,
                                      1,
                                      date,
                                      Broker::TradeRepublic,
                                      "statement.csv",
                                      makeTimestamp(2024, 1, 1, 12)),
            .mType = CorporateActionType::Split,
            .mRatio = 2 * CORP_RATIO_SCALE,
        }},
    }};

    const auto result = DeterministicStatementMerger{}.merge(std::span{&input, 1U});
    const std::vector<StatementEventReference> expected{
        {StatementEventKind::Trade, 0, 0},
        {StatementEventKind::CorporateAction, 0, 0},
    };

    EXPECT_EQ(result.mStatement.mChronologicalOrder, expected);
    EXPECT_EQ(result.mStatement.mPresentation.mTradeInstruments[0].mTransactions[0].mUnits,
              trade.mUnits);
}

TEST(DeterministicStatementMergerTest, FailedFilesPreserveDiagnosticsAndUnaffectedLedger) {
    StatementMergeInput failed{.mSourceIndex = 0};
    failed.mParseResult.mDiagnostics = {ParseDiagnostic{
        .mCode = DiagnosticCode::ParseError,
        .mSourceFile = "failed.csv",
        .mMessage = "Synthetic file failure.",
    }};
    auto partial = makeDuplicateTestInput(1, "partial.csv");
    partial.mParseResult.mDiagnostics = {ParseDiagnostic{
        .mCode = DiagnosticCode::InvalidValue,
        .mSourceFile = "partial.csv",
        .mRowIndex = 20,
        .mMessage = "Synthetic rejected row.",
    }};
    std::array inputs{partial, failed};
    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 6U);
    ASSERT_EQ(result.mDiagnostics.size(), 2U);
    EXPECT_EQ(std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]).mSourceIndex, 0U);
    EXPECT_EQ(std::get<SourcedParseDiagnostic>(result.mDiagnostics[1]).mDiagnostic.mRowIndex, 20U);

    for (const auto& reference : result.mStatement.mChronologicalOrder)
    {
        EXPECT_EQ(primarySource(referencedMetadata(result.mStatement, reference))
                      .mInputSequence.mSourceIndex,
                  1U);
    }

    inputs[0].mParseResult.mStatement = {};
    const auto allFailed = DeterministicStatementMerger{}.merge(inputs);

    EXPECT_TRUE(allFailed.mStatement.mChronologicalOrder.empty());
    EXPECT_EQ(mergedEventCount(allFailed.mStatement.mPresentation), 0U);
    EXPECT_EQ(allFailed.mDiagnostics.size(), 2U);
}

TEST(DeterministicStatementMergerTest, DiagnosticsUseSourceOrderAndPreserveParserCreationOrder) {
    auto first = makeDuplicateTestInput(0, "first.csv");
    first.mParseResult.mDiagnostics = {
        ParseDiagnostic{.mSeverity = DiagnosticSeverity::Warning,
                        .mCode = DiagnosticCode::InvalidValue,
                        .mRowIndex = 12,
                        .mMessage = "Z first parser diagnostic."},
        ParseDiagnostic{.mCode = DiagnosticCode::InvalidValue,
                        .mRowIndex = 2,
                        .mMessage = "A second parser diagnostic."},
    };
    auto second = makeDuplicateTestInput(1, "second.csv");
    second.mParseResult.mStatement.mTradeInstruments[0].mTransactions[0].mUnitPrice += 1;
    second.mParseResult.mStatement.mPrivateMarketEvents[0].mAmount += 1;
    second.mParseResult.mDiagnostics = {ParseDiagnostic{
        .mCode = DiagnosticCode::ParseError,
        .mMessage = "Later source diagnostic.",
    }};
    std::array inputs{first, second};

    do
    {
        const auto result = DeterministicStatementMerger{}.merge(inputs);

        ASSERT_EQ(result.mDiagnostics.size(), 5U);
        EXPECT_EQ(std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]).mDiagnostic.mMessage,
                  "Z first parser diagnostic.");
        EXPECT_EQ(std::get<SourcedParseDiagnostic>(result.mDiagnostics[1]).mDiagnostic.mRowIndex,
                  2U);
        EXPECT_EQ(std::get<SourcedParseDiagnostic>(result.mDiagnostics[2]).mSourceIndex, 1U);

        const auto& firstConflict = std::get<MergeDiagnostic>(result.mDiagnostics[3]);
        const auto& secondConflict = std::get<MergeDiagnostic>(result.mDiagnostics[4]);

        EXPECT_EQ(firstConflict.mEventKinds, std::vector{StatementEventKind::Trade});
        EXPECT_EQ(secondConflict.mEventKinds, std::vector{StatementEventKind::PrivateMarket});
        ASSERT_EQ(firstConflict.mSources.size(), 2U);
        EXPECT_EQ(firstConflict.mSources[0].mInputSequence.mSourceIndex, 0U);
        EXPECT_EQ(firstConflict.mSources[1].mInputSequence.mSourceIndex, 1U);
        EXPECT_EQ(result.mStatement.mChronologicalOrder.size(), 4U);
    } while (std::next_permutation(inputs.begin(),
                                   inputs.end(),
                                   [](const auto& aLeft, const auto& aRight) {
                                       return aLeft.mSourceIndex < aRight.mSourceIndex;
                                   }));
}

TEST(DeterministicStatementMergerTest, InstrumentDiagnosticsUseEventSequenceThenSeverity) {
    StatementMergeInput first{.mSourceIndex = 0};
    first.mParseResult.mStatement.mTradeInstruments = {
        TradeInstrument{
            .mName = "First name",
            .mIsin = "XX0000000001",
            .mAssetClass = AssetClass::Stock,
            .mTransactions = {makeTrade(0, 2, makeDate(2024, 1, 1))},
        },
        TradeInstrument{
            .mName = "Second name",
            .mIsin = "XX0000000002",
            .mAssetClass = AssetClass::Stock,
            .mTransactions = {makeTrade(0, 0, makeDate(2025, 1, 1))},
        },
    };
    StatementMergeInput second{.mSourceIndex = 1};
    second.mParseResult.mStatement.mTradeInstruments = {
        TradeInstrument{
            .mName = "Other first name",
            .mIsin = "XX0000000001",
            .mAssetClass = AssetClass::Fund,
            .mTransactions = {makeTrade(1, 0, makeDate(2024, 1, 1))},
        },
        TradeInstrument{
            .mName = "Other second name",
            .mIsin = "XX0000000002",
            .mAssetClass = AssetClass::Stock,
            .mTransactions = {makeTrade(1, 1, makeDate(2025, 1, 1))},
        },
    };
    const std::array inputs{second, first};
    const auto result = DeterministicStatementMerger{}.merge(inputs);
    const auto diagnostics = diagnosticsOf<MergeDiagnostic>(result);

    ASSERT_EQ(diagnostics.size(), 3U);
    EXPECT_EQ(diagnostics[0]->mIsin, "XX0000000002");
    EXPECT_EQ(diagnostics[0]->mCode, MergeDiagnosticCode::InstrumentNameConflict);
    EXPECT_EQ(diagnostics[1]->mIsin, "XX0000000001");
    EXPECT_EQ(diagnostics[1]->mCode, MergeDiagnosticCode::InstrumentAssetClassConflict);
    EXPECT_EQ(diagnostics[2]->mCode, MergeDiagnosticCode::InstrumentNameConflict);
}

TEST(DeterministicStatementMergerTest, MissingSourceDiagnosticsUseTaxDateThenCreationOrder) {
    StatementMergeInput input{.mSourceIndex = 0};
    input.mParseResult.mStatement.mBenefitEvents = {
        BenefitEvent{.mMetadata = {.mTaxDate = makeDate(2025, 1, 1)}, .mName = "Future event"},
        BenefitEvent{.mMetadata = {.mTaxDate = makeDate(2024, 1, 1)}, .mName = "Zulu event"},
        BenefitEvent{.mMetadata = {.mTaxDate = makeDate(2024, 1, 1)}, .mName = "Alpha event"},
    };
    const auto result = DeterministicStatementMerger{}.merge(std::span{&input, 1U});
    const auto diagnostics = diagnosticsOf<MergeDiagnostic>(result);

    ASSERT_EQ(diagnostics.size(), 3U);
    EXPECT_EQ(diagnostics[0]->mInstrumentName, "Zulu event");
    EXPECT_EQ(diagnostics[1]->mInstrumentName, "Alpha event");
    EXPECT_EQ(diagnostics[2]->mInstrumentName, "Future event");
    EXPECT_TRUE(result.mStatement.mChronologicalOrder.empty());
}

// For the six-event fixture: trade, split, dividend, interest, benefit, private market.
// This is collection order, not chronological order; pointers let tests change input metadata.
std::vector<EventMetadata*> eventMetadataInTestOrder(BrokerStatement& aStatement) {
    std::vector<EventMetadata*> metadata;
    const auto appendMetadata = [&metadata](auto& aEvents) {
        std::transform(aEvents.begin(),
                       aEvents.end(),
                       std::back_inserter(metadata),
                       [](auto& aEvent) { return &aEvent.mMetadata; });
    };

    for (auto& instrument : aStatement.mTradeInstruments)
    {
        appendMetadata(instrument.mTransactions);
        appendMetadata(instrument.mCorporateActions);
    }

    for (auto& instrument : aStatement.mDividendInstruments)
    {
        appendMetadata(instrument.mTransactions);
    }

    for (auto& instrument : aStatement.mInterestInstruments)
    {
        appendMetadata(instrument.mTransactions);
    }

    appendMetadata(aStatement.mBenefitEvents);
    appendMetadata(aStatement.mPrivateMarketEvents);

    return metadata;
}

TEST(DeterministicStatementMergerTest, EveryDuplicateKindRetainsExactSourcesAcrossThreeFiles) {
    std::array inputs{makeDuplicateTestInput(0, "same.csv"),
                      makeDuplicateTestInput(1, "same.csv"),
                      makeDuplicateTestInput(2, "same.csv")};
    const auto expected = DeterministicStatementMerger{}.merge(inputs);
    const std::array kinds{StatementEventKind::Trade,
                           StatementEventKind::CorporateAction,
                           StatementEventKind::Dividend,
                           StatementEventKind::Interest,
                           StatementEventKind::Benefit,
                           StatementEventKind::PrivateMarket};
    const std::array ids{"trade-id",
                         "action-id",
                         "dividend-id",
                         "interest-id",
                         "benefit-id",
                         "private-id"};

    do
    {
        const auto result = DeterministicStatementMerger{}.merge(inputs);

        test::expectMergeResultsEqual(result, expected);
        ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), kinds.size());
        EXPECT_TRUE(result.mDiagnostics.empty());

        for (std::size_t event = 0; event < kinds.size(); ++event)
        {
            SCOPED_TRACE(event);
            const auto& reference = result.mStatement.mChronologicalOrder[event];
            const std::vector sources{
                makeSource(Broker::TradeRepublic, "same.csv", event + 2, ids[event], {0, event}),
                makeSource(Broker::TradeRepublic, "same.csv", event + 2, ids[event], {1, event}),
                makeSource(Broker::TradeRepublic, "same.csv", event + 2, ids[event], {2, event}),
            };

            EXPECT_EQ(reference.mKind, kinds[event]);
            EXPECT_EQ(referencedMetadata(result.mStatement, reference).mSources, sources);
        }
    } while (std::next_permutation(inputs.begin(),
                                   inputs.end(),
                                   [](const auto& aLeft, const auto& aRight) {
                                       return aLeft.mSourceIndex < aRight.mSourceIndex;
                                   }));
}

TEST(DeterministicStatementMergerTest, MissingAndEmptyIdsKeepEveryKindIndependentAcrossFiles) {
    std::array inputs{makeDuplicateTestInput(0, "first.csv"),
                      makeDuplicateTestInput(1, "second.csv")};

    for (auto& input : inputs)
    {
        for (auto* metadata : eventMetadataInTestOrder(input.mParseResult.mStatement))
        {
            auto& id = metadata->mSources.front().mTransactionId;

            if (input.mSourceIndex == 0)
            {
                id.reset();
            }
            else
            {
                id = "";
            }
        }
    }

    const auto expected = DeterministicStatementMerger{}.merge(inputs);

    do
    {
        const auto result = DeterministicStatementMerger{}.merge(inputs);

        test::expectMergeResultsEqual(result, expected);
        ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 12U);
        EXPECT_TRUE(result.mDiagnostics.empty());

        for (std::size_t event = 0; event < 6; ++event)
        {
            for (std::size_t source = 0; source < 2; ++source)
            {
                const auto& metadata =
                    referencedMetadata(result.mStatement,
                                       result.mStatement.mChronologicalOrder[event * 2 + source]);

                ASSERT_EQ(metadata.mSources.size(), 1U);
                EXPECT_EQ(metadata.mSources.front().mInputSequence,
                          (StableInputSequence{source, event}));
                EXPECT_FALSE(transactionIdentity(metadata).has_value());
                EXPECT_EQ(metadata.mSources.front().mTransactionId.has_value(), source == 1);
            }
        }
    } while (std::next_permutation(inputs.begin(),
                                   inputs.end(),
                                   [](const auto& aLeft, const auto& aRight) {
                                       return aLeft.mSourceIndex < aRight.mSourceIndex;
                                   }));
}

TEST(DeterministicStatementMergerTest, EveryKindRejectsChangedTaxDateOrTimestampFacts) {
    const std::array kinds{StatementEventKind::Trade,
                           StatementEventKind::CorporateAction,
                           StatementEventKind::Dividend,
                           StatementEventKind::Interest,
                           StatementEventKind::Benefit,
                           StatementEventKind::PrivateMarket};

    for (std::size_t kind = 0; kind < kinds.size(); ++kind)
    {
        for (const auto change : {"tax date", "timestamp value", "timestamp presence"})
        {
            SCOPED_TRACE(kind);
            SCOPED_TRACE(change);
            std::array inputs{makeDuplicateTestInput(0, "first.csv"),
                              makeDuplicateTestInput(1, "second.csv"),
                              makeDuplicateTestInput(2, "third.csv")};

            for (auto& input : inputs)
            {
                auto* metadata = eventMetadataInTestOrder(input.mParseResult.mStatement)[kind];
                metadata->mSourceTimestamp =
                    makeTimestamp(2024, 1, static_cast<unsigned>(kind + 1), 9);
            }

            auto* changed = eventMetadataInTestOrder(inputs[2].mParseResult.mStatement)[kind];

            if (std::string_view{change} == "tax date")
            {
                changed->mTaxDate += DayDuration{1};
            }
            else if (std::string_view{change} == "timestamp value")
            {
                *changed->mSourceTimestamp += std::chrono::milliseconds{1};
            }
            else
            {
                changed->mSourceTimestamp.reset();
            }

            const auto expected = DeterministicStatementMerger{}.merge(inputs);

            do
            {
                const auto result = DeterministicStatementMerger{}.merge(inputs);

                test::expectMergeResultsEqual(result, expected);
                ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 5U);
                ASSERT_EQ(result.mDiagnostics.size(), 1U);
                const auto& diagnostic = std::get<MergeDiagnostic>(result.mDiagnostics.front());
                const auto originalMetadata =
                    eventMetadataInTestOrder(inputs[0].mParseResult.mStatement);
                const auto id = originalMetadata[kind]->mSources.front().mTransactionId;
                const std::vector sources{
                    makeSource(Broker::TradeRepublic, "first.csv", kind + 2, *id, {0, kind}),
                    makeSource(Broker::TradeRepublic, "second.csv", kind + 2, *id, {1, kind}),
                    makeSource(Broker::TradeRepublic, "third.csv", kind + 2, *id, {2, kind}),
                };

                EXPECT_EQ(diagnostic.mCode, MergeDiagnosticCode::ConflictingDuplicate);
                EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
                EXPECT_EQ(diagnostic.mEventKinds, std::vector{kinds[kind]});
                EXPECT_EQ(diagnostic.mSources, sources);
                EXPECT_EQ(diagnostic.mTaxDate.has_value(), std::string_view{change} != "tax date");

                for (const auto& reference : result.mStatement.mChronologicalOrder)
                {
                    EXPECT_NE(reference.mKind, kinds[kind]);
                }
            } while (std::next_permutation(inputs.begin(),
                                           inputs.end(),
                                           [](const auto& aLeft, const auto& aRight) {
                                               return aLeft.mSourceIndex < aRight.mSourceIndex;
                                           }));
        }
    }
}

TEST(DeterministicStatementMergerTest, EveryKindRejectsDifferentOrderingTimestamp) {
    const std::array eventKinds{StatementEventKind::Trade,
                                StatementEventKind::CorporateAction,
                                StatementEventKind::Dividend,
                                StatementEventKind::Interest,
                                StatementEventKind::Benefit,
                                StatementEventKind::PrivateMarket};

    for (std::size_t eventIndex = 0; eventIndex < eventKinds.size(); ++eventIndex)
    {
        SCOPED_TRACE(eventIndex);
        std::array inputs{makeDuplicateTestInput(0, "first.csv"),
                          makeDuplicateTestInput(1, "second.csv"),
                          makeDuplicateTestInput(2, "third.csv")};

        for (auto& input : inputs)
        {
            auto* metadata = eventMetadataInTestOrder(input.mParseResult.mStatement)[eventIndex];
            metadata->mSourceTimestamp =
                makeTimestamp(2024, 1, static_cast<unsigned>(eventIndex + 1), 9);
            metadata->mOrderingTimestamp = metadata->mSourceTimestamp;
        }

        auto* changed = eventMetadataInTestOrder(inputs[2].mParseResult.mStatement)[eventIndex];
        *changed->mOrderingTimestamp += std::chrono::milliseconds{1};
        const auto expected = DeterministicStatementMerger{}.merge(inputs);

        do
        {
            const auto result = DeterministicStatementMerger{}.merge(inputs);

            test::expectMergeResultsEqual(result, expected);
            ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 5U);
            ASSERT_EQ(result.mDiagnostics.size(), 1U);
            const auto& conflict = std::get<MergeDiagnostic>(result.mDiagnostics.front());
            const auto originalMetadata =
                eventMetadataInTestOrder(inputs[0].mParseResult.mStatement);
            const auto transactionId =
                originalMetadata[eventIndex]->mSources.front().mTransactionId;
            const std::vector expectedSources{
                makeSource(Broker::TradeRepublic,
                           "first.csv",
                           eventIndex + 2,
                           *transactionId,
                           {0, eventIndex}),
                makeSource(Broker::TradeRepublic,
                           "second.csv",
                           eventIndex + 2,
                           *transactionId,
                           {1, eventIndex}),
                makeSource(Broker::TradeRepublic,
                           "third.csv",
                           eventIndex + 2,
                           *transactionId,
                           {2, eventIndex}),
            };

            EXPECT_EQ(conflict.mCode, MergeDiagnosticCode::ConflictingDuplicate);
            EXPECT_EQ(conflict.mSeverity, DiagnosticSeverity::Error);
            EXPECT_EQ(conflict.mEventKinds, std::vector{eventKinds[eventIndex]});
            EXPECT_EQ(conflict.mSources, expectedSources);
            EXPECT_TRUE(conflict.mTaxDate.has_value());

            for (const auto& event : result.mStatement.mChronologicalOrder)
            {
                EXPECT_NE(event.mKind, eventKinds[eventIndex]);
            }
        } while (std::next_permutation(inputs.begin(),
                                       inputs.end(),
                                       [](const auto& left, const auto& right) {
                                           return left.mSourceIndex < right.mSourceIndex;
                                       }));
    }
}

TEST(DeterministicStatementMergerTest, EveryKindRejectsMissingOrderingTimestamp) {
    const std::array eventKinds{StatementEventKind::Trade,
                                StatementEventKind::CorporateAction,
                                StatementEventKind::Dividend,
                                StatementEventKind::Interest,
                                StatementEventKind::Benefit,
                                StatementEventKind::PrivateMarket};

    for (std::size_t eventIndex = 0; eventIndex < eventKinds.size(); ++eventIndex)
    {
        SCOPED_TRACE(eventIndex);
        std::array inputs{makeDuplicateTestInput(0, "first.csv"),
                          makeDuplicateTestInput(1, "second.csv"),
                          makeDuplicateTestInput(2, "third.csv")};

        for (auto& input : inputs)
        {
            auto* metadata = eventMetadataInTestOrder(input.mParseResult.mStatement)[eventIndex];
            metadata->mSourceTimestamp =
                makeTimestamp(2024, 1, static_cast<unsigned>(eventIndex + 1), 9);
            metadata->mOrderingTimestamp = metadata->mSourceTimestamp;
        }

        auto* changed = eventMetadataInTestOrder(inputs[2].mParseResult.mStatement)[eventIndex];
        changed->mOrderingTimestamp.reset();
        const auto expected = DeterministicStatementMerger{}.merge(inputs);

        do
        {
            const auto result = DeterministicStatementMerger{}.merge(inputs);

            test::expectMergeResultsEqual(result, expected);
            ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 5U);
            ASSERT_EQ(result.mDiagnostics.size(), 1U);
            const auto& conflict = std::get<MergeDiagnostic>(result.mDiagnostics.front());
            const auto originalMetadata =
                eventMetadataInTestOrder(inputs[0].mParseResult.mStatement);
            const auto transactionId =
                originalMetadata[eventIndex]->mSources.front().mTransactionId;
            const std::vector expectedSources{
                makeSource(Broker::TradeRepublic,
                           "first.csv",
                           eventIndex + 2,
                           *transactionId,
                           {0, eventIndex}),
                makeSource(Broker::TradeRepublic,
                           "second.csv",
                           eventIndex + 2,
                           *transactionId,
                           {1, eventIndex}),
                makeSource(Broker::TradeRepublic,
                           "third.csv",
                           eventIndex + 2,
                           *transactionId,
                           {2, eventIndex}),
            };

            EXPECT_EQ(conflict.mCode, MergeDiagnosticCode::ConflictingDuplicate);
            EXPECT_EQ(conflict.mSeverity, DiagnosticSeverity::Error);
            EXPECT_EQ(conflict.mEventKinds, std::vector{eventKinds[eventIndex]});
            EXPECT_EQ(conflict.mSources, expectedSources);
            EXPECT_TRUE(conflict.mTaxDate.has_value());

            for (const auto& event : result.mStatement.mChronologicalOrder)
            {
                EXPECT_NE(event.mKind, eventKinds[eventIndex]);
            }
        } while (std::next_permutation(inputs.begin(),
                                       inputs.end(),
                                       [](const auto& left, const auto& right) {
                                           return left.mSourceIndex < right.mSourceIndex;
                                       }));
    }
}

TEST(DeterministicStatementMergerTest, CollectionReorderingPreservesCompleteResultsAndInputs) {
    std::array inputs{makeDuplicateTestInput(0, "first.csv"),
                      makeDuplicateTestInput(1, "second.csv"),
                      makeDuplicateTestInput(2, "third.csv")};
    inputs[1].mParseResult.mStatement.mTradeInstruments.front().mName = "Alternative name";
    inputs[2].mParseResult.mStatement.mPrivateMarketEvents.front().mFeePaid += 1;

    for (auto& input : inputs)
    {
        auto& statement = input.mParseResult.mStatement;
        auto other = statement.mTradeInstruments.front();
        other.mIsin = "XX0000000099";
        other.mName = "Other instrument";
        other.mAssetClass = input.mSourceIndex == 1 ? AssetClass::Fund : AssetClass::Stock;

        for (auto* metadata : eventMetadataInTestOrder(input.mParseResult.mStatement))
        {
            metadata->mSourceTimestamp.reset();
        }

        for (auto& event : other.mTransactions)
        {
            event.mMetadata.mSources.front().mInputSequence.mEventIndex += 10;
            event.mMetadata.mSources.front().mSourceRow += 10;
            event.mMetadata.mSources.front().mTransactionId =
                "other-trade-" + std::to_string(input.mSourceIndex);
        }

        other.mCorporateActions.clear();
        statement.mTradeInstruments.push_back(other);
        auto extraTrade = statement.mTradeInstruments.front().mTransactions.front();
        extraTrade.mMetadata.mSources.front().mInputSequence.mEventIndex += 20;
        extraTrade.mMetadata.mSources.front().mSourceRow += 20;
        extraTrade.mMetadata.mSources.front().mTransactionId.reset();
        statement.mTradeInstruments.front().mTransactions.push_back(extraTrade);
        const auto appendExtraEvent = [](auto& aEvents) {
            auto extra = aEvents.front();
            auto& source = extra.mMetadata.mSources.front();
            source.mInputSequence.mEventIndex += 30;
            source.mSourceRow += 30;
            source.mTransactionId.reset();
            aEvents.push_back(extra);
        };

        appendExtraEvent(statement.mTradeInstruments.front().mCorporateActions);
        appendExtraEvent(statement.mDividendInstruments.front().mTransactions);
        appendExtraEvent(statement.mInterestInstruments.front().mTransactions);
        appendExtraEvent(statement.mBenefitEvents);
        appendExtraEvent(statement.mPrivateMarketEvents);
        auto otherDividend = statement.mDividendInstruments.front();
        otherDividend.mIsin = "XX0000000099";
        auto otherInterest = statement.mInterestInstruments.front();
        otherInterest.mName = "Other account";

        for (auto& event : otherDividend.mTransactions)
        {
            auto& source = event.mMetadata.mSources.front();
            source.mInputSequence.mEventIndex += 40;
            source.mSourceRow += 40;
            source.mTransactionId.reset();
        }

        for (auto& event : otherInterest.mTransactions)
        {
            auto& source = event.mMetadata.mSources.front();
            source.mInputSequence.mEventIndex += 40;
            source.mSourceRow += 40;
            source.mTransactionId.reset();
        }

        statement.mDividendInstruments.push_back(otherDividend);
        statement.mInterestInstruments.push_back(otherInterest);
        input.mParseResult.mDiagnostics = {ParseDiagnostic{.mSeverity = DiagnosticSeverity::Warning,
                                                           .mCode = DiagnosticCode::InvalidValue,
                                                           .mSourceFile = "synthetic.csv",
                                                           .mRowIndex = 8,
                                                           .mTransactionId = "synthetic-warning",
                                                           .mField = "datetime",
                                                           .mMessage = "Synthetic warning."}};
    }

    const auto expected = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(expected.mStatement.mChronologicalOrder.size(), 38U);
    ASSERT_EQ(expected.mDiagnostics.size(), 6U);
    ASSERT_NE(findMergeDiagnostic(expected, MergeDiagnosticCode::InstrumentAssetClassConflict),
              nullptr);
    ASSERT_NE(findMergeDiagnostic(expected, MergeDiagnosticCode::InstrumentNameConflict), nullptr);
    ASSERT_NE(findMergeDiagnostic(expected, MergeDiagnosticCode::ConflictingDuplicate), nullptr);

    for (auto& input : inputs)
    {
        auto& statement = input.mParseResult.mStatement;

        for (auto& instrument : statement.mTradeInstruments)
        {
            std::reverse(instrument.mTransactions.begin(), instrument.mTransactions.end());
            std::reverse(instrument.mCorporateActions.begin(), instrument.mCorporateActions.end());
        }

        std::reverse(statement.mTradeInstruments.begin(), statement.mTradeInstruments.end());

        for (auto& instrument : statement.mDividendInstruments)
        {
            std::reverse(instrument.mTransactions.begin(), instrument.mTransactions.end());
        }

        for (auto& instrument : statement.mInterestInstruments)
        {
            std::reverse(instrument.mTransactions.begin(), instrument.mTransactions.end());
        }

        std::reverse(statement.mDividendInstruments.begin(), statement.mDividendInstruments.end());
        std::reverse(statement.mInterestInstruments.begin(), statement.mInterestInstruments.end());
        std::reverse(statement.mBenefitEvents.begin(), statement.mBenefitEvents.end());
        std::reverse(statement.mPrivateMarketEvents.begin(), statement.mPrivateMarketEvents.end());
    }

    const auto originalInputs = inputs;

    do
    {
        const auto result = DeterministicStatementMerger{}.merge(inputs);

        test::expectMergeResultsEqual(result, expected);

        for (const auto& input : inputs)
        {
            const auto& original = originalInputs[input.mSourceIndex];

            test::expectStatementsEqual(input.mParseResult.mStatement,
                                        original.mParseResult.mStatement);
        }
    } while (std::next_permutation(inputs.begin(),
                                   inputs.end(),
                                   [](const auto& aLeft, const auto& aRight) {
                                       return aLeft.mSourceIndex < aRight.mSourceIndex;
                                   }));
}

TEST(DeterministicStatementMergerTest,
     CorporateActionsKeepEconomicCandidatesAcrossFilesAndBrokers) {
    std::array inputs{makeDuplicateTestInput(0, "first.csv"),
                      makeDuplicateTestInput(1, "overlap.csv"),
                      makeDuplicateTestInput(2, "other-broker.csv")};
    auto& other = inputs[2].mParseResult;
    other.mBroker = Broker::InteractiveBrokers;

    for (auto* metadata : eventMetadataInTestOrder(other.mStatement))
    {
        metadata->mSources.front().mBroker = Broker::InteractiveBrokers;
    }

    auto& instrument = inputs[1].mParseResult.mStatement.mTradeInstruments.front();
    auto distinctAction = instrument.mCorporateActions.front();
    distinctAction.mMetadata.mSources.front().mInputSequence.mEventIndex = 6;
    distinctAction.mMetadata.mSources.front().mSourceRow = 8;
    distinctAction.mMetadata.mSources.front().mTransactionId = "different-action-id";
    instrument.mCorporateActions.push_back(distinctAction);
    const auto result = DeterministicStatementMerger{}.merge(inputs);
    const auto& actions =
        result.mStatement.mPresentation.mTradeInstruments.front().mCorporateActions;

    ASSERT_EQ(actions.size(), 3U);
    EXPECT_EQ(
        actions[0].mMetadata.mSources,
        (std::vector{makeSource(Broker::TradeRepublic, "first.csv", 3, "action-id", {0, 1}),
                     makeSource(Broker::TradeRepublic, "overlap.csv", 3, "action-id", {1, 1})}));
    EXPECT_EQ(actions[1].mMetadata.mSources,
              std::vector{distinctAction.mMetadata.mSources.front()});
    EXPECT_EQ(
        actions[2].mMetadata.mSources,
        other.mStatement.mTradeInstruments.front().mCorporateActions.front().mMetadata.mSources);
    EXPECT_EQ(test::eventFacts(actions[0]), test::eventFacts(actions[1]));
    EXPECT_EQ(test::eventFacts(actions[1]), test::eventFacts(actions[2]));
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(DeterministicStatementMergerTest, PreservesExplicitTwoForOneSplitEvidence) {
    // The type and ratio are explicit synthetic evidence, not derived from TR shares.
    StatementMergeInput input{.mSourceIndex = 0};
    input.mParseResult.mStatement.mTradeInstruments = {TradeInstrument{
        .mName = "Synthetic Share",
        .mIsin = "XX9000000001",
        .mAssetClass = AssetClass::Stock,
        .mCorporateActions = {CorporateAction{
                                  .mMetadata = makeMetadata(0,
                                                            0,
                                                            makeDate(2024, 1, 15),
                                                            Broker::InteractiveBrokers,
                                                            "verified.csv",
                                                            std::nullopt,
                                                            "verified-action"),
                                  .mType = CorporateActionType::Split,
                                  .mUnitsDelta = 100'000'000,
                                  .mRatio = 200'000'000,
                              },
                              CorporateAction{
                                  .mMetadata = makeMetadata(0,
                                                            1,
                                                            makeDate(2024, 1, 15),
                                                            Broker::TradeRepublic,
                                                            "unresolved.csv",
                                                            std::nullopt,
                                                            "unresolved-action"),
                                  .mUnitsDelta = -50'000'000,
                              }},
    }};

    const auto result = DeterministicStatementMerger{}.merge(std::span{&input, 1U});

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 2U);
    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments.front();

    EXPECT_TRUE(instrument.mTransactions.empty());
    ASSERT_EQ(instrument.mCorporateActions.size(), 2U);

    const auto& resolved = instrument.mCorporateActions[0];

    EXPECT_EQ(resolved.mType, CorporateActionType::Split);
    EXPECT_EQ(resolved.mUnitsDelta, 100'000'000);
    ASSERT_TRUE(resolved.mRatio.has_value());
    EXPECT_EQ(*resolved.mRatio, 200'000'000);
    EXPECT_EQ(
        resolved.mMetadata.mSources,
        (std::vector{
            makeSource(Broker::InteractiveBrokers, "verified.csv", 2, "verified-action", {0, 0})}));

    const auto& unresolved = instrument.mCorporateActions[1];

    EXPECT_EQ(unresolved.mType, CorporateActionType::UnresolvedSplit);
    EXPECT_EQ(unresolved.mUnitsDelta, -50'000'000);
    EXPECT_FALSE(unresolved.mRatio.has_value());
    EXPECT_EQ(
        unresolved.mMetadata.mSources,
        (std::vector{
            makeSource(Broker::TradeRepublic, "unresolved.csv", 3, "unresolved-action", {0, 1})}));
}

TEST(DeterministicStatementMergerTest, PreservesExplicitOneForTenReverseSplitEvidence) {
    // The type and ratio are explicit synthetic evidence, not derived from TR shares.
    StatementMergeInput input{.mSourceIndex = 0};
    input.mParseResult.mStatement.mTradeInstruments = {TradeInstrument{
        .mName = "Synthetic Share",
        .mIsin = "XX9000000001",
        .mAssetClass = AssetClass::Stock,
        .mCorporateActions = {CorporateAction{
                                  .mMetadata = makeMetadata(0,
                                                            0,
                                                            makeDate(2024, 1, 15),
                                                            Broker::InteractiveBrokers,
                                                            "verified.csv",
                                                            std::nullopt,
                                                            "verified-action"),
                                  .mType = CorporateActionType::ReverseSplit,
                                  .mUnitsDelta = -900'000'000,
                                  .mRatio = 10'000'000,
                              },
                              CorporateAction{
                                  .mMetadata = makeMetadata(0,
                                                            1,
                                                            makeDate(2024, 1, 15),
                                                            Broker::TradeRepublic,
                                                            "unresolved.csv",
                                                            std::nullopt,
                                                            "unresolved-action"),
                                  .mUnitsDelta = -50'000'000,
                              }},
    }};

    const auto result = DeterministicStatementMerger{}.merge(std::span{&input, 1U});

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 2U);
    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments.front();

    EXPECT_TRUE(instrument.mTransactions.empty());
    ASSERT_EQ(instrument.mCorporateActions.size(), 2U);

    const auto& resolved = instrument.mCorporateActions[0];

    EXPECT_EQ(resolved.mType, CorporateActionType::ReverseSplit);
    EXPECT_EQ(resolved.mUnitsDelta, -900'000'000);
    ASSERT_TRUE(resolved.mRatio.has_value());
    EXPECT_EQ(*resolved.mRatio, 10'000'000);
    EXPECT_EQ(
        resolved.mMetadata.mSources,
        (std::vector{
            makeSource(Broker::InteractiveBrokers, "verified.csv", 2, "verified-action", {0, 0})}));

    const auto& unresolved = instrument.mCorporateActions[1];

    EXPECT_EQ(unresolved.mType, CorporateActionType::UnresolvedSplit);
    EXPECT_EQ(unresolved.mUnitsDelta, -50'000'000);
    EXPECT_FALSE(unresolved.mRatio.has_value());
    EXPECT_EQ(
        unresolved.mMetadata.mSources,
        (std::vector{
            makeSource(Broker::TradeRepublic, "unresolved.csv", 3, "unresolved-action", {0, 1})}));
}

TEST(DeterministicStatementMergerTest, RejectsSameIdentityWithResolvedAndUnresolvedSplitEvidence) {
    StatementMergeInput input{.mSourceIndex = 0};
    input.mParseResult.mStatement.mTradeInstruments = {TradeInstrument{
        .mName = "Synthetic Share",
        .mIsin = "XX9000000001",
        .mAssetClass = AssetClass::Stock,
        .mCorporateActions = {CorporateAction{
                                  .mMetadata = makeMetadata(0,
                                                            0,
                                                            makeDate(2024, 1, 15),
                                                            Broker::TradeRepublic,
                                                            "unresolved.csv",
                                                            std::nullopt,
                                                            "same-action"),
                                  .mUnitsDelta = 100'000'000,
                              },
                              CorporateAction{
                                  .mMetadata = makeMetadata(0,
                                                            1,
                                                            makeDate(2024, 1, 15),
                                                            Broker::TradeRepublic,
                                                            "resolved.csv",
                                                            std::nullopt,
                                                            "same-action"),
                                  .mType = CorporateActionType::Split,
                                  .mUnitsDelta = 100'000'000,
                                  .mRatio = 200'000'000,
                              }},
    }};

    const auto result = DeterministicStatementMerger{}.merge(std::span{&input, 1U});

    EXPECT_TRUE(result.mStatement.mChronologicalOrder.empty());
    EXPECT_TRUE(result.mStatement.mPresentation.mTradeInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<MergeDiagnostic>(result.mDiagnostics.front()));

    const auto& diagnostic = std::get<MergeDiagnostic>(result.mDiagnostics.front());

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, MergeDiagnosticCode::ConflictingDuplicate);
    EXPECT_EQ(
        diagnostic.mSources,
        (std::vector{makeSource(Broker::TradeRepublic, "unresolved.csv", 2, "same-action", {0, 0}),
                     makeSource(Broker::TradeRepublic, "resolved.csv", 3, "same-action", {0, 1})}));
}

TEST(DeterministicStatementMergerTest, UnresolvedSplitDuplicatesKeepEachQuantitySourceOnce) {
    const auto firstSource = makeSource(Broker::TradeRepublic, "same.csv", 2, "split-id", {0, 0});
    const auto secondSource = makeSource(Broker::TradeRepublic, "same.csv", 3, "split-id", {0, 1});
    const auto sharedSource = makeSource(Broker::TradeRepublic, "same.csv", 4, "split-id", {0, 2});
    const UnitSourceEvidence sharedEvidence{
        .mSource = sharedSource,
        .mSourceText = "-0.12345678500",
        .mDiscardedDigits = "500",
        .mCanonicalValue = "-0.123456785",
        .mRoundedAwayFromZero = true,
    };
    const CorporateAction first{
        .mMetadata = {.mTaxDate = makeDate(2024, 1, 15), .mSources = {firstSource, sharedSource}},
        .mType = CorporateActionType::UnresolvedSplit,
        // -0.123456785 rounds to -0.12345679 shares, stored at a scale of 100,000,000.
        .mUnitsDelta = -12'345'679,
        .mUnitEvidence = {{.mSource = firstSource,
                           .mSourceText = "-0.123456785",
                           .mDiscardedDigits = "5",
                           .mCanonicalValue = "-0.123456785",
                           .mRoundedAwayFromZero = true},
                          sharedEvidence},
    };
    const CorporateAction second{
        .mMetadata = {.mTaxDate = makeDate(2024, 1, 15), .mSources = {secondSource, sharedSource}},
        .mType = CorporateActionType::UnresolvedSplit,
        .mUnitsDelta = -12'345'679,
        .mUnitEvidence = {sharedEvidence,
                          {.mSource = secondSource,
                           .mSourceText = "-0.1234567850",
                           .mDiscardedDigits = "50",
                           .mCanonicalValue = "-0.123456785",
                           .mRoundedAwayFromZero = true}},
    };
    StatementMergeInput input{.mSourceIndex = 0};
    input.mParseResult.mStatement.mTradeInstruments.push_back(
        {.mName = "Synthetic Share",
         .mIsin = "XX9000000001",
         .mAssetClass = AssetClass::Stock,
         .mCorporateActions = {second, first}});

    const auto result = DeterministicStatementMerger{}.merge(std::span{&input, 1U});

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 1U);
    EXPECT_EQ(result.mStatement.mChronologicalOrder.front().mKind,
              StatementEventKind::CorporateAction);
    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments.front();

    EXPECT_TRUE(instrument.mTransactions.empty());
    ASSERT_EQ(instrument.mCorporateActions.size(), 1U);

    const auto& action = instrument.mCorporateActions.front();

    // A negative broker quantity still leaves the split direction unresolved.
    EXPECT_EQ(action.mType, CorporateActionType::UnresolvedSplit);
    // Keep the supplied -0.12345679 shares: -0.12345679 * 100,000,000 = -12,345,679.
    EXPECT_EQ(action.mUnitsDelta, -12'345'679);
    EXPECT_FALSE(action.mRatio.has_value());
    EXPECT_EQ(action.mMetadata.mSources, (std::vector{firstSource, secondSource, sharedSource}));
    ASSERT_EQ(action.mUnitEvidence.size(), 3U);

    const auto& firstEvidence = action.mUnitEvidence[0];
    const auto& secondEvidence = action.mUnitEvidence[1];

    EXPECT_EQ(firstEvidence.mSource, firstSource);
    EXPECT_EQ(firstEvidence.mSourceText, "-0.123456785");
    EXPECT_EQ(firstEvidence.mDiscardedDigits, "5");
    EXPECT_EQ(firstEvidence.mCanonicalValue, "-0.123456785");
    EXPECT_TRUE(firstEvidence.mRoundedAwayFromZero);

    EXPECT_EQ(secondEvidence.mSource, secondSource);
    EXPECT_EQ(secondEvidence.mSourceText, "-0.1234567850");
    EXPECT_EQ(secondEvidence.mDiscardedDigits, "50");
    EXPECT_EQ(secondEvidence.mCanonicalValue, "-0.123456785");
    EXPECT_TRUE(secondEvidence.mRoundedAwayFromZero);
    EXPECT_EQ(action.mUnitEvidence[2], sharedEvidence);
}

} // namespace
