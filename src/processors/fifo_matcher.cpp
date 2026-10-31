#include "processors/fifo_matcher.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace taxbroker {

ProcessingDiagnostic incompleteHistoryDiagnostic(const FifoIsinResult& aPool,
                                                 const TradeTransaction& aSale) {
    auto message = makeDiagnosticMessage(MessageKey::IncompleteHistory);

    return ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::IncompleteHistory,
        .mContext =
            {
                .mStage = PipelineStage::HistoricalValidation,
                .mAffectedReports = {ReportType::CapitalGains},
                .mSources = aSale.mMetadata.mSources,
                .mTaxDate = aSale.mMetadata.mTaxDate,
                .mInstrumentName = aPool.mName,
                .mIsin = aPool.mIsin,
            },
        .mMessage = std::move(message.mEnglish),
        .mMessageKey = message.mKey,
        .mMessageParams = std::move(message.mParams),
    };
}

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

    aDiagnostics.push_back(incompleteHistoryDiagnostic(aPool, aTrade));
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
/// failing sale leaves the lots exactly as they were. The matches of an earlier sale are kept
/// apart from those of the selected year. A sale with too few units stops the pool at
/// aActionAfter when there is one and some units are open, else for missing history.
void applySale(FifoIsinResult& aPool,
               const StatementEventReference& aEvent,
               const TradeTransaction& aTrade,
               bool aInSelectedYear,
               const std::optional<StatementEventReference>& aActionAfter,
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

    // The action may have taken effect before the sale and be booked later; its question
    // decides.
    if (aTrade.mUnits > available && aActionAfter && available > 0)
    {
        stopPool(aPool, FifoIsinStatus::CorporateAction, *aActionAfter);
        aPool.mShortSale = aEvent;

        return;
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
            .mLotChangedByAction = lot.mChangedByAction,
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

    auto& kept = aInSelectedYear ? aPool.mMatches : aPool.mEarlierMatches;

    kept.insert(kept.end(),
                std::make_move_iterator(matches.begin()),
                std::make_move_iterator(matches.end()));
}

/// The confirmed action that has this action row, or nullptr.
const ConfirmedCorporateAction*
confirmedActionOf(const StatementEventReference& aEvent,
                  const std::vector<ConfirmedCorporateAction>& aConfirmedActions) {
    const auto action =
        std::find_if(aConfirmedActions.begin(),
                     aConfirmedActions.end(),
                     [&aEvent](const ConfirmedCorporateAction& aAction) {
                         return std::find(aAction.mEvents.begin(), aAction.mEvents.end(), aEvent) !=
                                aAction.mEvents.end();
                     });

    return action == aConfirmedActions.end() ? nullptr : &*action;
}

/// The first corporate action without a confirmed ratio of each instrument, by instrument index:
/// in the ledger, or else after 31 December in the loss-analysis view. A row after 31 December
/// never stops a pool; a sale with too few units just before it may still ask for it.
std::vector<std::optional<StatementEventReference>>
firstUnconfirmedActions(const std::vector<TradeInstrument>& aInstruments,
                        const ReportingYearLedger& aLedger,
                        const std::vector<ConfirmedCorporateAction>& aConfirmedActions) {
    std::vector<std::optional<StatementEventReference>> firstActions(aInstruments.size());

    const auto take = [&firstActions, &aConfirmedActions](const StatementEventReference& aEvent) {
        auto& firstAction = firstActions.at(aEvent.mInstrumentIndex.value());

        if (aEvent.mKind == StatementEventKind::CorporateAction && !firstAction &&
            confirmedActionOf(aEvent, aConfirmedActions) == nullptr)
        {
            firstAction = aEvent;
        }
    };

    std::for_each(aLedger.mCapitalEvents.begin(), aLedger.mCapitalEvents.end(), take);

    for (const auto& event : aLedger.mLossAnalysisEvents)
    {
        const bool afterYear = event.mKind == StatementEventKind::CorporateAction &&
                               aInstruments.at(event.mInstrumentIndex.value())
                                       .mCorporateActions.at(event.mEventIndex)
                                       .mMetadata.mTaxDate > aLedger.mYearEnd;

        if (afterYear)
        {
            take(event);
        }
    }

    return firstActions;
}

/// Tells whether the instrument has a sale in the selected year: only then does its pool block the
/// capital report.
bool hasSaleInYear(const TradeInstrument& aInstrument, const ReportingYearLedger& aLedger) {
    return std::any_of(aInstrument.mTransactions.begin(),
                       aInstrument.mTransactions.end(),
                       [&aLedger](const TradeTransaction& aTrade) {
                           const auto date = aTrade.mMetadata.mTaxDate;

                           return aTrade.mTradeSide == TradeSide::Sell &&
                                  date >= aLedger.mYearStart && date <= aLedger.mYearEnd;
                       });
}

/// The first unconfirmed action of the ISIN when it is a split row at most kBrokerDateMarginDays
/// after the sale: the split may have taken effect before the sale and been booked later.
std::optional<StatementEventReference>
splitShortlyAfter(const TradeTransaction& aSale,
                  const std::optional<StatementEventReference>& aFirstAction,
                  const TradeInstrument& aInstrument) {
    if (!aFirstAction)
    {
        return std::nullopt;
    }

    const auto& action = aInstrument.mCorporateActions.at(aFirstAction->mEventIndex);
    const auto daysAfter = action.mMetadata.mTaxDate - aSale.mMetadata.mTaxDate;

    if (action.mType == CorporateActionType::Merger ||
        daysAfter > DayDuration{kBrokerDateMarginDays})
    {
        return std::nullopt;
    }

    return aFirstAction;
}

/// The confirmed actions of one pool that are not applied yet, in date order, and the EUR value
/// that the rounding of its applied actions changed.
struct PendingActions {
    std::vector<const ConfirmedCorporateAction*> mActions;
    std::size_t mNext{};
    ExactRational mRoundingImpact;
};

std::vector<PendingActions>
pendingActionsByInstrument(std::size_t aInstrumentCount,
                           const std::vector<ConfirmedCorporateAction>& aConfirmedActions) {
    std::vector<PendingActions> pending(aInstrumentCount);

    for (const auto& action : aConfirmedActions)
    {
        pending.at(action.mEvents.front().mInstrumentIndex.value()).mActions.push_back(&action);
    }

    for (auto& pool : pending)
    {
        std::stable_sort(
            pool.mActions.begin(),
            pool.mActions.end(),
            [](const ConfirmedCorporateAction* aLeft, const ConfirmedCorporateAction* aRight) {
                return aLeft->mEffectiveDate < aRight->mEffectiveDate;
            });
    }

    return pending;
}

/// The context of a capital-gains diagnostic of the action: the sources of all its rows and the
/// date of its first row.
ProcessingContext actionContext(const FifoIsinResult& aPool,
                                const ConfirmedCorporateAction& aAction,
                                const std::vector<TradeInstrument>& aInstruments) {
    ProcessingContext context{
        .mStage = PipelineStage::CorporateAction,
        .mAffectedReports = {ReportType::CapitalGains},
        .mInstrumentName = aPool.mName,
        .mIsin = aPool.mIsin,
    };

    for (const auto& event : aAction.mEvents)
    {
        const auto& metadata = aInstruments.at(event.mInstrumentIndex.value())
                                   .mCorporateActions.at(event.mEventIndex)
                                   .mMetadata;

        context.mSources.insert(context.mSources.end(),
                                metadata.mSources.begin(),
                                metadata.mSources.end());

        if (!context.mTaxDate)
        {
            context.mTaxDate = metadata.mTaxDate;
        }
    }

    return context;
}

/// A capital-gains diagnostic of the action without units or amounts in the message.
ProcessingDiagnostic actionDiagnostic(DiagnosticSeverity aSeverity,
                                      ProcessingDiagnosticCode aCode,
                                      MessageKey aKey,
                                      ProcessingContext aContext) {
    auto message = makeDiagnosticMessage(aKey);

    return ProcessingDiagnostic{
        .mSeverity = aSeverity,
        .mCode = aCode,
        .mContext = std::move(aContext),
        .mMessage = std::move(message.mEnglish),
        .mMessageKey = message.mKey,
        .mMessageParams = std::move(message.mParams),
    };
}

/// The EUR value of the rounding: the rounded-away units times the highest value per unit of the
/// adjusted lots, as a positive amount.
NumericResult<ExactRational> roundingImpact(const ExactRational& aResidualUnits,
                                            const std::vector<FifoLot>& aLots) {
    ExactRational highestValue;

    for (const auto& lot : aLots)
    {
        const auto value = exactLotUnitValue(lot.mState);

        if (const auto* error = std::get_if<NumericError>(&value))
        {
            return *error;
        }

        if (compareExact(std::get<ExactRational>(value), highestValue) > 0)
        {
            highestValue = std::get<ExactRational>(value);
        }
    }

    // The rounded total may be above or below the exact one; the impact counts either way.
    const auto residual = aResidualUnits.numerator() < 0
                              ? subtractExact(ExactRational{}, aResidualUnits)
                              : NumericResult<ExactRational>{aResidualUnits};

    if (const auto* error = std::get_if<NumericError>(&residual))
    {
        return *error;
    }

    return multiplyExact(std::get<ExactRational>(residual), highestValue);
}

/// Applies a confirmed action to every open lot of the pool. Nothing changes unless the whole
/// action can be applied; otherwise the pool stops at the action with an error.
void applyAction(FifoIsinResult& aPool,
                 const ConfirmedCorporateAction& aAction,
                 PendingActions& aPending,
                 const std::vector<TradeInstrument>& aInstruments,
                 std::vector<ProcessingDiagnostic>& aDiagnostics) {
    const auto fail = [&aPool, &aAction, &aDiagnostics](ProcessingDiagnostic aDiagnostic) {
        stopPool(aPool, FifoIsinStatus::NumericFailure, aAction.mEvents.front());
        aDiagnostics.push_back(std::move(aDiagnostic));
    };

    std::vector<UnitAllocationPart> parts;

    for (std::size_t index = 0; index < aPool.mOpenLots.size(); ++index)
    {
        parts.push_back(
            {.mUnits = aPool.mOpenLots[index].mState.mRemainingUnits, .mLotOrder = index});
    }

    const auto allocation =
        allocateAdjustedUnits(parts, aAction.mRatio.mNewShares, aAction.mRatio.mOldShares);

    if (const auto* error = std::get_if<NumericError>(&allocation))
    {
        fail(numericDiagnostic(*error, actionContext(aPool, aAction, aInstruments)));

        return;
    }

    const auto& allocated = std::get<RoundedAllocation>(allocation);
    auto lots = aPool.mOpenLots;

    // Each allocated part is above 0, so every open lot keeps its basis with its new units.
    for (std::size_t index = 0; index < lots.size(); ++index)
    {
        lots[index].mState =
            std::get<LotBasisState>(adjustLotUnits(lots[index].mState, allocated.mParts[index]));
        lots[index].mChangedByAction = true;
    }

    if (allocated.mResidual.numerator() != 0)
    {
        const auto impact = roundingImpact(allocated.mResidual, lots);
        const auto total = std::holds_alternative<ExactRational>(impact)
                               ? addExact(aPending.mRoundingImpact, std::get<ExactRational>(impact))
                               : impact;

        if (const auto* error = std::get_if<NumericError>(&total))
        {
            fail(numericDiagnostic(*error, actionContext(aPool, aAction, aInstruments)));

            return;
        }

        // EUR 0.01 or more in total is not a rounding difference any more.
        constexpr std::int64_t kCentsPerEuro = 100;

        if (compareExact(std::get<ExactRational>(total),
                         std::get<ExactRational>(ExactRational::create(1, kCentsPerEuro))) >= 0)
        {
            fail(actionDiagnostic(DiagnosticSeverity::Error,
                                  ProcessingDiagnosticCode::CorporateActionUnrepresentable,
                                  MessageKey::CorporateActionUnrepresentableRoundingBudget,
                                  actionContext(aPool, aAction, aInstruments)));

            return;
        }

        aPending.mRoundingImpact = std::get<ExactRational>(total);
        aDiagnostics.push_back(
            actionDiagnostic(DiagnosticSeverity::Warning,
                             ProcessingDiagnosticCode::CorporateActionQuantityRounded,
                             MessageKey::CorporateActionQuantityRounded,
                             actionContext(aPool, aAction, aInstruments)));
    }

    aPool.mOpenLots = std::move(lots);
    aPool.mAppliedActions.push_back({.mEvents = aAction.mEvents,
                                     .mEffectiveDate = aAction.mEffectiveDate,
                                     .mRatio = aAction.mRatio});
}

/// Applies the pool's confirmed actions dated on or before aDate, in date order, until one fails.
void applyActionsUpTo(FifoIsinResult& aPool,
                      PendingActions& aPending,
                      Date aDate,
                      const std::vector<TradeInstrument>& aInstruments,
                      std::vector<ProcessingDiagnostic>& aDiagnostics) {
    while (aPool.mStatus == FifoIsinStatus::Matched && aPending.mNext < aPending.mActions.size() &&
           aPending.mActions[aPending.mNext]->mEffectiveDate <= aDate)
    {
        applyAction(aPool,
                    *aPending.mActions[aPending.mNext],
                    aPending,
                    aInstruments,
                    aDiagnostics);
        ++aPending.mNext;
    }
}

} // namespace

