#include "processors/corporate_actions.hpp"
#include "taxbroker/exact_arithmetic.hpp"
#include "taxbroker/split_ratio.hpp"
#include "utils/date_utils.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace taxbroker {
namespace {

const CorporateAction& actionOf(const std::vector<TradeInstrument>& aInstruments,
                                const StatementEventReference& aEvent) {
    return aInstruments.at(aEvent.mInstrumentIndex.value())
        .mCorporateActions.at(aEvent.mEventIndex);
}

/// The broker of a merged event: every source of one event has the same broker.
Broker brokerOf(const CorporateAction& aAction) {
    return aAction.mMetadata.mSources.empty() ? Broker::Unknown
                                              : aAction.mMetadata.mSources.front().mBroker;
}

/// Tells whether the instrument has a sale in the selected year: only then does a stopped pool
/// block the capital report.
bool hasSaleInYear(const TradeInstrument& aInstrument, const ReportingYearLedger& aLedger) {
    return std::any_of(aInstrument.mTransactions.begin(),
                       aInstrument.mTransactions.end(),
                       [&aLedger](const TradeTransaction& aTrade) {
                           const auto date = aTrade.mMetadata.mTaxDate;

                           return aTrade.mTradeSide == TradeSide::Sell &&
                                  date >= aLedger.mYearStart && date <= aLedger.mYearEnd;
                       });
}

/// Tells whether sales of this class can reach Doh-KDVP: shares, funds, and an unknown class,
/// which the user may still classify as one of them. The capital report gates block the others,
/// so their ratio is not asked (the same rule as the loss stage).
bool mayReachKdvp(AssetClass aAssetClass) {
    switch (aAssetClass)
    {
    case AssetClass::Stock:
    case AssetClass::Fund:
    case AssetClass::Unknown:
        return true;
    case AssetClass::Bond:
    case AssetClass::Derivative:
    case AssetClass::Crypto:
    case AssetClass::PrivateFund:
        return false;
    }

    return false;
}

/// The corporate actions of one ISIN up to 31 December, grouped by date, in ledger order.
std::vector<std::vector<StatementEventReference>>
actionsByDate(const std::vector<TradeInstrument>& aInstruments,
              const ReportingYearLedger& aLedger,
              std::size_t aInstrumentIndex) {
    std::vector<std::vector<StatementEventReference>> groups;

    for (const auto& event : aLedger.mCapitalEvents)
    {
        if (event.mKind != StatementEventKind::CorporateAction ||
            event.mInstrumentIndex != aInstrumentIndex)
        {
            continue;
        }

        // The ledger is in date order, so a new date starts a new group.
        const auto date = actionOf(aInstruments, event).mMetadata.mTaxDate;
        const bool sameDate =
            !groups.empty() &&
            actionOf(aInstruments, groups.back().front()).mMetadata.mTaxDate == date;

        if (sameDate)
        {
            groups.back().push_back(event);
        }
        else
        {
            groups.push_back({event});
        }
    }

    return groups;
}

/// The sources of every event of the action, in event order.
std::vector<SourceReference> sourcesOf(const std::vector<TradeInstrument>& aInstruments,
                                       const ResolvedCorporateAction& aAction) {
    std::vector<SourceReference> sources;

    for (const auto& event : aAction.mEvents)
    {
        const auto& eventSources = actionOf(aInstruments, event).mMetadata.mSources;

        sources.insert(sources.end(), eventSources.begin(), eventSources.end());
    }

    return sources;
}

/// Tells whether one broker has more than one action row in the group.
bool hasSameBrokerTwice(const std::vector<TradeInstrument>& aInstruments,
                        const std::vector<StatementEventReference>& aEvents) {
    std::vector<Broker> brokers;

    for (const auto& event : aEvents)
    {
        const auto broker = brokerOf(actionOf(aInstruments, event));

        if (std::find(brokers.begin(), brokers.end(), broker) != brokers.end())
        {
            return true;
        }

        brokers.push_back(broker);
    }

    return false;
}

/// Tells whether the group has a merger row.
bool hasMerger(const std::vector<TradeInstrument>& aInstruments,
               const std::vector<StatementEventReference>& aEvents) {
    return std::any_of(aEvents.begin(),
                       aEvents.end(),
                       [&aInstruments](const StatementEventReference& aEvent) {
                           return actionOf(aInstruments, aEvent).mType ==
                                  CorporateActionType::Merger;
                       });
}

/// Tells whether another group of the ISIN has a split row of another broker on another date at
/// most kDifferentDatesWindowDays from this group. Split, reverse split and a split of unknown
/// direction are one kind, because two brokers may name one event differently; a merger row is
/// not a split.
bool hasSplitOfAnotherBrokerNearby(const std::vector<TradeInstrument>& aInstruments,
                                   const std::vector<std::vector<StatementEventReference>>& aGroups,
                                   std::size_t aGroup) {
    const auto isSplit = [&aInstruments](const StatementEventReference& aEvent) {
        return actionOf(aInstruments, aEvent).mType != CorporateActionType::Merger;
    };
    const auto date = actionOf(aInstruments, aGroups[aGroup].front()).mMetadata.mTaxDate;

    for (std::size_t other = 0; other < aGroups.size(); ++other)
    {
        const auto otherDate = actionOf(aInstruments, aGroups[other].front()).mMetadata.mTaxDate;
        const auto distance = date < otherDate ? otherDate - date : date - otherDate;

        if (other == aGroup || distance > DayDuration{kDifferentDatesWindowDays})
        {
            continue;
        }

        const auto& otherEvents = aGroups[other];
        const auto pairsWithOtherBroker = [&](const StatementEventReference& aEvent) {
            const auto broker = brokerOf(actionOf(aInstruments, aEvent));

            return isSplit(aEvent) &&
                   std::any_of(otherEvents.begin(),
                               otherEvents.end(),
                               [&](const StatementEventReference& aOtherEvent) {
                                   return isSplit(aOtherEvent) &&
                                          brokerOf(actionOf(aInstruments, aOtherEvent)) != broker;
                               });
        };

        if (std::any_of(aGroups[aGroup].begin(), aGroups[aGroup].end(), pairsWithOtherBroker))
        {
            return true;
        }
    }

    return false;
}

/// The context of a capital-gains diagnostic of the action, with the sources of all its events.
ProcessingContext actionContext(const ResolvedCorporateAction& aAction,
                                const std::vector<TradeInstrument>& aInstruments) {
    return ProcessingContext{
        .mStage = PipelineStage::CorporateAction,
        .mAffectedReports = {ReportType::CapitalGains},
        .mSources = sourcesOf(aInstruments, aAction),
        .mTaxDate = aAction.mEffectiveDate,
        .mInstrumentName = aAction.mName,
        .mIsin = aAction.mIsin,
    };
}

/// A capital-gains error of the action. No units or amounts in the message.
ProcessingDiagnostic actionDiagnostic(ProcessingDiagnosticCode aCode,
                                      DiagnosticMessage aMessage,
                                      const ResolvedCorporateAction& aAction,
                                      const std::vector<TradeInstrument>& aInstruments) {
    return ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = aCode,
        .mContext = actionContext(aAction, aInstruments),
        .mMessage = std::move(aMessage.mEnglish),
        .mMessageKey = aMessage.mKey,
        .mMessageParams = std::move(aMessage.mParams),
    };
}

