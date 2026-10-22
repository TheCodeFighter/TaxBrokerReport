#include "processors/fifo_matcher.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>
#include <variant>

namespace taxbroker {
namespace {

/// Marks a pool as stopped at an event. Its later events are ignored.
void stopPool(FifoIsinResult& aPool,
              FifoIsinStatus aStatus,
              const StatementEventReference& aEvent) {
    aPool.mStatus = aStatus;
    aPool.mStoppedAt = aEvent;
}

/// Stops a pool at a numeric failure of a trade and records the diagnostic for the capital report.
void failPool(FifoIsinResult& aPool,
              const StatementEventReference& aEvent,
              const TradeTransaction& aTrade,
              NumericError aError,
              std::vector<ProcessingDiagnostic>& aDiagnostics) {
    stopPool(aPool, FifoIsinStatus::NumericFailure, aEvent);

    aDiagnostics.push_back(numericDiagnostic(aError,
                                             ProcessingContext{
                                                 .mStage = PipelineStage::FifoTax,
                                                 .mAffectedReports = {ReportType::CapitalGains},
                                                 .mSources = aTrade.mMetadata.mSources,
                                                 .mTaxDate = aTrade.mMetadata.mTaxDate,
                                                 .mInstrumentName = aPool.mName,
                                                 .mIsin = aPool.mIsin,
                                             }));
}

/// Stops a pool at a sale that needs more units than the earlier purchases left, and records the
/// incomplete-history error for the capital report. The units are not part of the message.
void stopForMissingHistory(FifoIsinResult& aPool,
                           const StatementEventReference& aEvent,
                           const TradeTransaction& aTrade,
                           Units aAvailableUnits,
                           std::vector<ProcessingDiagnostic>& aDiagnostics) {
    stopPool(aPool, FifoIsinStatus::InsufficientUnits, aEvent);
    aPool.mAvailableUnits = aAvailableUnits;

    auto message = makeDiagnosticMessage(MessageKey::IncompleteHistory);
    ProcessingDiagnostic diagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::IncompleteHistory,
        .mContext =
            {
                .mStage = PipelineStage::HistoricalValidation,
                .mAffectedReports = {ReportType::CapitalGains},
                .mSources = aTrade.mMetadata.mSources,
                .mTaxDate = aTrade.mMetadata.mTaxDate,
                .mInstrumentName = aPool.mName,
                .mIsin = aPool.mIsin,
            },
        .mMessage = std::move(message.mEnglish),
        .mMessageKey = message.mKey,
        .mMessageParams = std::move(message.mParams),
    };

