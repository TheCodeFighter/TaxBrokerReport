#include "taxbroker/statement_merger.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace {
using namespace taxbroker;

// remove identical source references (from different inputs)
void normalizeSources(std::vector<SourceReference>& aSources) {
    std::sort(aSources.begin(), aSources.end(), StableSourceOrder{});
    aSources.erase(std::unique(aSources.begin(), aSources.end()), aSources.end());
}

void appendSources(std::vector<SourceReference>& aDestination, const EventMetadata& aMetadata) {
    aDestination.insert(aDestination.end(), aMetadata.mSources.begin(), aMetadata.mSources.end());
}

template <typename Event>
void appendEventSources(std::vector<SourceReference>& aDestination,
                        const std::vector<Event>& aEvents) {
    for (const auto& event : aEvents)
    {
        appendSources(aDestination, event.mMetadata);
    }
}

std::vector<SourceReference> statementSources(const BrokerStatement& aStatement) {
    std::vector<SourceReference> sources;

    for (const auto& instrument : aStatement.mTradeInstruments)
    {
        appendEventSources(sources, instrument.mTransactions);
        appendEventSources(sources, instrument.mCorporateActions);
    }

    for (const auto& instrument : aStatement.mDividendInstruments)
    {
        appendEventSources(sources, instrument.mTransactions);
    }

    for (const auto& instrument : aStatement.mInterestInstruments)
    {
        appendEventSources(sources, instrument.mTransactions);
    }

    appendEventSources(sources, aStatement.mBenefitEvents);
    appendEventSources(sources, aStatement.mPrivateMarketEvents);

    normalizeSources(sources);

    return sources;
}

struct NameVariantSet {
    std::string mCanonicalName;
    std::optional<SourceReference> mCanonicalSource;
    std::map<std::string, std::vector<SourceReference>> mSourcesByName;

    void add(const std::string& aName, const EventMetadata& aMetadata) {
        const auto& source = primarySource(aMetadata);

        if (!mCanonicalSource || StableSourceOrder{}(source, *mCanonicalSource))
        {
            mCanonicalName = aName;
            mCanonicalSource = source;
        }

        appendSources(mSourcesByName[aName], aMetadata);
    }
};

struct AssetClassVariantSet {
    std::map<AssetClass, std::vector<SourceReference>> mSourcesByAssetClass;

    void add(AssetClass aAssetClass, const EventMetadata& aMetadata) {
        appendSources(mSourcesByAssetClass[aAssetClass], aMetadata);
    }
};

struct TradeBucket {
    NameVariantSet mNames;
    AssetClassVariantSet mAssetClasses;
    std::vector<TradeTransaction> mTransactions;
    std::vector<CorporateAction> mCorporateActions;
};

struct DividendBucket {
    NameVariantSet mNames;
    std::vector<DividendTransaction> mTransactions;
};

struct InterestIdentity {
    InterestType mType{InterestType::UnknownInterest};
    std::optional<Isin> mIsin;
    std::string mNameWithoutIsin;
};

[[nodiscard]] int interestTypeRank(InterestType aType) {
    switch (aType)
    {
    case InterestType::BondInterest:
        return 0;
    case InterestType::BrokerInterest:
        return 1;
    case InterestType::OtherInterest:
        return 2;
    case InterestType::UnknownInterest:
        return 3;
    }
    return 3;
}

struct InterestIdentityOrder {
    [[nodiscard]] bool operator()(const InterestIdentity& aLeft,
                                  const InterestIdentity& aRight) const {
        const auto leftRank = interestTypeRank(aLeft.mType);
        const auto rightRank = interestTypeRank(aRight.mType);

        if (leftRank != rightRank)
        {
            return leftRank < rightRank;
        }

        const bool leftHasIsin = aLeft.mIsin.has_value();
        const bool rightHasIsin = aRight.mIsin.has_value();

        if (leftHasIsin != rightHasIsin)
        {
            return leftHasIsin;
        }

        if (aLeft.mIsin != aRight.mIsin)
        {
            return aLeft.mIsin < aRight.mIsin;
        }

        return aLeft.mNameWithoutIsin < aRight.mNameWithoutIsin;
    }
};