/// The open units of all lots of the pool, with a checked sum.
NumericResult<Units> openUnitsOf(const FifoIsinResult& aPool) {
    Units total = 0;

    for (const auto& lot : aPool.mOpenLots)
    {
        const auto sum = checkedAdd(total, lot.mState.mRemainingUnits);

        if (!std::holds_alternative<std::int64_t>(sum))
        {
            return sum;
        }

        total = std::get<std::int64_t>(sum);
    }

    return total;
}

/// The signed units of the trades of one ISIN on one date.
struct TradeDay {
    Date mDate;
    /// All trades of the day together.
    Units mNet{};
    /// The lowest running total within the day, in ledger order.
    Units mLowest{};
};

/// The dates the action may have, each with the open units just before the action on that date.
/// The dates run from the day after aPreviousDate (or the first trade) to the day before
/// aNextActionDate (or 31 December). A date with no units, or after a sale of more units than
/// were held, is left out. Each range also keeps the lowest running total of the trades after
/// the action, so a later sale can be checked against it.
std::vector<EffectiveDateRange> dateRanges(const std::vector<TradeInstrument>& aInstruments,
                                           const ReportingYearLedger& aLedger,
                                           std::size_t aInstrumentIndex,
                                           Date aAnchorDate,
                                           Units aAnchorPosition,
                                           std::optional<Date> aPreviousDate,
                                           std::optional<Date> aNextActionDate) {
    // The trades of the ISIN per date, in date order (the ledger is in date order).
    std::vector<TradeDay> days;

    for (const auto& event : aLedger.mCapitalEvents)
    {
        if (event.mKind != StatementEventKind::Trade || event.mInstrumentIndex != aInstrumentIndex)
        {
            continue;
        }

        const auto& trade = aInstruments.at(aInstrumentIndex).mTransactions.at(event.mEventIndex);
        const Units units = trade.mTradeSide == TradeSide::Buy ? trade.mUnits : -trade.mUnits;

        if (aPreviousDate && trade.mMetadata.mTaxDate <= *aPreviousDate)
        {
            continue;
        }

        if (days.empty() || days.back().mDate != trade.mMetadata.mTaxDate)
        {
            days.push_back({.mDate = trade.mMetadata.mTaxDate, .mNet = 0, .mLowest = units});
        }

        days.back().mNet += units;
        days.back().mLowest = std::min(days.back().mLowest, days.back().mNet);
    }

    // Open units need a purchase before the action, so the ISIN has a trade before the anchor
    // date. The next action is in the ledger, so the day before it is always before 31 December.
    const Date earliest = aPreviousDate ? *aPreviousDate + DayDuration{1} : days.front().mDate;
    const Date latest = aNextActionDate ? *aNextActionDate - DayDuration{1} : aLedger.mYearEnd;

    // A trade at most kBrokerDateMarginDays before the next action's row may come after that
    // action, which the broker booked later; the next action's own question checks it.
    const std::optional<Date> checkedUntil =
        aNextActionDate
            ? std::optional<Date>{*aNextActionDate - DayDuration{kBrokerDateMarginDays + 1}}
            : std::nullopt;

    // The lowest running total from the start of day k to the last checked date,
    // and 0 for no trade at all. An action dated up to day k has the trades from day k on after
    // it.
    std::vector<Units> lowestFrom(days.size() + 1, 0);

    for (std::size_t index = days.size(); index-- > 0;)
    {
        lowestFrom[index] =
            days[index].mDate > latest || (checkedUntil && days[index].mDate > *checkedUntil)
                ? 0
                : std::min(
                      {Units{0}, days[index].mLowest, days[index].mNet + lowestFrom[index + 1]});
    }

    // The position at the earliest date: the anchor date's units without the trades in between.
    Units position =
        std::accumulate(days.begin(),
                        days.end(),
                        aAnchorPosition,
                        [aAnchorDate](Units aPosition, const TradeDay& aDay) {
                            return aDay.mDate < aAnchorDate ? aPosition - aDay.mNet : aPosition;
                        });

    std::vector<EffectiveDateRange> ranges;

    // Adds the dates aFrom to aTo with the current position, joined to the previous range when it
    // ends the day before and has the same position and lowest total. A day that sells and buys
    // back the same units keeps the position but not the lowest total, so it stays apart.
    const auto addRange = [&ranges, &position](Date aFrom, Date aTo, Units aLowestChangeAfter) {
        if (aFrom > aTo || position <= 0)
        {
            return;
        }

        if (!ranges.empty() && ranges.back().mPositionBefore == position &&
            ranges.back().mLowestChangeAfter == aLowestChangeAfter &&
            ranges.back().mLatestDate + DayDuration{1} == aFrom)
        {
            ranges.back().mLatestDate = aTo;
            return;
        }

        ranges.push_back({.mEarliestDate = aFrom,
                          .mLatestDate = aTo,
                          .mPositionBefore = position,
                          .mLowestChangeAfter = aLowestChangeAfter});
    };

    Date from = earliest;

    // On a trade's date the action still comes first, so the trade counts from the next day.
    for (std::size_t index = 0; index < days.size() && days[index].mDate <= latest; ++index)
    {
        // The next action comes after this one, so the trades on this one's own date are always
        // between them and must fit, also in the days the next action's question checks.
        if (checkedUntil && days[index].mDate > *checkedUntil)
        {
            addRange(from, days[index].mDate - DayDuration{1}, lowestFrom[index]);
            addRange(days[index].mDate, days[index].mDate, std::min(Units{0}, days[index].mLowest));
        }
        else
        {
            addRange(from, days[index].mDate, lowestFrom[index]);
        }

        // A sale of more units than were held cannot come before the action, also when a purchase
        // later that day makes up for it.
        if (position + days[index].mLowest < 0)
        {
            return ranges;
        }

        position += days[index].mNet;
        from = days[index].mDate + DayDuration{1};
    }

    addRange(from, latest, 0);

    return ranges;
}

