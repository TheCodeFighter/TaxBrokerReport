#include "processors/corporate_actions.hpp"
#include "taxbroker/exact_arithmetic.hpp"
#include "taxbroker/split_ratio.hpp"
#include "utils/date_utils.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

struct EffectiveDateRange {
    Date mEarliest;
    Date mLatest;
};

/// The dates that give the same position as the row date. An action comes before the trades of
/// its day, so the position on day d is the trades dated before d: no trade of the ISIN may lie
/// from the earlier of d and the row date to the day before the later one. The latest date is
/// also the day before the next action, and at most 31 December.
EffectiveDateRange samePositionDates(const std::vector<TradeInstrument>& aInstruments,
                                     const ReportingYearLedger& aLedger,
                                     std::size_t aInstrumentIndex,
                                     Date aRowDate,
                                     std::optional<Date> aNextActionDate) {
    std::optional<Date> lastTradeBefore;
    std::optional<Date> firstTradeFrom;

    for (const auto& event : aLedger.mCapitalEvents)
    {
        if (event.mKind != StatementEventKind::Trade || event.mInstrumentIndex != aInstrumentIndex)
        {
            continue;
        }

        const auto date = aInstruments.at(aInstrumentIndex)
                              .mTransactions.at(event.mEventIndex)
                              .mMetadata.mTaxDate;

        // The ledger is in date order.
        if (date < aRowDate)
        {
            lastTradeBefore = date;
        }
        else if (!firstTradeFrom)
        {
            firstTradeFrom = date;
        }
    }

    // Open units need a purchase before the action, so a trade before it always exists.
    auto latest = aLedger.mYearEnd;

    if (firstTradeFrom && *firstTradeFrom < latest)
    {
        latest = *firstTradeFrom;
    }

    if (aNextActionDate && *aNextActionDate - DayDuration{1} < latest)
    {
        latest = *aNextActionDate - DayDuration{1};
    }

    return EffectiveDateRange{
        .mEarliest = lastTradeBefore.value() + DayDuration{1},
        .mLatest = latest,
    };
}

