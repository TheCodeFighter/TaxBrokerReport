#pragma once

#include "taxbroker/statement_merger.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <vector>

namespace taxbroker::test {

inline const EventMetadata& referencedMetadata(const MergedStatement& aStatement,
                                               const StatementEventReference& aReference) {
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

inline auto eventFacts(const TradeTransaction& aEvent) {
    return std::tuple{aEvent.mTradeSide,
                      aEvent.mUnitPrice,
                      aEvent.mUnits,
                      aEvent.mAmount,
                      aEvent.mFeePaid,
                      aEvent.mExchangeRate,
                      aEvent.mCurrency};
}

inline auto eventFacts(const CorporateAction& aEvent) {
    return std::tuple{aEvent.mType, aEvent.mUnitsDelta, aEvent.mRatio};
}

inline auto eventFacts(const DividendTransaction& aEvent) {
    return std::tuple{aEvent.mGrossAmount,
                      aEvent.mTaxPaid,
                      aEvent.mExchangeRate,
                      aEvent.mCurrency,
                      aEvent.mTaxCurrency};
}

inline auto eventFacts(const InterestTransaction& aEvent) {
    return std::tuple{aEvent.mGrossAmount,
                      aEvent.mTaxPaid,
                      aEvent.mExchangeRate,
                      aEvent.mCurrency,
                      aEvent.mTaxCurrency};
}

inline auto eventFacts(const BenefitEvent& aEvent) {
    return std::tuple{aEvent.mType,
                      aEvent.mName,
                      aEvent.mIsin,
                      aEvent.mAssetClass,
                      aEvent.mAmount,
                      aEvent.mCurrency};
}

inline auto eventFacts(const PrivateMarketEvent& aEvent) {
    return std::tuple{aEvent.mType,
                      aEvent.mName,
                      aEvent.mIsin,
                      aEvent.mAssetClass,
                      aEvent.mAmount,
                      aEvent.mFeePaid,
                      aEvent.mCurrency,
                      aEvent.mDescription};
}

template <typename Event>
void expectEventsEqual(const std::vector<Event>& aActual,
                       const std::vector<Event>& aExpected,
                       bool aCompareSources) {
    ASSERT_EQ(aActual.size(), aExpected.size());

    for (std::size_t index = 0; index < aActual.size(); ++index)
    {
        SCOPED_TRACE(index);
        const auto& actual = aActual[index];
        const auto& expected = aExpected[index];

        EXPECT_EQ(eventFacts(actual), eventFacts(expected));
        EXPECT_EQ(actual.mMetadata.mTaxDate, expected.mMetadata.mTaxDate);
        EXPECT_EQ(actual.mMetadata.mSourceTimestamp, expected.mMetadata.mSourceTimestamp);
        EXPECT_EQ(actual.mMetadata.mOrderingTimestamp, expected.mMetadata.mOrderingTimestamp);

        if (aCompareSources)
        {
            EXPECT_EQ(actual.mMetadata.mSources, expected.mMetadata.mSources);

            if constexpr (std::is_same_v<Event, TradeTransaction> ||
                          std::is_same_v<Event, CorporateAction>)
            {
                EXPECT_EQ(actual.mUnitEvidence, expected.mUnitEvidence);
            }
        }
    }
}

template <typename Instrument>
void expectInstrumentsEqual(const std::vector<Instrument>& aActual,
                            const std::vector<Instrument>& aExpected,
                            bool aCompareSources) {
    ASSERT_EQ(aActual.size(), aExpected.size());

    for (std::size_t index = 0; index < aActual.size(); ++index)
    {
        SCOPED_TRACE(index);
        const auto& actual = aActual[index];
        const auto& expected = aExpected[index];

        EXPECT_EQ(actual.mName, expected.mName);
        EXPECT_EQ(actual.mIsin, expected.mIsin);
        expectEventsEqual(actual.mTransactions, expected.mTransactions, aCompareSources);

        if constexpr (std::is_same_v<Instrument, TradeInstrument>)
        {
            EXPECT_EQ(actual.mAssetClass, expected.mAssetClass);
            expectEventsEqual(actual.mCorporateActions,
                              expected.mCorporateActions,
                              aCompareSources);
        }
        else if constexpr (std::is_same_v<Instrument, InterestInstrument>)
        {
            EXPECT_EQ(actual.mInterestType, expected.mInterestType);
        }
        else if constexpr (!std::is_same_v<Instrument, DividendInstrument>)
        {
            static_assert(std::is_same_v<Instrument, void>, "Unsupported instrument comparison");
        }
    }
}

// Different file layouts retain different provenance even when the event facts agree.
inline void expectStatementsEqual(const BrokerStatement& aActual,
                                  const BrokerStatement& aExpected,
                                  bool aCompareSources = true) {
    expectInstrumentsEqual(aActual.mTradeInstruments, aExpected.mTradeInstruments, aCompareSources);
    expectInstrumentsEqual(aActual.mDividendInstruments,
                           aExpected.mDividendInstruments,
                           aCompareSources);
    expectInstrumentsEqual(aActual.mInterestInstruments,
                           aExpected.mInterestInstruments,
                           aCompareSources);
    expectEventsEqual(aActual.mBenefitEvents, aExpected.mBenefitEvents, aCompareSources);
    expectEventsEqual(aActual.mPrivateMarketEvents,
                      aExpected.mPrivateMarketEvents,
                      aCompareSources);
}

inline void expectDiagnosticsEqual(const std::vector<StatementMergeDiagnostic>& aActual,
                                   const std::vector<StatementMergeDiagnostic>& aExpected) {
    ASSERT_EQ(aActual.size(), aExpected.size());

    for (std::size_t index = 0; index < aActual.size(); ++index)
    {
        SCOPED_TRACE(index);
        ASSERT_EQ(aActual[index].index(), aExpected[index].index());

        const auto compareDiagnostic = [&expected = aExpected[index]](const auto& aValue) {
            using Diagnostic = std::decay_t<decltype(aValue)>;
            const auto& other = std::get<Diagnostic>(expected);

            if constexpr (std::is_same_v<Diagnostic, SourcedParseDiagnostic>)
            {
                EXPECT_EQ(aValue.mSourceIndex, other.mSourceIndex);
                EXPECT_EQ(aValue.mBroker, other.mBroker);

                const auto fields = [](const ParseDiagnostic& aDiagnostic) {
                    return std::tuple{aDiagnostic.mSeverity,
                                      aDiagnostic.mCode,
                                      aDiagnostic.mSourceFile,
                                      aDiagnostic.mRowIndex,
                                      aDiagnostic.mTransactionId,
                                      aDiagnostic.mField,
                                      aDiagnostic.mMessage};
                };

                EXPECT_EQ(fields(aValue.mDiagnostic), fields(other.mDiagnostic));
            }
            else if constexpr (std::is_same_v<Diagnostic, MergeDiagnostic>)
            {
                const auto fields = [](const MergeDiagnostic& aDiagnostic) {
                    return std::tuple{aDiagnostic.mSeverity,
                                      aDiagnostic.mCode,
                                      aDiagnostic.mMessage,
                                      aDiagnostic.mSources,
                                      aDiagnostic.mTaxDate,
                                      aDiagnostic.mInstrumentName,
                                      aDiagnostic.mIsin,
                                      aDiagnostic.mSourceIndex,
                                      aDiagnostic.mEventKinds};
                };

                EXPECT_EQ(fields(aValue), fields(other));
                ASSERT_EQ(aValue.mNameVariants.size(), other.mNameVariants.size());
                ASSERT_EQ(aValue.mAssetClassVariants.size(), other.mAssetClassVariants.size());

                for (std::size_t variant = 0; variant < aValue.mNameVariants.size(); ++variant)
                {
                    EXPECT_EQ(aValue.mNameVariants[variant].mName,
                              other.mNameVariants[variant].mName);
                    EXPECT_EQ(aValue.mNameVariants[variant].mSources,
                              other.mNameVariants[variant].mSources);
                }

                for (std::size_t variant = 0; variant < aValue.mAssetClassVariants.size();
                     ++variant)
                {
                    EXPECT_EQ(aValue.mAssetClassVariants[variant].mAssetClass,
                              other.mAssetClassVariants[variant].mAssetClass);
                    EXPECT_EQ(aValue.mAssetClassVariants[variant].mSources,
                              other.mAssetClassVariants[variant].mSources);
                }
            }
            else
            {
                static_assert(std::is_same_v<Diagnostic, void>,
                              "Unsupported diagnostic comparison");
            }
        };

        std::visit(compareDiagnostic, aActual[index]);
    }
}

inline void expectMergeResultsEqual(const StatementMergeResult& aActual,
                                    const StatementMergeResult& aExpected) {
    expectStatementsEqual(aActual.mStatement.mPresentation, aExpected.mStatement.mPresentation);
    EXPECT_EQ(aActual.mStatement.mChronologicalOrder, aExpected.mStatement.mChronologicalOrder);
    expectDiagnosticsEqual(aActual.mDiagnostics, aExpected.mDiagnostics);
}

} // namespace taxbroker::test