/// The ratio question of the action: every source record, the position before it on its row
/// date (or the earlier sale it must come before) and the dates it may have, each with the
/// position before it.
DecisionQuestion questionOf(const ResolvedCorporateAction& aAction,
                            std::optional<Units> aPositionBefore,
                            std::optional<Date> aEarlierSaleDate,
                            std::vector<EffectiveDateRange> aDateRanges,
                            const std::vector<TradeInstrument>& aInstruments) {
    CorporateActionRatioQuestion data{
        .mIsin = aAction.mIsin,
        .mName = aAction.mName,
        .mEffectiveDate = aAction.mEffectiveDate,
        .mPositionBefore = aPositionBefore,
        .mEarlierSaleDate = aEarlierSaleDate,
        .mDateRanges = std::move(aDateRanges),
    };

    for (const auto& event : aAction.mEvents)
    {
        const auto& action = actionOf(aInstruments, event);
        const auto identity = transactionIdentity(action.mMetadata);
        CorporateActionSourceFacts facts{.mBroker = brokerOf(action)};

        if (identity)
        {
            facts.mTransactionId = identity->mTransactionId;
        }

        // Copies of one record share the value, so the first evidence is enough.
        if (!action.mUnitEvidence.empty())
        {
            facts.mSourceValue = action.mUnitEvidence.front().mCanonicalValue;
        }

        data.mSources.push_back(std::move(facts));
    }

    return DecisionQuestion{
        .mType = RequestType::CorporateActionRatioRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mSources = sourcesOf(aInstruments, aAction),
        .mTaxDate = aAction.mEffectiveDate,
        .mIsin = aAction.mIsin,
        .mData = std::move(data),
    };
}