/// The ratio question of the action: every source record, the position before it and the dates
/// that keep that position.
DecisionQuestion questionOf(const ResolvedCorporateAction& aAction,
                            Units aPositionBefore,
                            const EffectiveDateRange& aDates,
                            const std::vector<TradeInstrument>& aInstruments) {
    CorporateActionRatioQuestion data{
        .mIsin = aAction.mIsin,
        .mName = aAction.mName,
        .mEffectiveDate = aAction.mEffectiveDate,
        .mPositionBefore = aPositionBefore,
        .mEarliestDate = aDates.mEarliest,
        .mLatestDate = aDates.mLatest,
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

/// Resolves the first action a pool reached: asks its ratio, or applies a recorded answer.
/// aNextActionDate is the date of the ISIN's next action, if it has one. Returns the index of
/// its open ratio request, if it has one.
std::optional<std::size_t> resolveFirstAction(ResolvedCorporateAction& aAction,
                                              const FifoIsinResult& aPool,
                                              const std::vector<TradeInstrument>& aInstruments,
                                              const ReportingYearLedger& aLedger,
                                              std::optional<Date> aNextActionDate,
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

    if (hasSameBrokerTwice(aInstruments, aAction.mEvents))
    {
        aAction.mState = CorporateActionState::SameDayAmbiguous;
        aResult.mDiagnostics.push_back(
            actionDiagnostic(ProcessingDiagnosticCode::CorporateActionAmbiguous,
                             makeDiagnosticMessage(MessageKey::CorporateActionAmbiguousSameDay),
                             aAction,
                             aInstruments));

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

    const Units positionBefore = std::get<std::int64_t>(openUnits);

    // corporate_action_missing_position belongs to the lot step; until then the action is only
    // not applied.
    if (positionBefore == 0)
    {
        aAction.mState = CorporateActionState::NoPosition;
        aResult.mDiagnostics.push_back(
            actionDiagnostic(ProcessingDiagnosticCode::CorporateActionNotApplied,
                             makeDiagnosticMessage(MessageKey::CorporateActionNotApplied),
                             aAction,
                             aInstruments));

        return std::nullopt;
    }

    const auto dates = samePositionDates(aInstruments,
                                         aLedger,
                                         instrumentIndex,
                                         aAction.mEffectiveDate,
                                         aNextActionDate);
    auto question = questionOf(aAction, positionBefore, dates, aInstruments);
    const auto* answer = aDecisions.find(question);

    if (answer == nullptr)
    {
        aAction.mState = CorporateActionState::WaitingForRatio;

        RequestInput request{.mQuestion = std::move(question)};

        request.mProcessingDiagnostics.push_back(aResult.mDiagnostics.size());
        aResult.mDiagnostics.push_back(actionDiagnostic(
            ProcessingDiagnosticCode::CorporateActionAmbiguous,
            makeDiagnosticMessage(MessageKey::CorporateActionAmbiguousRatioRequired),
            aAction,
            aInstruments));
        aResult.mRequests.push_back(std::move(request));

        return aResult.mRequests.size() - 1;
    }

    const auto apply = [&aAction, &aInstruments, &aResult](const auto& aAnswer) {
        using Answer = std::decay_t<decltype(aAnswer)>;

        if constexpr (std::is_same_v<Answer, SplitRatioAnswer>)
        {
            // The session keeps only checked, reduced ratios.
            const auto ratio = makeSplitRatio(aAnswer.mNewShares, aAnswer.mOldShares).value();

            aAction.mState = CorporateActionState::RatioConfirmed;
            aAction.mRatio = ratio;
            aAction.mConfirmedDate = aAnswer.mEffectiveDate;
            aResult.mDiagnostics.push_back(actionDiagnostic(
                ProcessingDiagnosticCode::CorporateActionNotApplied,
                makeDiagnosticMessage(
                    MessageKey::CorporateActionNotAppliedRatioConfirmed,
                    {numberParam("newShares", static_cast<std::uint64_t>(ratio.mNewShares)),
                     numberParam("oldShares", static_cast<std::uint64_t>(ratio.mOldShares))}),
                aAction,
                aInstruments));
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

} // namespace

CorporateActionResult CorporateActionResolver::resolve(const MergedStatement& aStatement,
                                                       const ReportingYearLedger& aLedger,
                                                       const FifoMatchingResult& aFifo,
                                                       RunDecisions& aDecisions) {
    const auto& instruments = aStatement.mPresentation.mTradeInstruments;
    CorporateActionResult result;

    // aFifo.mIsins follows the instruments, so an index is both.
    for (std::size_t index = 0; index < instruments.size(); ++index)
    {
        const auto& pool = aFifo.mIsins.at(index);

        if (pool.mStatus != FifoIsinStatus::CorporateAction ||
            !hasSaleInYear(instruments.at(index), aLedger))
        {
            continue;
        }

        const auto groups = actionsByDate(instruments, aLedger, index);
        std::optional<std::size_t> openRequest;

        // The pool stops at its first action, so the first group is the one it reached.
        for (std::size_t position = 0; position < groups.size(); ++position)
        {
            ResolvedCorporateAction action{
                .mIsin = pool.mIsin,
                .mName = pool.mName,
                .mEffectiveDate =
                    actionOf(instruments, groups[position].front()).mMetadata.mTaxDate,
                .mEvents = groups[position],
            };

            if (position == 0)
            {
                const auto nextActionDate =
                    groups.size() > 1 ? std::optional<Date>{actionOf(instruments, groups[1].front())
                                                                .mMetadata.mTaxDate}
                                      : std::nullopt;

                openRequest = resolveFirstAction(action,
                                                 pool,
                                                 instruments,
                                                 aLedger,
                                                 nextActionDate,
                                                 aDecisions,
                                                 result);
                result.mActions.push_back(std::move(action));
                continue;
            }

            // One action or several is unknown, whatever happens to the earlier action.
            if (hasSameBrokerTwice(instruments, action.mEvents))
            {
                action.mState = CorporateActionState::SameDayAmbiguous;
                result.mDiagnostics.push_back(actionDiagnostic(
                    ProcessingDiagnosticCode::CorporateActionAmbiguous,
                    makeDiagnosticMessage(MessageKey::CorporateActionAmbiguousSameDay),
                    action,
                    instruments));
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

} // namespace taxbroker