bool isPastHoldingPeriod(const FifoMatch& aMatch) {
    constexpr int kFirstYearOfShortPeriod = 2022;
    constexpr int kShortPeriodYears = 15;
    constexpr int kLongPeriodYears = 20;

    const std::chrono::year_month_day purchase{std::chrono::sys_days{aMatch.mPurchaseDate}};
    const std::chrono::year_month_day sale{std::chrono::sys_days{aMatch.mSaleDate}};
    const int periodYears = static_cast<int>(sale.year()) >= kFirstYearOfShortPeriod
                                ? kShortPeriodYears
                                : kLongPeriodYears;

    const auto anniversaryYear = purchase.year() + std::chrono::years{periodYears};
    auto anniversary = anniversaryYear / purchase.month() / purchase.day();

    if (!anniversary.ok())
    {
        anniversary = anniversary.year() / anniversary.month() / std::chrono::day{28};
    }

    return std::chrono::sys_days{aMatch.mSaleDate} > std::chrono::sys_days{anniversary};
}

FifoMatchingResult
FifoMatcher::match(const MergedStatement& aStatement,
                   const ReportingYearLedger& aLedger,
                   const std::vector<ConfirmedCorporateAction>& aConfirmedActions) {
    FifoMatchingResult result;
    const auto& instruments = aStatement.mPresentation.mTradeInstruments;

    result.mIsins.reserve(instruments.size());

    for (const auto& instrument : instruments)
    {
        result.mIsins.push_back({.mIsin = instrument.mIsin, .mName = instrument.mName});
    }

    const auto firstActions = firstUnconfirmedActions(instruments, aLedger, aConfirmedActions);
    auto pending = pendingActionsByInstrument(instruments.size(), aConfirmedActions);
    std::vector<bool> saleInYear;

    std::transform(instruments.begin(),
                   instruments.end(),
                   std::back_inserter(saleInYear),
                   [&aLedger](const TradeInstrument& aInstrument) {
                       return hasSaleInYear(aInstrument, aLedger);
                   });

    // The ledger holds only trades and corporate actions, in merger order, up to 31 December. A
    // confirmed action is applied by its effective date, not at its row.
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
            if (confirmedActionOf(event, aConfirmedActions) != nullptr)
            {
                continue;
            }

            // The position before an unconfirmed action includes the confirmed ones before it.
            const auto date = instruments.at(instrumentIndex)
                                  .mCorporateActions.at(event.mEventIndex)
                                  .mMetadata.mTaxDate;

            applyActionsUpTo(pool,
                             pending.at(instrumentIndex),
                             date,
                             instruments,
                             result.mDiagnostics);

            if (pool.mStatus == FifoIsinStatus::Matched)
            {
                stopPool(pool, FifoIsinStatus::CorporateAction, event);
            }

            continue;
        }

        const auto& trade = instruments.at(instrumentIndex).mTransactions.at(event.mEventIndex);
        const auto& firstAction = firstActions.at(instrumentIndex);

        // A confirmed action comes before the trades of its effective date.
        applyActionsUpTo(pool,
                         pending.at(instrumentIndex),
                         trade.mMetadata.mTaxDate,
                         instruments,
                         result.mDiagnostics);

        if (pool.mStatus != FifoIsinStatus::Matched)
        {
            continue;
        }

        // An action comes before the trades of its day, so a trade on that day is not applied.
        if (firstAction &&
            trade.mMetadata.mTaxDate == instruments.at(instrumentIndex)
                                            .mCorporateActions.at(firstAction->mEventIndex)
                                            .mMetadata.mTaxDate)
        {
            stopPool(pool, FifoIsinStatus::CorporateAction, *firstAction);

            continue;
        }

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
        case TradeSide::Sell: {
            // Only an ISIN with a sale in the year gets the question.
            const auto actionAfter =
                saleInYear[instrumentIndex]
                    ? splitShortlyAfter(trade, firstAction, instruments.at(instrumentIndex))
                    : std::nullopt;

            applySale(pool,
                      event,
                      trade,
                      trade.mMetadata.mTaxDate >= aLedger.mYearStart,
                      actionAfter,
                      result.mDiagnostics);
            break;
        }
        }
    }

    // Confirmed actions after the last trade still change the lots at 31 December. Those dated
    // after it are kept for the loss rule only.
    for (std::size_t index = 0; index < result.mIsins.size(); ++index)
    {
        auto& pool = result.mIsins[index];

        applyActionsUpTo(pool, pending[index], aLedger.mYearEnd, instruments, result.mDiagnostics);

        for (const auto* action : pending[index].mActions)
        {
            if (action->mEffectiveDate > aLedger.mYearEnd)
            {
                pool.mLaterActions.push_back({.mEvents = action->mEvents,
                                              .mEffectiveDate = action->mEffectiveDate,
                                              .mRatio = action->mRatio});
            }
        }
    }

    return result;
}

} // namespace taxbroker