/// The checks that every action of an ISIN needs before its ratio can be asked: a merger is
/// unsupported, and rows that may be one action or several are ambiguous. Returns false after
/// adding the error.
bool checkActionRows(ResolvedCorporateAction& aAction,
                     const std::vector<TradeInstrument>& aInstruments,
                     const std::vector<std::vector<StatementEventReference>>& aGroups,
                     std::size_t aGroup,
                     CorporateActionResult& aResult) {
    if (hasMerger(aInstruments, aAction.mEvents))
    {
        aAction.mState = CorporateActionState::Unsupported;
        aResult.mDiagnostics.push_back(
            actionDiagnostic(ProcessingDiagnosticCode::UnsupportedCorporateAction,
                             makeDiagnosticMessage(MessageKey::UnsupportedCorporateActionMerger),
                             aAction,
                             aInstruments));

        return false;
    }

    if (hasSameBrokerTwice(aInstruments, aAction.mEvents))
    {
        aAction.mState = CorporateActionState::SameDayAmbiguous;
        aResult.mDiagnostics.push_back(
            actionDiagnostic(ProcessingDiagnosticCode::CorporateActionAmbiguous,
                             makeDiagnosticMessage(MessageKey::CorporateActionAmbiguousSameDay),
                             aAction,
                             aInstruments));

        return false;
    }

    if (hasSplitOfAnotherBrokerNearby(aInstruments, aGroups, aGroup))
    {
        aAction.mState = CorporateActionState::DifferentDatesAmbiguous;
        aResult.mDiagnostics.push_back(actionDiagnostic(
            ProcessingDiagnosticCode::CorporateActionAmbiguous,
            makeDiagnosticMessage(
                MessageKey::CorporateActionAmbiguousDifferentDates,
                {numberParam("days", static_cast<std::uint64_t>(kDifferentDatesWindowDays))}),
            aAction,
            aInstruments));

        return false;
    }

    return true;
}