struct InterestBucket {
    NameVariantSet mNames;
    std::vector<InterestTransaction> mTransactions;
};

template <typename Event> void sortEvents(std::vector<Event>& aEvents) {
    std::stable_sort(aEvents.begin(), aEvents.end(), [](const Event& aLeft, const Event& aRight) {
        return ChronologicalEventOrder{}(aLeft.mMetadata, aRight.mMetadata);
    });
}

[[nodiscard]] std::vector<InstrumentNameVariant> makeNameVariants(NameVariantSet& aNames) {
    std::vector<InstrumentNameVariant> variants;
    variants.reserve(aNames.mSourcesByName.size());

    for (auto& [name, sources] : aNames.mSourcesByName)
    {
        normalizeSources(sources);

        variants.push_back(InstrumentNameVariant{
            .mName = name,
            .mSources = sources,
        });
    }

    return variants;
}

[[nodiscard]] std::vector<InstrumentAssetClassVariant>
makeAssetClassVariants(AssetClassVariantSet& aAssetClasses) {
    std::vector<InstrumentAssetClassVariant> variants;
    variants.reserve(aAssetClasses.mSourcesByAssetClass.size());

    for (auto& [assetClass, sources] : aAssetClasses.mSourcesByAssetClass)
    {
        normalizeSources(sources);

        variants.push_back(InstrumentAssetClassVariant{
            .mAssetClass = assetClass,
            .mSources = sources,
        });
    }

    return variants;
}

template <typename Variant>
[[nodiscard]] std::vector<SourceReference> variantSources(const std::vector<Variant>& aVariants) {
    std::vector<SourceReference> sources;

    for (const auto& variant : aVariants)
    {
        sources.insert(sources.end(), variant.mSources.begin(), variant.mSources.end());
    }

    normalizeSources(sources);

    return sources;
}

void addNameConflictDiagnostic(const Isin& aIsin,
                               NameVariantSet& aNames,
                               std::vector<MergeDiagnostic>& aDiagnostics) {
    if (aNames.mSourcesByName.size() < 2)
    {
        return;
    }

    auto variants = makeNameVariants(aNames);
    auto sources = variantSources(variants);

    aDiagnostics.push_back(MergeDiagnostic{
        .mSeverity = DiagnosticSeverity::Warning,
        .mCode = MergeDiagnosticCode::InstrumentNameConflict,
        .mMessage =
            "Instrument names differ for the same identity; the earliest source name is used "
            "for presentation.",
        .mSources = std::move(sources),
        .mInstrumentName = aNames.mCanonicalName,
        .mIsin = aIsin,
        .mNameVariants = std::move(variants),
    });
}

void addAssetClassConflictDiagnostic(const Isin& aIsin,
                                     const std::string& aName,
                                     AssetClassVariantSet& aAssetClasses,
                                     std::vector<MergeDiagnostic>& aDiagnostics) {
    if (aAssetClasses.mSourcesByAssetClass.size() < 2)
    {
        return;
    }

    auto variants = makeAssetClassVariants(aAssetClasses);
    auto sources = variantSources(variants);

    aDiagnostics.push_back(MergeDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = MergeDiagnosticCode::InstrumentAssetClassConflict,
        .mMessage = "Asset classes differ for the same ISIN; the merged asset class is unknown.",
        .mSources = std::move(sources),
        .mInstrumentName = aName,
        .mIsin = aIsin,
        .mAssetClassVariants = std::move(variants),
    });
}

