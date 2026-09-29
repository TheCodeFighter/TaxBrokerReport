#include "taxbroker/statement_merger.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <span>
#include <type_traits>

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

static_assert(std::is_abstract_v<StatementMerger>);
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

} // namespace