/// The neighbour of the first action a pool stopped at: the effective date of the action applied
/// before it, if any, and the row date of the next action, if any.
struct NeighbourDates {
    std::optional<Date> mPrevious;
    std::optional<Date> mNext;
};

/// Where the date ranges of the first action start from: the row date with the open units, or,
/// for a pool stopped at a sale with too few units just before the row, that sale's date with the
/// units at the start of its day.
struct RangeAnchor {
    Date mDate;
    Units mPosition{};
    /// Set for a sale with too few units.
    std::optional<Date> mEarlierSaleDate;
};

/// The anchor of the pool's first action; aOpenUnits are the pool's open units. The trades of the
/// sale's day before it in the ledger are taken off, because the action comes before the trades of
/// its day.
RangeAnchor rangeAnchor(const ResolvedCorporateAction& aAction,
                        const FifoIsinResult& aPool,
                        const std::vector<TradeInstrument>& aInstruments,
                        const ReportingYearLedger& aLedger,
                        Units aOpenUnits) {
    if (!aPool.mShortSale)
    {
        return RangeAnchor{.mDate = aAction.mEffectiveDate, .mPosition = aOpenUnits};
    }

    const auto instrumentIndex = aPool.mShortSale->mInstrumentIndex.value();
    const auto& transactions = aInstruments.at(instrumentIndex).mTransactions;
    const auto saleDate = transactions.at(aPool.mShortSale->mEventIndex).mMetadata.mTaxDate;
    Units position = aOpenUnits;

    for (const auto& event : aLedger.mCapitalEvents)
    {
        if (event == *aPool.mShortSale)
        {
            break;
        }

        if (event.mKind != StatementEventKind::Trade || event.mInstrumentIndex != instrumentIndex)
        {
            continue;
        }

        const auto& trade = transactions.at(event.mEventIndex);

        if (trade.mMetadata.mTaxDate == saleDate)
        {
            position -= trade.mTradeSide == TradeSide::Buy ? trade.mUnits : -trade.mUnits;
        }
    }

    return RangeAnchor{.mDate = saleDate, .mPosition = position, .mEarlierSaleDate = saleDate};
}