template <typename Event>
[[nodiscard]] bool normalizeAndValidateEvent(Event& aEvent,
                                             std::size_t aSourceIndex,
                                             std::optional<std::string> aInstrumentName,
                                             std::optional<Isin> aIsin,
                                             std::vector<MergeDiagnostic>& aDiagnostics) {
    normalizeSources(aEvent.mMetadata.mSources);

    const bool hasConsistentSource =
        !aEvent.mMetadata.mSources.empty() &&
        std::all_of(aEvent.mMetadata.mSources.begin(),
                    aEvent.mMetadata.mSources.end(),
                    [aSourceIndex](const SourceReference& aSource) {
                        return aSource.mInputSequence.mSourceIndex == aSourceIndex;
                    });

    if (hasConsistentSource)
    {
        return true;
    }

    aDiagnostics.push_back(MergeDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = MergeDiagnosticCode::InconsistentSourceIndex,
        .mMessage = aEvent.mMetadata.mSources.empty()
                        ? "An event has no source reference and was not merged."
                        : "An event source index does not match its merge input and was not "
                          "merged.",
        .mSources = aEvent.mMetadata.mSources,
        .mTaxDate = aEvent.mMetadata.mTaxDate,
        .mInstrumentName = std::move(aInstrumentName),
        .mIsin = std::move(aIsin),
        .mSourceIndex = aSourceIndex,
    });

    return false;
}

[[nodiscard]] InterestIdentity interestIdentity(const InterestInstrument& aInstrument) {
    return InterestIdentity{
        .mType = aInstrument.mInterestType,
        .mIsin = aInstrument.mIsin,
        .mNameWithoutIsin = aInstrument.mIsin ? std::string{} : aInstrument.mName,
    };
}

[[nodiscard]] auto mergeDiagnosticKey(const MergeDiagnostic& aDiagnostic) {
    const auto missing = std::numeric_limits<std::size_t>::max();
    std::size_t sourceIndex = aDiagnostic.mSourceIndex.value_or(missing);
    std::size_t eventIndex = missing;

    if (!aDiagnostic.mSources.empty())
    {
        const auto& source = aDiagnostic.mSources.front();
        sourceIndex = source.mInputSequence.mSourceIndex;
        eventIndex = source.mInputSequence.mEventIndex;
    }

    return std::tuple{
        sourceIndex,
        eventIndex,
        !aDiagnostic.mTaxDate.has_value(),
        aDiagnostic.mTaxDate.value_or(Date{}),
        !aDiagnostic.mIsin.has_value(),
        aDiagnostic.mIsin.value_or(Isin{}),
        aDiagnostic.mSeverity == DiagnosticSeverity::Error ? 0 : 1,
        static_cast<int>(aDiagnostic.mCode),
    };
}

} // namespace

namespace taxbroker {

StatementMergeResult
DeterministicStatementMerger::merge(std::span<const StatementMergeInput> aInputs) const {
    StatementMergeResult result;
    std::vector<const StatementMergeInput*> orderedInputs(aInputs.size());

    std::transform(aInputs.begin(),
                   aInputs.end(),
                   orderedInputs.begin(),
                   [](const StatementMergeInput& aInput) { return &aInput; });

    std::stable_sort(orderedInputs.begin(),
                     orderedInputs.end(),
                     [](const StatementMergeInput* aLeft, const StatementMergeInput* aRight) {
                         if (aLeft->mSourceIndex != aRight->mSourceIndex)
                         {
                             return aLeft->mSourceIndex < aRight->mSourceIndex;
                         }
                         return aLeft->mParseResult.mBroker < aRight->mParseResult.mBroker;
                     });

    for (const auto* input : orderedInputs)
    {
        for (const auto& diagnostic : input->mParseResult.mDiagnostics)
        {
            result.mDiagnostics.emplace_back(SourcedParseDiagnostic{
                .mSourceIndex = input->mSourceIndex,
                .mBroker = input->mParseResult.mBroker,
                .mDiagnostic = diagnostic,
            });
        }
    }

    std::vector<MergeDiagnostic> mergeDiagnostics;
    std::set<std::size_t> duplicateSourceIndices;

    for (std::size_t begin = 0; begin < orderedInputs.size();)
    {
        std::size_t end = begin + 1;

        while (end < orderedInputs.size() &&
               orderedInputs[end]->mSourceIndex == orderedInputs[begin]->mSourceIndex)
        {
            ++end;
        }

        if (end - begin > 1)
        {
            const auto sourceIndex = orderedInputs[begin]->mSourceIndex;
            duplicateSourceIndices.insert(sourceIndex);

            std::vector<SourceReference> sources;

            for (std::size_t index = begin; index < end; ++index)
            {
                auto inputSources = statementSources(orderedInputs[index]->mParseResult.mStatement);

                sources.insert(sources.end(), inputSources.begin(), inputSources.end());
            }

            normalizeSources(sources);

            mergeDiagnostics.push_back(MergeDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = MergeDiagnosticCode::DuplicateSourceIndex,
                .mMessage = "Multiple merge inputs use the same source index; their events were "
                            "not merged.",
                .mSources = std::move(sources),
                .mSourceIndex = sourceIndex,
            });
        }

        begin = end;
    }