    aDiagnostics.push_back(std::move(diagnostic));
}

/// Adds a lot with the exact basis of a purchase at the end of the FIFO queue.
void applyPurchase(FifoIsinResult& aPool,
                   const StatementEventReference& aEvent,
                   const TradeTransaction& aTrade,
                   std::vector<ProcessingDiagnostic>& aDiagnostics) {
    const auto basis = createLotBasis(aTrade.mUnitPrice, aTrade.mUnits);

    if (const auto* error = std::get_if<NumericError>(&basis))
    {
        failPool(aPool, aEvent, aTrade, *error, aDiagnostics);

        return;
    }

    aPool.mOpenLots.push_back({aEvent, aTrade.mMetadata.mTaxDate, std::get<LotBasisState>(basis)});
}

/// Matches a sale to the oldest open lots. Nothing changes unless the whole sale is matched, so a
/// failing sale leaves the lots exactly as they were. Matches are kept only for a selected-year
/// sale; an earlier sale only uses up lots.
void applySale(FifoIsinResult& aPool,
               const StatementEventReference& aEvent,
               const TradeTransaction& aTrade,
               bool aInSelectedYear,
               std::vector<ProcessingDiagnostic>& aDiagnostics) {
    if (aTrade.mUnits <= 0 || aTrade.mUnitPrice <= 0)
    {
        failPool(aPool, aEvent, aTrade, NumericError::UnrepresentableValue, aDiagnostics);

        return;
    }

    Units available = 0;

    for (const auto& lot : aPool.mOpenLots)
    {
        const auto total = checkedAdd(available, lot.mState.mRemainingUnits);

        if (const auto* error = std::get_if<NumericError>(&total))
        {
            failPool(aPool, aEvent, aTrade, *error, aDiagnostics);

            return;
        }

        available = std::get<std::int64_t>(total);
    }

    if (aTrade.mUnits > available)
    {
        stopForMissingHistory(aPool, aEvent, aTrade, available, aDiagnostics);

        return;
    }

    // Work on copies and commit them only after the whole sale is matched.
    auto lots = aPool.mOpenLots;
    std::vector<FifoMatch> matches;
    std::size_t firstOpen = 0;
    Units unmatched = aTrade.mUnits;

    // The check above guarantees enough open units, so firstOpen stays inside lots.
    while (unmatched > 0)
    {
        auto& lot = lots[firstOpen];
        const Units matchedUnits = std::min(unmatched, lot.mState.mRemainingUnits);
        const auto consumption = consumeLotBasis(lot.mState, matchedUnits);

        if (const auto* error = std::get_if<NumericError>(&consumption))
        {
            failPool(aPool, aEvent, aTrade, *error, aDiagnostics);

            return;
        }

        const auto& consumed = std::get<BasisConsumption>(consumption);

        // UnitPrice and Units both have eight decimals, so the raw product is exact 10^-16 EUR.
        const ExactRational saleValue{WideInteger{aTrade.mUnitPrice} * matchedUnits};
        const auto gain = subtractExact(saleValue, consumed.mAllocatedBasis.mTicks);

        if (const auto* error = std::get_if<NumericError>(&gain))
        {
            failPool(aPool, aEvent, aTrade, *error, aDiagnostics);

            return;
        }

        matches.push_back({
            .mSale = aEvent,
            .mPurchase = lot.mPurchase,
            .mSaleDate = aTrade.mMetadata.mTaxDate,
            .mPurchaseDate = lot.mPurchaseDate,
            .mUnits = matchedUnits,
            .mBasis = consumed.mAllocatedBasis,
            .mSaleValueTicks = saleValue,
            .mGainTicks = std::get<ExactRational>(gain),
        });

        lot.mState = consumed.mRemaining;
        unmatched -= matchedUnits;

        if (lot.mState.mRemainingUnits == 0)
        {
            ++firstOpen;
        }
    }

    lots.erase(lots.begin(), lots.begin() + static_cast<std::ptrdiff_t>(firstOpen));
    aPool.mOpenLots = std::move(lots);

    if (aInSelectedYear)
    {
        aPool.mMatches.insert(aPool.mMatches.end(),
                              std::make_move_iterator(matches.begin()),
                              std::make_move_iterator(matches.end()));
    }
}

} // namespace

FifoMatchingResult FifoMatcher::match(const MergedStatement& aStatement,
                                      const ReportingYearLedger& aLedger) const {
    FifoMatchingResult result;
    const auto& instruments = aStatement.mPresentation.mTradeInstruments;

    result.mIsins.reserve(instruments.size());

    for (const auto& instrument : instruments)
    {
        result.mIsins.push_back({.mIsin = instrument.mIsin, .mName = instrument.mName});
    }

    // The ledger holds only trades and corporate actions, in merger order, up to 31 December.
    for (const auto& event : aLedger.mCapitalEvents)
    {
        const auto instrumentIndex = event.mInstrumentIndex.value();
        auto& pool = result.mIsins.at(instrumentIndex);

        if (pool.mStatus != FifoIsinStatus::Matched)
        {
            continue;
        }

        if (event.mKind == StatementEventKind::CorporateAction)
        {
            stopPool(pool, FifoIsinStatus::CorporateAction, event);

            continue;
        }

        const auto& trade = instruments.at(instrumentIndex).mTransactions.at(event.mEventIndex);

        if (trade.mCurrency != Currency::EUR)
        {
            stopPool(pool, FifoIsinStatus::NonEurTrade, event);

            continue;
        }

        switch (trade.mTradeSide)
        {
        case TradeSide::Buy:
            applyPurchase(pool, event, trade, result.mDiagnostics);
            break;
        case TradeSide::Sell:
            applySale(pool,
                      event,
                      trade,
                      trade.mMetadata.mTaxDate >= aLedger.mYearStart,
                      result.mDiagnostics);
            break;
        }
    }

    return result;
}

} // namespace taxbroker