/// Resolves the first action a pool stopped at: asks its ratio, or takes a recorded answer.
/// Returns the index of its open ratio request, if it has one.
std::optional<std::size_t>
resolveFirstAction(ResolvedCorporateAction& aAction,
                   const FifoIsinResult& aPool,
                   const std::vector<TradeInstrument>& aInstruments,
                   const ReportingYearLedger& aLedger,
                   const std::vector<std::vector<StatementEventReference>>& aGroups,
                   std::size_t aGroup,
                   NeighbourDates aNeighbours,
                   RunDecisions& aDecisions,
                   CorporateActionResult& aResult) {
    const auto instrumentIndex = aAction.mEvents.front().mInstrumentIndex.value();
    const auto& instrument = aInstruments.at(instrumentIndex);

    if (!mayReachKdvp(instrument.mAssetClass))
    {
        aAction.mState = CorporateActionState::NotAskedForClass;
        aResult.mDiagnostics.push_back(
            actionDiagnostic(ProcessingDiagnosticCode::CorporateActionNotApplied,
                             makeDiagnosticMessage(MessageKey::CorporateActionNotApplied),
                             aAction,
                             aInstruments));

        return std::nullopt;
    }

    if (!checkActionRows(aAction, aInstruments, aGroups, aGroup, aResult))
    {
        return std::nullopt;
    }

    const auto openUnits = openUnitsOf(aPool);

    if (!std::holds_alternative<std::int64_t>(openUnits))
    {
        aAction.mState = CorporateActionState::NumericFailure;
        aResult.mDiagnostics.push_back(numericDiagnostic(std::get<NumericError>(openUnits),
                                                         actionContext(aAction, aInstruments)));

        return std::nullopt;
    }

    const auto anchor =
        rangeAnchor(aAction, aPool, aInstruments, aLedger, std::get<std::int64_t>(openUnits));
    auto ranges = dateRanges(aInstruments,
                             aLedger,
                             instrumentIndex,
                             anchor.mDate,
                             anchor.mPosition,
                             aNeighbours.mPrevious,
                             aNeighbours.mNext);

    // A broker row for an action while the files show no units points to missing history. Before
    // a sale with too few units the same holds when no date before it has units.
    if (anchor.mPosition == 0 || ranges.empty())
    {
        aAction.mState = CorporateActionState::NoPosition;
        aResult.mDiagnostics.push_back(
            actionDiagnostic(ProcessingDiagnosticCode::CorporateActionMissingPosition,
                             makeDiagnosticMessage(MessageKey::CorporateActionMissingPosition),
                             aAction,
                             aInstruments));

        return std::nullopt;
    }

    const auto positionBefore =
        anchor.mEarlierSaleDate ? std::nullopt : std::optional<Units>{anchor.mPosition};
    auto question = questionOf(aAction,
                               positionBefore,
                               anchor.mEarlierSaleDate,
                               std::move(ranges),
                               aInstruments);
    const auto* answer = aDecisions.find(question);

    if (answer == nullptr)
    {
        aAction.mState = CorporateActionState::WaitingForRatio;

        RequestInput request{.mQuestion = std::move(question)};
        auto message =
            anchor.mEarlierSaleDate
                ? makeDiagnosticMessage(
                      MessageKey::CorporateActionAmbiguousRatioRequiredBeforeSale,
                      {textParam("saleDate", formatCalendarDate(*anchor.mEarlierSaleDate)),
                       numberParam("days", static_cast<std::uint64_t>(kBrokerDateMarginDays))})
                : makeDiagnosticMessage(MessageKey::CorporateActionAmbiguousRatioRequired);

        request.mProcessingDiagnostics.push_back(aResult.mDiagnostics.size());
        aResult.mDiagnostics.push_back(
            actionDiagnostic(ProcessingDiagnosticCode::CorporateActionAmbiguous,
                             std::move(message),
                             aAction,
                             aInstruments));
        aResult.mRequests.push_back(std::move(request));

        return aResult.mRequests.size() - 1;
    }

    const auto apply = [&aAction, &aInstruments, &aResult](const auto& aAnswer) {
        using Answer = std::decay_t<decltype(aAnswer)>;

        if constexpr (std::is_same_v<Answer, SplitRatioAnswer>)
        {
            // The session keeps only checked, reduced ratios. matchCapitalPools applies it in the
            // next calculation.
            aAction.mState = CorporateActionState::RatioConfirmed;
            aAction.mRatio = makeSplitRatio(aAnswer.mNewShares, aAnswer.mOldShares).value();
            aAction.mConfirmedDate = aAnswer.mEffectiveDate;
        }
        else if constexpr (std::is_same_v<Answer, CancelAnswer>)
        {
            aAction.mState = CorporateActionState::Cancelled;
            aResult.mDiagnostics.push_back(actionDiagnostic(
                ProcessingDiagnosticCode::CorporateActionAmbiguous,
                makeDiagnosticMessage(MessageKey::CorporateActionAmbiguousCancelled),
                aAction,
                aInstruments));
        }
        else
        {
            static_assert(std::is_same_v<Answer, FailureResolutionAnswer> ||
                          std::is_same_v<Answer, NoOtherActivityAnswer> ||
                          std::is_same_v<Answer, OtherActivityAnswer>);
            // The session accepts only a ratio or a cancel for a ratio question.
            throw std::logic_error("Only a ratio or a cancel can answer a ratio question.");
        }
    };

    std::visit(apply, *answer);

    return std::nullopt;
}