    std::map<Isin, TradeBucket> tradeBuckets;
    std::map<Isin, DividendBucket> dividendBuckets;
    std::map<InterestIdentity, InterestBucket, InterestIdentityOrder> interestBuckets;
    std::vector<BenefitEvent> benefitEvents;
    std::vector<PrivateMarketEvent> privateMarketEvents;

    for (const auto* input : orderedInputs)
    {
        if (duplicateSourceIndices.contains(input->mSourceIndex))
        {
            continue;
        }

        for (const auto& sourceInstrument : input->mParseResult.mStatement.mTradeInstruments)
        {
            auto& bucket = tradeBuckets[sourceInstrument.mIsin];

            for (auto event : sourceInstrument.mTransactions)
            {
                if (!normalizeAndValidateEvent(event,
                                               input->mSourceIndex,
                                               sourceInstrument.mName,
                                               sourceInstrument.mIsin,
                                               mergeDiagnostics))
                {
                    continue;
                }

                bucket.mNames.add(sourceInstrument.mName, event.mMetadata);
                bucket.mAssetClasses.add(sourceInstrument.mAssetClass, event.mMetadata);
                bucket.mTransactions.push_back(std::move(event));
            }

            for (auto event : sourceInstrument.mCorporateActions)
            {
                if (!normalizeAndValidateEvent(event,
                                               input->mSourceIndex,
                                               sourceInstrument.mName,
                                               sourceInstrument.mIsin,
                                               mergeDiagnostics))
                {
                    continue;
                }

                bucket.mNames.add(sourceInstrument.mName, event.mMetadata);
                bucket.mAssetClasses.add(sourceInstrument.mAssetClass, event.mMetadata);
                bucket.mCorporateActions.push_back(std::move(event));
            }
        }

        for (const auto& sourceInstrument : input->mParseResult.mStatement.mDividendInstruments)
        {
            auto& bucket = dividendBuckets[sourceInstrument.mIsin];

            for (auto event : sourceInstrument.mTransactions)
            {
                if (!normalizeAndValidateEvent(event,
                                               input->mSourceIndex,
                                               sourceInstrument.mName,
                                               sourceInstrument.mIsin,
                                               mergeDiagnostics))
                {
                    continue;
                }

                bucket.mNames.add(sourceInstrument.mName, event.mMetadata);
                bucket.mTransactions.push_back(std::move(event));
            }
        }

        for (const auto& sourceInstrument : input->mParseResult.mStatement.mInterestInstruments)
        {
            const auto identity = interestIdentity(sourceInstrument);
            auto& bucket = interestBuckets[identity];

            for (auto event : sourceInstrument.mTransactions)
            {
                if (!normalizeAndValidateEvent(event,
                                               input->mSourceIndex,
                                               sourceInstrument.mName,
                                               sourceInstrument.mIsin,
                                               mergeDiagnostics))
                {
                    continue;
                }

                bucket.mNames.add(sourceInstrument.mName, event.mMetadata);
                bucket.mTransactions.push_back(std::move(event));
            }
        }

        for (auto event : input->mParseResult.mStatement.mBenefitEvents)
        {
            if (normalizeAndValidateEvent(event,
                                          input->mSourceIndex,
                                          event.mName,
                                          event.mIsin,
                                          mergeDiagnostics))
            {
                benefitEvents.push_back(std::move(event));
            }
        }

        for (auto event : input->mParseResult.mStatement.mPrivateMarketEvents)
        {
            if (normalizeAndValidateEvent(event,
                                          input->mSourceIndex,
                                          event.mName,
                                          event.mIsin,
                                          mergeDiagnostics))
            {
                privateMarketEvents.push_back(std::move(event));
            }
        }
    }

