#include "taxbroker/statement_merger.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace {
using namespace taxbroker;

// Remove identical source references contributed by overlapping exports.
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

struct NameContribution {
    std::string mName;
    std::vector<SourceReference> mSources;
};

struct NameVariantSet {
    std::string mCanonicalName;
    std::optional<SourceReference> mCanonicalSource;
    std::map<std::string, std::vector<SourceReference>> mSourcesByName;

    void add(const NameContribution& aContribution) {
        const auto source = std::min_element(aContribution.mSources.begin(),
                                             aContribution.mSources.end(),
                                             StableSourceOrder{});

        if (source != aContribution.mSources.end() &&
            (!mCanonicalSource || StableSourceOrder{}(*source, *mCanonicalSource)))
        {
            mCanonicalName = aContribution.mName;
            mCanonicalSource = *source;
        }

        auto& sources = mSourcesByName[aContribution.mName];
        sources.insert(sources.end(), aContribution.mSources.begin(), aContribution.mSources.end());
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

struct TradeRecord {
    std::string mName;
    Isin mIsin;
    AssetClass mAssetClass{AssetClass::Unknown};
    std::vector<NameContribution> mNameContributions;
    TradeTransaction mEvent;
};

struct CorporateActionRecord {
    std::string mName;
    Isin mIsin;
    AssetClass mAssetClass{AssetClass::Unknown};
    std::vector<NameContribution> mNameContributions;
    CorporateAction mEvent;
};

struct DividendRecord {
    std::string mName;
    Isin mIsin;
    std::vector<NameContribution> mNameContributions;
    DividendTransaction mEvent;
};

struct InterestRecord {
    std::string mName;
    std::optional<Isin> mIsin;
    InterestType mInterestType{InterestType::UnknownInterest};
    std::vector<NameContribution> mNameContributions;
    InterestTransaction mEvent;
};

using MergeEvent = std::variant<TradeRecord,
                                CorporateActionRecord,
                                DividendRecord,
                                InterestRecord,
                                BenefitEvent,
                                PrivateMarketEvent>;

struct TransactionIdentityOrder {
    [[nodiscard]] bool operator()(const TransactionIdentity& aLeft,
                                  const TransactionIdentity& aRight) const {
        if (aLeft.mBroker != aRight.mBroker)
        {
            return aLeft.mBroker < aRight.mBroker;
        }

        return aLeft.mTransactionId < aRight.mTransactionId;
    }
};

template <typename Event> void sortEvents(std::vector<Event>& aEvents) {
    std::stable_sort(aEvents.begin(), aEvents.end(), [](const Event& aLeft, const Event& aRight) {
        return ChronologicalEventOrder{}(aLeft.mMetadata, aRight.mMetadata);
    });
}

struct ChronologicalEntry {
    StatementEventReference mReference;
    const EventMetadata* mMetadata;
};

template <typename Event>
void appendChronologicalEntries(std::vector<ChronologicalEntry>& aEntries,
                                const std::vector<Event>& aEvents,
                                StatementEventKind aKind,
                                std::optional<std::size_t> aInstrumentIndex = std::nullopt) {
    for (std::size_t index = 0; index < aEvents.size(); ++index)
    {
        aEntries.push_back(ChronologicalEntry{
            .mReference =
                {
                    .mKind = aKind,
                    .mInstrumentIndex = aInstrumentIndex,
                    .mEventIndex = index,
                },
            .mMetadata = &aEvents[index].mMetadata,
        });
    }
}

void buildChronologicalOrder(MergedStatement& aStatement) {
    const auto& presentation = aStatement.mPresentation;
    std::vector<ChronologicalEntry> entries;

    for (std::size_t index = 0; index < presentation.mTradeInstruments.size(); ++index)
    {
        const auto& instrument = presentation.mTradeInstruments[index];

        appendChronologicalEntries(entries,
                                   instrument.mTransactions,
                                   StatementEventKind::Trade,
                                   index);
        appendChronologicalEntries(entries,
                                   instrument.mCorporateActions,
                                   StatementEventKind::CorporateAction,
                                   index);
    }

    for (std::size_t index = 0; index < presentation.mDividendInstruments.size(); ++index)
    {
        appendChronologicalEntries(entries,
                                   presentation.mDividendInstruments[index].mTransactions,
                                   StatementEventKind::Dividend,
                                   index);
    }

    for (std::size_t index = 0; index < presentation.mInterestInstruments.size(); ++index)
    {
        appendChronologicalEntries(entries,
                                   presentation.mInterestInstruments[index].mTransactions,
                                   StatementEventKind::Interest,
                                   index);
    }

    appendChronologicalEntries(entries, presentation.mBenefitEvents, StatementEventKind::Benefit);
    appendChronologicalEntries(entries,
                               presentation.mPrivateMarketEvents,
                               StatementEventKind::PrivateMarket);

    std::stable_sort(entries.begin(), entries.end(), [](const auto& aLeft, const auto& aRight) {
        return ChronologicalEventOrder{}(*aLeft.mMetadata, *aRight.mMetadata);
    });

    aStatement.mChronologicalOrder.reserve(entries.size());

    for (const auto& entry : entries)
    {
        aStatement.mChronologicalOrder.push_back(entry.mReference);
    }
}

[[nodiscard]] const EventMetadata& eventMetadata(const MergeEvent& aEvent) {
    const auto getMetadata = [](const auto& aValue) -> const EventMetadata& {
        if constexpr (std::is_same_v<std::decay_t<decltype(aValue)>, BenefitEvent> ||
                      std::is_same_v<std::decay_t<decltype(aValue)>, PrivateMarketEvent>)
        {
            return aValue.mMetadata;
        }
        else
        {
            return aValue.mEvent.mMetadata;
        }
    };

    return std::visit(getMetadata, aEvent);
}

[[nodiscard]] StatementEventKind eventKind(const MergeEvent& aEvent) {
    const auto getKind = [](const auto& aValue) {
        using Value = std::decay_t<decltype(aValue)>;

        if constexpr (std::is_same_v<Value, TradeRecord>)
        {
            return StatementEventKind::Trade;
        }
        else if constexpr (std::is_same_v<Value, CorporateActionRecord>)
        {
            return StatementEventKind::CorporateAction;
        }
        else if constexpr (std::is_same_v<Value, DividendRecord>)
        {
            return StatementEventKind::Dividend;
        }
        else if constexpr (std::is_same_v<Value, InterestRecord>)
        {
            return StatementEventKind::Interest;
        }
        else if constexpr (std::is_same_v<Value, BenefitEvent>)
        {
            return StatementEventKind::Benefit;
        }
        else if constexpr (std::is_same_v<Value, PrivateMarketEvent>)
        {
            return StatementEventKind::PrivateMarket;
        }
        else
        {
            static_assert(std::is_same_v<Value, void>,
                          "Missing StatementEventKind mapping for this event type.");
        }
    };

    return std::visit(getKind, aEvent);
}

[[nodiscard]] std::optional<Isin> eventIsin(const MergeEvent& aEvent) {
    const auto getIsin = [](const auto& aValue) -> std::optional<Isin> { return aValue.mIsin; };

    return std::visit(getIsin, aEvent);
}

[[nodiscard]] std::string eventName(const MergeEvent& aEvent) {
    const auto getName = [](const auto& aValue) { return aValue.mName; };

    return std::visit(getName, aEvent);
}

[[nodiscard]] bool sameMetadataFacts(const EventMetadata& aLeft, const EventMetadata& aRight) {
    return aLeft.mTaxDate == aRight.mTaxDate && aLeft.mSourceTimestamp == aRight.mSourceTimestamp;
}

[[nodiscard]] bool sameEventFacts(const TradeRecord& aLeft, const TradeRecord& aRight) {
    return sameMetadataFacts(aLeft.mEvent.mMetadata, aRight.mEvent.mMetadata) &&
           aLeft.mIsin == aRight.mIsin && aLeft.mAssetClass == aRight.mAssetClass &&
           aLeft.mEvent.mTradeSide == aRight.mEvent.mTradeSide &&
           aLeft.mEvent.mUnitPrice == aRight.mEvent.mUnitPrice &&
           aLeft.mEvent.mUnits == aRight.mEvent.mUnits &&
           aLeft.mEvent.mAmount == aRight.mEvent.mAmount &&
           aLeft.mEvent.mFeePaid == aRight.mEvent.mFeePaid &&
           aLeft.mEvent.mExchangeRate == aRight.mEvent.mExchangeRate &&
           aLeft.mEvent.mCurrency == aRight.mEvent.mCurrency;
}

[[nodiscard]] bool sameEventFacts(const CorporateActionRecord& aLeft,
                                  const CorporateActionRecord& aRight) {
    return sameMetadataFacts(aLeft.mEvent.mMetadata, aRight.mEvent.mMetadata) &&
           aLeft.mIsin == aRight.mIsin && aLeft.mAssetClass == aRight.mAssetClass &&
           aLeft.mEvent.mType == aRight.mEvent.mType &&
           aLeft.mEvent.mUnitsDelta == aRight.mEvent.mUnitsDelta &&
           aLeft.mEvent.mRatio == aRight.mEvent.mRatio;
}

[[nodiscard]] bool sameEventFacts(const DividendRecord& aLeft, const DividendRecord& aRight) {
    return sameMetadataFacts(aLeft.mEvent.mMetadata, aRight.mEvent.mMetadata) &&
           aLeft.mIsin == aRight.mIsin && aLeft.mEvent.mGrossAmount == aRight.mEvent.mGrossAmount &&
           aLeft.mEvent.mTaxPaid == aRight.mEvent.mTaxPaid &&
           aLeft.mEvent.mExchangeRate == aRight.mEvent.mExchangeRate &&
           aLeft.mEvent.mCurrency == aRight.mEvent.mCurrency &&
           aLeft.mEvent.mTaxCurrency == aRight.mEvent.mTaxCurrency;
}

[[nodiscard]] bool sameEventFacts(const InterestRecord& aLeft, const InterestRecord& aRight) {
    const bool sameInstrument =
        aLeft.mIsin ? aLeft.mIsin == aRight.mIsin : !aRight.mIsin && aLeft.mName == aRight.mName;

    return sameMetadataFacts(aLeft.mEvent.mMetadata, aRight.mEvent.mMetadata) && sameInstrument &&
           aLeft.mInterestType == aRight.mInterestType &&
           aLeft.mEvent.mGrossAmount == aRight.mEvent.mGrossAmount &&
           aLeft.mEvent.mTaxPaid == aRight.mEvent.mTaxPaid &&
           aLeft.mEvent.mExchangeRate == aRight.mEvent.mExchangeRate &&
           aLeft.mEvent.mCurrency == aRight.mEvent.mCurrency &&
           aLeft.mEvent.mTaxCurrency == aRight.mEvent.mTaxCurrency;
}

[[nodiscard]] bool sameEventFacts(const BenefitEvent& aLeft, const BenefitEvent& aRight) {
    return sameMetadataFacts(aLeft.mMetadata, aRight.mMetadata) && aLeft.mType == aRight.mType &&
           aLeft.mName == aRight.mName && aLeft.mIsin == aRight.mIsin &&
           aLeft.mAssetClass == aRight.mAssetClass && aLeft.mAmount == aRight.mAmount &&
           aLeft.mCurrency == aRight.mCurrency;
}

[[nodiscard]] bool sameEventFacts(const PrivateMarketEvent& aLeft,
                                  const PrivateMarketEvent& aRight) {
    return sameMetadataFacts(aLeft.mMetadata, aRight.mMetadata) && aLeft.mType == aRight.mType &&
           aLeft.mName == aRight.mName && aLeft.mIsin == aRight.mIsin &&
           aLeft.mAssetClass == aRight.mAssetClass && aLeft.mAmount == aRight.mAmount &&
           aLeft.mFeePaid == aRight.mFeePaid && aLeft.mCurrency == aRight.mCurrency &&
           aLeft.mDescription == aRight.mDescription;
}

[[nodiscard]] bool sameEventFacts(const MergeEvent& aLeft, const MergeEvent& aRight) {
    if (aLeft.index() != aRight.index())
    {
        return false;
    }

    const auto compareFacts = [&aRight](const auto& aLeftValue) {
        using Value = std::decay_t<decltype(aLeftValue)>;

        return sameEventFacts(aLeftValue, std::get<Value>(aRight));
    };

    return std::visit(compareFacts, aLeft);
}

void mergeExactEvent(MergeEvent& aDestination, const MergeEvent& aSource) {
    const auto mergeProvenance = [&aSource](auto& aDestinationValue) {
        using Value = std::decay_t<decltype(aDestinationValue)>;
        const auto& source = std::get<Value>(aSource);

        if constexpr (std::is_same_v<Value, BenefitEvent> ||
                      std::is_same_v<Value, PrivateMarketEvent>)
        {
            appendSources(aDestinationValue.mMetadata.mSources, source.mMetadata);
            normalizeSources(aDestinationValue.mMetadata.mSources);
        }
        else
        {
            appendSources(aDestinationValue.mEvent.mMetadata.mSources, source.mEvent.mMetadata);
            aDestinationValue.mNameContributions.insert(aDestinationValue.mNameContributions.end(),
                                                        source.mNameContributions.begin(),
                                                        source.mNameContributions.end());

            normalizeSources(aDestinationValue.mEvent.mMetadata.mSources);
        }
    };

    std::visit(mergeProvenance, aDestination);
}

void addConflictingDuplicateDiagnostic(const std::vector<MergeEvent>& aEvents,
                                       std::span<const std::size_t> aCandidates,
                                       std::vector<MergeDiagnostic>& aDiagnostics) {
    const auto& first = aEvents[aCandidates.front()];

    MergeDiagnostic diagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = MergeDiagnosticCode::ConflictingDuplicate,
        .mMessage = "Events with the same broker transaction ID have conflicting "
                    "tax-relevant data and were not merged.",
        .mTaxDate = eventMetadata(first).mTaxDate,
        .mInstrumentName = eventName(first),
        .mIsin = eventIsin(first),
    };

    for (const auto index : aCandidates)
    {
        const auto& event = aEvents[index];

        appendSources(diagnostic.mSources, eventMetadata(event));
        diagnostic.mEventKinds.push_back(eventKind(event));

        if (diagnostic.mTaxDate != eventMetadata(event).mTaxDate)
        {
            diagnostic.mTaxDate.reset();
        }

        if (diagnostic.mInstrumentName != eventName(event) || eventName(event).empty())
        {
            diagnostic.mInstrumentName.reset();
        }

        if (diagnostic.mIsin != eventIsin(event))
        {
            diagnostic.mIsin.reset();
        }
    }

    normalizeSources(diagnostic.mSources);

    auto& kinds = diagnostic.mEventKinds;
    std::sort(kinds.begin(), kinds.end());
    kinds.erase(std::unique(kinds.begin(), kinds.end()), kinds.end());

    aDiagnostics.push_back(std::move(diagnostic));
}

void deduplicateEvents(std::vector<MergeEvent>& aEvents,
                       std::vector<MergeDiagnostic>& aDiagnostics) {
    std::stable_sort(aEvents.begin(),
                     aEvents.end(),
                     [](const MergeEvent& aLeft, const MergeEvent& aRight) {
                         return StableSourceOrder{}(primarySource(eventMetadata(aLeft)),
                                                    primarySource(eventMetadata(aRight)));
                     });

    std::map<TransactionIdentity, std::vector<std::size_t>, TransactionIdentityOrder> groups;
    std::vector<bool> rejected(aEvents.size(), false);

    for (std::size_t index = 0; index < aEvents.size(); ++index)
    {
        const auto identity = transactionIdentity(eventMetadata(aEvents[index]));

        if (identity)
        {
            groups[*identity].push_back(index);
        }
    }

    for (auto& group : groups)
    {
        auto& candidates = group.second;
        auto& canonical = aEvents[candidates.front()];

        const bool allMatch =
            std::all_of(candidates.begin() + 1, candidates.end(), [&](std::size_t aIndex) {
                return sameEventFacts(canonical, aEvents[aIndex]);
            });

        if (!allMatch)
        {
            addConflictingDuplicateDiagnostic(aEvents, candidates, aDiagnostics);
            rejected[candidates.front()] = true;
        }

        for (std::size_t index = 1; index < candidates.size(); ++index)
        {
            if (allMatch)
            {
                mergeExactEvent(canonical, aEvents[candidates[index]]);
            }

            rejected[candidates[index]] = true;
        }
    }

    std::vector<MergeEvent> retainedEvents;

    for (std::size_t index = 0; index < aEvents.size(); ++index)
    {
        if (!rejected[index])
        {
            retainedEvents.push_back(std::move(aEvents[index]));
        }
    }

    aEvents = std::move(retainedEvents);
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

[[nodiscard]] InterestIdentity interestIdentity(const InterestRecord& aRecord) {
    return InterestIdentity{
        .mType = aRecord.mInterestType,
        .mIsin = aRecord.mIsin,
        .mNameWithoutIsin = aRecord.mIsin ? std::string{} : aRecord.mName,
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

    std::vector<MergeEvent> events;

    for (const auto* input : orderedInputs)
    {
        if (duplicateSourceIndices.contains(input->mSourceIndex))
        {
            continue;
        }

        for (const auto& sourceInstrument : input->mParseResult.mStatement.mTradeInstruments)
        {
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

                events.emplace_back(TradeRecord{
                    .mName = sourceInstrument.mName,
                    .mIsin = sourceInstrument.mIsin,
                    .mAssetClass = sourceInstrument.mAssetClass,
                    .mNameContributions = {{
                        .mName = sourceInstrument.mName,
                        .mSources = event.mMetadata.mSources,
                    }},
                    .mEvent = std::move(event),
                });
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

                events.emplace_back(CorporateActionRecord{
                    .mName = sourceInstrument.mName,
                    .mIsin = sourceInstrument.mIsin,
                    .mAssetClass = sourceInstrument.mAssetClass,
                    .mNameContributions = {{
                        .mName = sourceInstrument.mName,
                        .mSources = event.mMetadata.mSources,
                    }},
                    .mEvent = std::move(event),
                });
            }
        }

        for (const auto& sourceInstrument : input->mParseResult.mStatement.mDividendInstruments)
        {
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

                events.emplace_back(DividendRecord{
                    .mName = sourceInstrument.mName,
                    .mIsin = sourceInstrument.mIsin,
                    .mNameContributions = {{
                        .mName = sourceInstrument.mName,
                        .mSources = event.mMetadata.mSources,
                    }},
                    .mEvent = std::move(event),
                });
            }
        }

        for (const auto& sourceInstrument : input->mParseResult.mStatement.mInterestInstruments)
        {
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

                events.emplace_back(InterestRecord{
                    .mName = sourceInstrument.mName,
                    .mIsin = sourceInstrument.mIsin,
                    .mInterestType = sourceInstrument.mInterestType,
                    .mNameContributions = {{
                        .mName = sourceInstrument.mName,
                        .mSources = event.mMetadata.mSources,
                    }},
                    .mEvent = std::move(event),
                });
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
                events.emplace_back(std::move(event));
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
                events.emplace_back(std::move(event));
            }
        }
    }