/// The applied action of the pool that starts with this action row, if there is one.
const AppliedCorporateAction* appliedActionOf(const FifoIsinResult& aPool,
                                              const StatementEventReference& aEvent) {
    const auto applied = std::find_if(
        aPool.mAppliedActions.begin(),
        aPool.mAppliedActions.end(),
        [&aEvent](const AppliedCorporateAction& aApplied) { return aApplied.mAction == aEvent; });

    return applied == aPool.mAppliedActions.end() ? nullptr : &*applied;
}

/// The confirmed action that starts with this action row; the caller knows it exists.
const ConfirmedCorporateAction&
confirmedActionOf(const std::vector<ConfirmedCorporateAction>& aConfirmedActions,
                  const StatementEventReference& aEvent) {
    return *std::find_if(aConfirmedActions.begin(),
                         aConfirmedActions.end(),
                         [&aEvent](const ConfirmedCorporateAction& aAction) {
                             return aAction.mEvents.front() == aEvent;
                         });
}

/// Marks an action that the pool applied and adds the warning that the user supplied its ratio.
void markApplied(ResolvedCorporateAction& aAction,
                 const AppliedCorporateAction& aApplied,
                 const std::vector<ConfirmedCorporateAction>& aConfirmedActions,
                 const std::vector<TradeInstrument>& aInstruments,
                 CorporateActionResult& aResult) {
    const auto& confirmed = confirmedActionOf(aConfirmedActions, aApplied.mAction);

    aAction.mState = CorporateActionState::Applied;
    aAction.mRatio = confirmed.mRatio;
    aAction.mConfirmedDate = aApplied.mEffectiveDate;

    // A warning, so the result shows beside the download that the broker did not give the ratio.
    auto diagnostic = actionDiagnostic(
        ProcessingDiagnosticCode::CorporateActionApplied,
        makeDiagnosticMessage(
            MessageKey::CorporateActionAppliedUserRatio,
            {numberParam("newShares", static_cast<std::uint64_t>(confirmed.mRatio.mNewShares)),
             numberParam("oldShares", static_cast<std::uint64_t>(confirmed.mRatio.mOldShares)),
             textParam("effectiveDate", formatCalendarDate(aApplied.mEffectiveDate))}),
        aAction,
        aInstruments);

    diagnostic.mSeverity = DiagnosticSeverity::Warning;
    aResult.mDiagnostics.push_back(std::move(diagnostic));
}

} // namespace