    for (auto& [isin, bucket] : tradeBuckets)
    {
        if (bucket.mTransactions.empty() && bucket.mCorporateActions.empty())
        {
            continue;
        }

        sortEvents(bucket.mTransactions);
        sortEvents(bucket.mCorporateActions);

        const auto assetClass = bucket.mAssetClasses.mSourcesByAssetClass.size() == 1
                                    ? bucket.mAssetClasses.mSourcesByAssetClass.begin()->first
                                    : AssetClass::Unknown;

        result.mStatement.mPresentation.mTradeInstruments.push_back(TradeInstrument{
            .mName = bucket.mNames.mCanonicalName,
            .mIsin = isin,
            .mAssetClass = assetClass,
            .mTransactions = std::move(bucket.mTransactions),
            .mCorporateActions = std::move(bucket.mCorporateActions),
        });

        addNameConflictDiagnostic(isin, bucket.mNames, mergeDiagnostics);
        addAssetClassConflictDiagnostic(isin,
                                        bucket.mNames.mCanonicalName,
                                        bucket.mAssetClasses,
                                        mergeDiagnostics);
    }

    for (auto& [isin, bucket] : dividendBuckets)
    {
        if (bucket.mTransactions.empty())
        {
            continue;
        }

        sortEvents(bucket.mTransactions);

        result.mStatement.mPresentation.mDividendInstruments.push_back(DividendInstrument{
            .mName = bucket.mNames.mCanonicalName,
            .mIsin = isin,
            .mTransactions = std::move(bucket.mTransactions),
        });

        addNameConflictDiagnostic(isin, bucket.mNames, mergeDiagnostics);
    }

    for (auto& [identity, bucket] : interestBuckets)
    {
        if (bucket.mTransactions.empty())
        {
            continue;
        }

        sortEvents(bucket.mTransactions);

        result.mStatement.mPresentation.mInterestInstruments.push_back(InterestInstrument{
            .mName = bucket.mNames.mCanonicalName,
            .mIsin = identity.mIsin,
            .mInterestType = identity.mType,
            .mTransactions = std::move(bucket.mTransactions),
        });

        if (identity.mIsin)
        {
            addNameConflictDiagnostic(*identity.mIsin, bucket.mNames, mergeDiagnostics);
        }
    }

    sortEvents(benefitEvents);
    sortEvents(privateMarketEvents);

    result.mStatement.mPresentation.mBenefitEvents = std::move(benefitEvents);
    result.mStatement.mPresentation.mPrivateMarketEvents = std::move(privateMarketEvents);

    std::stable_sort(mergeDiagnostics.begin(),
                     mergeDiagnostics.end(),
                     [](const MergeDiagnostic& aLeft, const MergeDiagnostic& aRight) {
                         return mergeDiagnosticKey(aLeft) < mergeDiagnosticKey(aRight);
                     });

    for (auto& diagnostic : mergeDiagnostics)
    {
        result.mDiagnostics.emplace_back(std::move(diagnostic));
    }

    return result;
}

} // namespace taxbroker
