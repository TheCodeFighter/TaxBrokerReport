#include "taxbroker/statement_merger.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
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

TEST(DeterministicStatementMergerTest, KeepsDuplicateLookingTradesForLaterDeduplication) {
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
    EXPECT_EQ(result.mStatement.mPresentation.mTradeInstruments.front().mTransactions.size(), 2U);
    EXPECT_TRUE(diagnosticsOf<MergeDiagnostic>(result).empty());
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