CorporateActionResult
CorporateActionResolver::resolve(const MergedStatement& aStatement,
                                 const ReportingYearLedger& aLedger,
                                 const FifoMatchingResult& aFifo,
                                 const std::vector<ConfirmedCorporateAction>& aConfirmedActions,
                                 RunDecisions& aDecisions) {
    const auto& instruments = aStatement.mPresentation.mTradeInstruments;
    CorporateActionResult result;

    // aFifo.mIsins follows the instruments, so an index is both.
    for (std::size_t index = 0; index < instruments.size(); ++index)
    {
        const auto& pool = aFifo.mIsins.at(index);

        if (!hasSaleInYear(instruments.at(index), aLedger))
        {
            continue;
        }

        const auto groups = actionsByDate(instruments, aLedger, index);
        std::optional<std::size_t> openRequest;
        std::optional<Date> previousDate;
        bool reachedFirst = false;

        for (std::size_t position = 0; position < groups.size(); ++position)
        {
            ResolvedCorporateAction action{
                .mIsin = pool.mIsin,
                .mName = pool.mName,
                .mEffectiveDate =
                    actionOf(instruments, groups[position].front()).mMetadata.mTaxDate,
                .mEvents = groups[position],
            };

            if (!reachedFirst)
            {
                if (const auto* applied = appliedActionOf(pool, groups[position].front()))
                {
                    markApplied(action, *applied, aConfirmedActions, instruments, result);
                    previousDate = applied->mEffectiveDate;
                    result.mActions.push_back(std::move(action));
                    continue;
                }

                // The pool stopped earlier for another reason (missing history, a non-EUR trade,
                // an action that could not be applied), which has its own error.
                if (pool.mStatus != FifoIsinStatus::CorporateAction)
                {
                    break;
                }

                const auto nextActionDate =
                    position + 1 < groups.size()
                        ? std::optional<Date>{actionOf(instruments, groups[position + 1].front())
                                                  .mMetadata.mTaxDate}
                        : std::nullopt;

                reachedFirst = true;
                openRequest =
                    resolveFirstAction(action,
                                       pool,
                                       instruments,
                                       aLedger,
                                       groups,
                                       position,
                                       {.mPrevious = previousDate, .mNext = nextActionDate},
                                       aDecisions,
                                       result);
                result.mActions.push_back(std::move(action));
                continue;
            }

            // One action or several is unknown, or a merger, whatever happens to the earlier one.
            if (!checkActionRows(action, instruments, groups, position, result))
            {
                result.mActions.push_back(std::move(action));
                continue;
            }

            // The position before this action depends on the earlier one, so it waits. While the
            // earlier question is open, answering it is the way forward.
            action.mState = CorporateActionState::WaitingForEarlierAction;

            const auto earlierDate = result.mActions.back().mEffectiveDate;

            if (openRequest)
            {
                result.mRequests.at(*openRequest)
                    .mProcessingDiagnostics.push_back(result.mDiagnostics.size());
            }

            result.mDiagnostics.push_back(actionDiagnostic(
                ProcessingDiagnosticCode::CorporateActionNotApplied,
                makeDiagnosticMessage(MessageKey::CorporateActionNotAppliedWaitingForEarlierAction,
                                      {textParam("earlierDate", formatCalendarDate(earlierDate))}),
                action,
                instruments));
            result.mActions.push_back(std::move(action));
        }
    }

    return result;
}

CapitalPools matchCapitalPools(const MergedStatement& aStatement,
                               const ReportingYearLedger& aLedger,
                               RunDecisions& aDecisions) {
    const auto& instruments = aStatement.mPresentation.mTradeInstruments;
    const std::size_t actionRows =
        std::accumulate(instruments.begin(),
                        instruments.end(),
                        std::size_t{0},
                        [](std::size_t aCount, const TradeInstrument& aInstrument) {
                            return aCount + aInstrument.mCorporateActions.size();
                        });
    std::vector<ConfirmedCorporateAction> confirmed;

    // A round that confirms nothing new returns, and an action is confirmed once, so one round
    // per action row and a last one are enough.
    for (std::size_t round = 0; round <= actionRows; ++round)
    {
        auto fifo = FifoMatcher::match(aStatement, aLedger, confirmed);
        auto actions =
            CorporateActionResolver::resolve(aStatement, aLedger, fifo, confirmed, aDecisions);
        const auto confirmedBefore = confirmed.size();

        for (const auto& action : actions.mActions)
        {
            const bool alreadyConfirmed =
                std::any_of(confirmed.begin(),
                            confirmed.end(),
                            [&action](const ConfirmedCorporateAction& aConfirmed) {
                                return aConfirmed.mEvents == action.mEvents;
                            });

            if (action.mState == CorporateActionState::RatioConfirmed && !alreadyConfirmed)
            {
                confirmed.push_back({
                    .mEvents = action.mEvents,
                    .mRatio = action.mRatio.value(),
                    .mEffectiveDate = action.mConfirmedDate.value(),
                });
            }
        }

        if (confirmed.size() == confirmedBefore)
        {
            return CapitalPools{.mFifo = std::move(fifo), .mActions = std::move(actions)};
        }
    }

    throw std::logic_error("The corporate actions were still confirmed after one round per row.");
}

} // namespace taxbroker
