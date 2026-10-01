#include "taxbroker/statement_merger.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace {
using namespace taxbroker;

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
    EXPECT_TRUE(result.mStatement.mChronologicalOrder.empty());
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

        change.mApply(aSelectRecord(second.mParseResult.mStatement));
        const std::array inputs{second, first};

        const auto result = DeterministicStatementMerger{}.merge(inputs);

        // Only the conflicting identity is removed; the five unrelated identities survive.
        EXPECT_EQ(mergedEventCount(result.mStatement.mPresentation), 5U);

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
    const std::array<FieldChange<DividendInstrument>, 6> changes{{
        {"ISIN", [](DividendInstrument& aValue) { aValue.mIsin = "XX0000000099"; }},
        {"gross", [](DividendInstrument& aValue) { ++aValue.mTransactions.front().mGrossAmount; }},
        {"tax", [](DividendInstrument& aValue) { ++aValue.mTransactions.front().mTaxPaid; }},
        {"exchange rate",
         [](DividendInstrument& aValue) { ++aValue.mTransactions.front().mExchangeRate; }},
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
    const std::array<FieldChange<InterestInstrument>, 8> changes{{
        {"name without ISIN", [](InterestInstrument& aValue) { aValue.mName = "Other payer"; }},
        {"ISIN presence", [](InterestInstrument& aValue) { aValue.mIsin = "XX0000000099"; }},
        {"type",
         [](InterestInstrument& aValue) { aValue.mInterestType = InterestType::OtherInterest; }},
        {"gross", [](InterestInstrument& aValue) { ++aValue.mTransactions.front().mGrossAmount; }},
        {"tax", [](InterestInstrument& aValue) { ++aValue.mTransactions.front().mTaxPaid; }},
        {"exchange rate",
         [](InterestInstrument& aValue) { ++aValue.mTransactions.front().mExchangeRate; }},
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

} // namespace