    deduplicateEvents(events, mergeDiagnostics);

    std::map<Isin, TradeBucket> tradeBuckets;
    std::map<Isin, DividendBucket> dividendBuckets;
    std::map<InterestIdentity, InterestBucket, InterestIdentityOrder> interestBuckets;
    std::vector<BenefitEvent> benefitEvents;
    std::vector<PrivateMarketEvent> privateMarketEvents;

    const auto addToPresentation = [&](auto& aValue) {
        using Value = std::decay_t<decltype(aValue)>;

        if constexpr (std::is_same_v<Value, TradeRecord>)
        {
            auto& bucket = tradeBuckets[aValue.mIsin];

            for (const auto& contribution : aValue.mNameContributions)
            {
                bucket.mNames.add(contribution);
            }

            bucket.mAssetClasses.add(aValue.mAssetClass, aValue.mEvent.mMetadata);
            bucket.mTransactions.push_back(std::move(aValue.mEvent));
        }
        else if constexpr (std::is_same_v<Value, CorporateActionRecord>)
        {
            auto& bucket = tradeBuckets[aValue.mIsin];

            for (const auto& contribution : aValue.mNameContributions)
            {
                bucket.mNames.add(contribution);
            }

            bucket.mAssetClasses.add(aValue.mAssetClass, aValue.mEvent.mMetadata);
            bucket.mCorporateActions.push_back(std::move(aValue.mEvent));
        }
        else if constexpr (std::is_same_v<Value, DividendRecord>)
        {
            auto& bucket = dividendBuckets[aValue.mIsin];

            for (const auto& contribution : aValue.mNameContributions)
            {
                bucket.mNames.add(contribution);
            }

            bucket.mTransactions.push_back(std::move(aValue.mEvent));
        }
        else if constexpr (std::is_same_v<Value, InterestRecord>)
        {
            auto& bucket = interestBuckets[interestIdentity(aValue)];

            for (const auto& contribution : aValue.mNameContributions)
            {
                bucket.mNames.add(contribution);
            }

            bucket.mTransactions.push_back(std::move(aValue.mEvent));
        }
        else if constexpr (std::is_same_v<Value, BenefitEvent>)
        {
            benefitEvents.push_back(std::move(aValue));
        }
        else if constexpr (std::is_same_v<Value, PrivateMarketEvent>)
        {
            privateMarketEvents.push_back(std::move(aValue));
        }
        else
        {
            static_assert(std::is_same_v<Value, void>,
                          "Missing presentation handling for this event type.");
        }
    };

    for (auto& event : events)
    {
        std::visit(addToPresentation, event);
    }

    for (auto& [isin, bucket] : tradeBuckets)
    {
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

    buildChronologicalOrder(result.mStatement);

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
