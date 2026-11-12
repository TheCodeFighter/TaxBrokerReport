#include "processors/application_summaries.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <variant>

namespace taxbroker {

namespace {

const TradeTransaction& tradeOf(const MergedStatement& aStatement,
                                const StatementEventReference& aEvent) {
    return aStatement.mPresentation.mTradeInstruments.at(aEvent.mInstrumentIndex.value())
        .mTransactions.at(aEvent.mEventIndex);
}

/// What is known about one ISIN with a sale in the year.
struct InvestmentDraft {
    std::string mName;
    bool mListed{};
    std::size_t mListedSales{};
    Units mListedUnits{};
    Units mBlockedLossUnits{};
    std::vector<StatementEventReference> mSales;
};

} // namespace

void appendCapitalSummary(ApplicationResultInput& aInput,
                          const MergedStatement& aStatement,
                          const CapitalGainsBuildResult& aBuilt,
                          const CapitalGainsReportKDVP& aPreview) {
    std::map<Isin, InvestmentDraft> drafts;

    for (const auto& list : aPreview.mLists)
    {
        auto& draft = drafts[list.mIsin];

        draft.mName = list.mName;
        draft.mListed = true;

        for (const auto& row : list.mRows)
        {
            if (const auto* sale = std::get_if<KdvpSaleRow>(&row.mEntry))
            {
                // The rows of one sale follow each other.
                if (draft.mSales.empty() || draft.mSales.back() != sale->mSale)
                {
                    ++draft.mListedSales;
                    draft.mSales.push_back(sale->mSale);
                }

                draft.mListedUnits += sale->mUnits;
                draft.mBlockedLossUnits +=
                    sale->mLossRule == KdvpLossRule::Blocked ? sale->mUnits : Units{0};
            }
        }
    }

    for (const auto& longHeld : aBuilt.mLongHeld)
    {
        auto& draft = drafts[longHeld.mIsin];

        draft.mName = longHeld.mName;
        draft.mSales.push_back(longHeld.mSale);

        const auto& metadata = tradeOf(aStatement, longHeld.mSale).mMetadata;

        aInput.mLongHeld.push_back({.mIsin = longHeld.mIsin,
                                    .mName = longHeld.mName,
                                    .mSaleDate = metadata.mTaxDate,
                                    .mUnits = longHeld.mUnits});
    }

    for (const auto& blocked : aBuilt.mBlocked)
    {
        auto& draft = drafts[blocked.mIsin];

        draft.mName = blocked.mName;
        draft.mSales = blocked.mSales;
    }

    for (const auto& excluded : aBuilt.mExcluded)
    {
        auto& draft = drafts[excluded.mIsin];

        draft.mName = excluded.mName;
        draft.mSales = excluded.mSales;
    }

    for (auto& [isin, draft] : drafts)
    {
        auto& sales = draft.mSales;

        std::sort(sales.begin(),
                  sales.end(),
                  [](const StatementEventReference& aLeft, const StatementEventReference& aRight) {
                      return aLeft.mEventIndex < aRight.mEventIndex;
                  });
        sales.erase(std::unique(sales.begin(), sales.end()), sales.end());

        CapitalInvestmentInput input{
            .mIsin = isin,
            .mName = std::move(draft.mName),
            .mListed = draft.mListed,
            .mListedSales = draft.mListedSales,
            .mListedUnits = draft.mListedUnits,
            .mBlockedLossUnits = draft.mBlockedLossUnits,
        };

        for (const auto& sale : sales)
        {
            const auto& metadata = tradeOf(aStatement, sale).mMetadata;

            input.mSales.emplace_back(metadata.mTaxDate, primarySource(metadata));
        }

        aInput.mCapitalInvestments.push_back(std::move(input));
    }
}

void appendCorrections(ApplicationResultInput& aInput,
                       const MergedStatement& aStatement,
                       const FifoMatchingResult& aFifo) {
    for (const auto& pool : aFifo.mIsins)
    {
        for (const auto& correction : pool.mCorrections)
        {
            const auto& metadata = tradeOf(aStatement, correction.mSale).mMetadata;
            const auto kind = correction.mKind == QuantityCorrectionKind::RoundingEvidence
                                  ? CorrectionKind::RoundingEvidence
                                  : CorrectionKind::UserConfirmed;

            aInput.mCorrections.push_back({
                .mIsin = pool.mIsin,
                .mSaleDate = metadata.mTaxDate,
                .mSaleSource = primarySource(metadata),
                .mOriginalUnits = correction.mOriginalUnits,
                .mCorrectedUnits = correction.mCorrectedUnits,
                .mKind = kind,
                .mImpact = correction.mImpact,
            });
        }

        if (pool.mAdjustmentImpact != ExactRational{})
        {
            aInput.mReconciliation.push_back(
                {.mIsin = pool.mIsin, .mImpact = pool.mAdjustmentImpact});
        }
    }
}

ApplicationDataSources dataSourcesOf(const std::array<CatalogueOrigin, 3>& aReferenceOrigins,
                                     const OfficialRateTable& aRates) {
    constexpr std::array<std::string_view, 3> names{"payers.csv",
                                                    "relief_statements.csv",
                                                    "broker_payers.csv"};
    ApplicationDataSources sources;

    for (std::size_t index = 0; index < names.size(); ++index)
    {
        ReferenceFileSource file{.mFile = std::string{names[index]},
                                 .mOrigin = aReferenceOrigins[index]};

        if (index == 0 && file.mOrigin == CatalogueOrigin::BuiltIn)
        {
            // The revision of the upstream data the built-in payers come from.
            const auto& payers = referenceCatalogue().payers();
            const auto upstream =
                std::find_if(payers.begin(), payers.end(), [](const PayerEntry& aPayer) {
                    return aPayer.mSource.mSource == "ib-edavki";
                });

            if (upstream != payers.end())
            {
                file.mUpstreamRevision = upstream->mSource.mSourceRevision;
            }
        }

        sources.mReferenceFiles.push_back(std::move(file));
    }

    for (const auto& file : aRates.mFiles)
    {
        sources.mRateFiles.push_back({.mFile = file.mSourceFile.value(),
                                      .mYear = file.mYear,
                                      .mOrigin = file.mOrigin,
                                      .mReplacesBuiltIn = file.mReplacesBuiltIn});
    }

    return sources;
}

} // namespace taxbroker
