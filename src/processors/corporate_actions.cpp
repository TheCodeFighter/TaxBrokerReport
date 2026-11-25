#include "processors/corporate_actions.hpp"
#include "taxbroker/exact_arithmetic.hpp"
#include "taxbroker/split_ratio.hpp"
#include "utils/date_utils.hpp"

#include <algorithm>
#include <chrono>
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
/// which the user may still classify as one of them. The capital report gates block the others
/// with a class error, so their actions are not asked (the same rule as the loss stage).
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

/// The corporate actions of one ISIN up to 31 December, then those after it in the loss-analysis
/// view, grouped by date, in ledger order.
std::vector<std::vector<StatementEventReference>>
actionsByDate(const std::vector<TradeInstrument>& aInstruments,
              const ReportingYearLedger& aLedger,
              std::size_t aInstrumentIndex) {
    std::vector<std::vector<StatementEventReference>> groups;

    const auto add = [&](const StatementEventReference& aEvent, bool aAfterYear) {
        if (aEvent.mKind != StatementEventKind::CorporateAction ||
            aEvent.mInstrumentIndex != aInstrumentIndex)
        {
            return;
        }

        const auto& action = actionOf(aInstruments, aEvent);

        const auto date = action.mMetadata.mTaxDate;

        if (aAfterYear != (date > aLedger.mYearEnd))
        {
            return;
        }

        // Both views are in date order, so a new date starts a new group.
        const bool sameDate =
            !groups.empty() &&
            actionOf(aInstruments, groups.back().front()).mMetadata.mTaxDate == date;

        if (sameDate)
        {
            groups.back().push_back(aEvent);
        }
        else
        {
            groups.push_back({aEvent});
        }
    };

    for (const auto& event : aLedger.mCapitalEvents)
    {
        add(event, false);
    }

    for (const auto& event : aLedger.mLossAnalysisEvents)
    {
        add(event, true);
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

/// Tells whether one broker has more than one action in the group. Rows of one broker with the
/// same broker action ID are one action; a row without an ID is an action of its own.
bool hasSameBrokerTwice(const std::vector<TradeInstrument>& aInstruments,
                        const std::vector<StatementEventReference>& aEvents) {
    std::vector<std::pair<Broker, std::optional<std::string>>> seen;

    for (const auto& event : aEvents)
    {
        const auto& action = actionOf(aInstruments, event);
        const auto broker = brokerOf(action);
        const auto& actionId = action.mBrokerActionId;
        const bool isOtherAction = std::any_of(seen.begin(), seen.end(), [&](const auto& aSeen) {
            return aSeen.first == broker && !(actionId && aSeen.second == actionId);
        });

        if (isOtherAction)
        {
            return true;
        }

        seen.emplace_back(broker, actionId);
    }

    return false;
}

/// Tells whether the group has a row that is not a split: a merger of any kind, or an ISIN change
/// that the ISIN change stage has not carried out.
bool hasMerger(const std::vector<TradeInstrument>& aInstruments,
               const std::vector<StatementEventReference>& aEvents) {
    return std::any_of(aEvents.begin(),
                       aEvents.end(),
                       [&aInstruments](const StatementEventReference& aEvent) {
                           return isNonSplitActionType(actionOf(aInstruments, aEvent).mType);
                       });
}

/// The other groups of the ISIN with a split row of another broker on another date at most
/// kDifferentDatesWindowDays from a split row of this group. Split, reverse split and a split of
/// unknown direction are one kind, because two brokers may name one event differently; a merger
/// row is not a split.
std::vector<std::size_t>
splitsOfAnotherBrokerNearby(const std::vector<TradeInstrument>& aInstruments,
                            const std::vector<std::vector<StatementEventReference>>& aGroups,
                            std::size_t aGroup) {
    const auto pairs = [&aInstruments](const StatementEventReference& aEvent,
                                       const StatementEventReference& aOtherEvent) {
        const auto& action = actionOf(aInstruments, aEvent);
        const auto& other = actionOf(aInstruments, aOtherEvent);
        const auto date = action.mMetadata.mTaxDate;
        const auto otherDate = other.mMetadata.mTaxDate;
        const auto distance = date < otherDate ? otherDate - date : date - otherDate;

        return !isNonSplitActionType(action.mType) && !isNonSplitActionType(other.mType) &&
               brokerOf(action) != brokerOf(other) && distance > DayDuration{0} &&
               distance <= DayDuration{kDifferentDatesWindowDays};
    };
    std::vector<std::size_t> nearby;

    for (std::size_t other = 0; other < aGroups.size(); ++other)
    {
        const bool paired =
            other != aGroup &&
            std::any_of(aGroups[aGroup].begin(),
                        aGroups[aGroup].end(),
                        [&](const StatementEventReference& aEvent) {
                            return std::any_of(aGroups[other].begin(),
                                               aGroups[other].end(),
                                               [&](const StatementEventReference& aOtherEvent) {
                                                   return pairs(aEvent, aOtherEvent);
                                               });
                        });

        if (paired)
        {
            nearby.push_back(other);
        }
    }

    return nearby;
}

/// The action rows of one ISIN that the resolver handles as one step.
struct ActionGroup {
    std::vector<StatementEventReference> mEvents;
    /// Rows of two brokers on two dates that may be one split: the ratio question asks whether
    /// they are.
    bool mAskSameEvent{};
    /// Rows that may be one action or several, all of them in mEvents: the identity question
    /// asks which rows belong together.
    bool mNeedsIdentity{};
    /// The user grouped the rows into actions that start on the same day, so their order is
    /// unknown.
    bool mOrderUnknown{};
};

/// The steps of one ISIN in date order. Each date is one step, with these exceptions: two dates
/// that may be one split booked by two brokers on their own dates are one step when each is the
/// other's only date with a split row of another broker nearby, neither has a merger row and no
/// broker has two actions (the question asks whether they are one split); the dates linked by
/// nearby rows of other brokers, or with one broker's rows on one day that are not one action, are
/// one step for the identity question. A merger row is never part of such a step.
std::vector<ActionGroup>
actionGroupsOf(const std::vector<TradeInstrument>& aInstruments,
               const std::vector<std::vector<StatementEventReference>>& aDateGroups) {
    std::vector<std::vector<std::size_t>> nearby;

    nearby.reserve(aDateGroups.size());

    for (std::size_t index = 0; index < aDateGroups.size(); ++index)
    {
        nearby.push_back(splitsOfAnotherBrokerNearby(aInstruments, aDateGroups, index));
    }

    std::vector<bool> taken(aDateGroups.size(), false);
    std::vector<ActionGroup> groups;

    for (std::size_t index = 0; index < aDateGroups.size(); ++index)
    {
        if (taken[index])
        {
            continue;
        }

        taken[index] = true;

        const auto next = index + 1;
        const bool mayBeOneSplit = next < aDateGroups.size() &&
                                   nearby[index] == std::vector<std::size_t>{next} &&
                                   nearby[next] == std::vector<std::size_t>{index};

        if (mayBeOneSplit)
        {
            auto rows = aDateGroups[index];

            rows.insert(rows.end(), aDateGroups[next].begin(), aDateGroups[next].end());

            if (!hasMerger(aInstruments, rows) && !hasSameBrokerTwice(aInstruments, rows))
            {
                taken[next] = true;
                groups.push_back({.mEvents = std::move(rows), .mAskSameEvent = true});
                continue;
            }
        }

        const bool needsIdentity =
            !hasMerger(aInstruments, aDateGroups[index]) &&
            (hasSameBrokerTwice(aInstruments, aDateGroups[index]) || !nearby[index].empty());
        ActionGroup group{.mEvents = aDateGroups[index], .mNeedsIdentity = needsIdentity};

        if (needsIdentity)
        {
            // The dates linked by nearby rows, which may be one action or several.
            std::vector<std::size_t> cluster{index};

            for (std::size_t at = 0; at < cluster.size(); ++at)
            {
                for (const auto other : nearby[cluster[at]])
                {
                    if (!taken[other] && !hasMerger(aInstruments, aDateGroups[other]))
                    {
                        taken[other] = true;
                        cluster.push_back(other);
                    }
                }
            }

            std::sort(cluster.begin(), cluster.end());
            group.mEvents.clear();

            for (const auto member : cluster)
            {
                group.mEvents.insert(group.mEvents.end(),
                                     aDateGroups[member].begin(),
                                     aDateGroups[member].end());
            }

            // A merger next to a lone row leaves one row, which has nothing to group.
            group.mNeedsIdentity = group.mEvents.size() > 1;
        }

        groups.push_back(std::move(group));
    }

    return groups;
}

/// One row of an identity question with the event it was made from.
struct IdentityRow {
    CorporateActionIdentityRow mFacts;
    StatementEventReference mEvent;
};

/// The rows of an identity question in the order of the question.
std::vector<IdentityRow> identityRowsOf(const std::vector<TradeInstrument>& aInstruments,
                                        const std::vector<StatementEventReference>& aEvents) {
    std::vector<IdentityRow> rows;

    rows.reserve(aEvents.size());

    for (const auto& event : aEvents)
    {
        const auto& action = actionOf(aInstruments, event);
        const auto identity = transactionIdentity(action.mMetadata);
        IdentityRow row{.mFacts = {.mBroker = brokerOf(action),
                                   .mDate = action.mMetadata.mTaxDate,
                                   .mType = action.mType,
                                   .mBrokerActionId = action.mBrokerActionId},
                        .mEvent = event};

        if (identity)
        {
            row.mFacts.mTransactionId = identity->mTransactionId;
        }

        // Copies of one record share the value, so the first evidence is enough.
        if (!action.mUnitEvidence.empty())
        {
            row.mFacts.mSourceValue = action.mUnitEvidence.front().mCanonicalValue;
        }

        rows.push_back(std::move(row));
    }

    std::stable_sort(rows.begin(),
                     rows.end(),
                     [](const IdentityRow& aLeft, const IdentityRow& aRight) {
                         return aLeft.mFacts.sortKey() < aRight.mFacts.sortKey();
                     });

    return rows;
}

/// The actions of an answered identity question, in date order: the rows with the same number
/// are one action, which starts on its earliest row. Actions that start on the same day have no
/// known order.
std::vector<ActionGroup> actionsOfGrouping(const std::vector<IdentityRow>& aRows,
                                           const CorporateActionGroupingAnswer& aAnswer) {
    const auto count = *std::max_element(aAnswer.mActionOfRow.begin(), aAnswer.mActionOfRow.end());
    std::vector<std::vector<std::size_t>> rowsOfAction(count);

    for (std::size_t row = 0; row < aRows.size(); ++row)
    {
        rowsOfAction.at(aAnswer.mActionOfRow.at(row) - 1).push_back(row);
    }

    // The rows are in date order, so the first row of an action gives its place.
    std::sort(rowsOfAction.begin(), rowsOfAction.end(), [](const auto& aLeft, const auto& aRight) {
        return aLeft.front() < aRight.front();
    });

    const auto dateOf = [&aRows, &rowsOfAction](std::size_t aAction) {
        return aRows[rowsOfAction[aAction].front()].mFacts.mDate;
    };
    std::vector<ActionGroup> actions;

    for (std::size_t action = 0; action < rowsOfAction.size(); ++action)
    {
        ActionGroup group{};

        for (const auto row : rowsOfAction[action])
        {
            group.mEvents.push_back(aRows[row].mEvent);
        }

        group.mOrderUnknown =
            (action > 0 && dateOf(action - 1) == dateOf(action)) ||
            (action + 1 < rowsOfAction.size() && dateOf(action + 1) == dateOf(action));
        actions.push_back(std::move(group));
    }

    return actions;
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

const TradeTransaction& tradeOf(const std::vector<TradeInstrument>& aInstruments,
                                const StatementEventReference& aEvent) {
    return aInstruments.at(aEvent.mInstrumentIndex.value()).mTransactions.at(aEvent.mEventIndex);
}

/// The trades of one ISIN in date order: every trade up to 31 December, then those after it in
/// the loss-analysis view.
std::vector<StatementEventReference> tradesOf(const std::vector<TradeInstrument>& aInstruments,
                                              const ReportingYearLedger& aLedger,
                                              std::size_t aInstrumentIndex) {
    std::vector<StatementEventReference> trades;

    const auto add = [&](const StatementEventReference& aEvent, bool aAfterYear) {
        if (aEvent.mKind != StatementEventKind::Trade ||
            aEvent.mInstrumentIndex != aInstrumentIndex)
        {
            return;
        }

        const auto& trade = tradeOf(aInstruments, aEvent);

        if (aAfterYear == (trade.mMetadata.mTaxDate > aLedger.mYearEnd))
        {
            trades.push_back(aEvent);
        }
    };

    for (const auto& event : aLedger.mCapitalEvents)
    {
        add(event, false);
    }

    for (const auto& event : aLedger.mLossAnalysisEvents)
    {
        add(event, true);
    }

    return trades;
}

/// The dates the action may have, each with the open units just before the action on that date.
/// The dates run from the day after aPreviousDate (or the first trade) to the day before
/// aNextActionDate, and at most to aLastDate. A date with no units, or after a sale of more units
/// than were held, is left out. Each range also keeps the lowest running total of the trades
/// after the action, so a later sale can be checked against it.
std::vector<EffectiveDateRange> dateRanges(const std::vector<TradeInstrument>& aInstruments,
                                           const ReportingYearLedger& aLedger,
                                           std::size_t aInstrumentIndex,
                                           Date aAnchorDate,
                                           Units aAnchorPosition,
                                           std::optional<Date> aPreviousDate,
                                           std::optional<Date> aNextActionDate,
                                           Date aLastDate,
                                           bool aNextAsksItsTrades) {
    // The trades of the ISIN per date, in date order.
    std::vector<TradeDay> days;

    for (const auto& event : tradesOf(aInstruments, aLedger, aInstrumentIndex))
    {
        const auto& trade = tradeOf(aInstruments, event);
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
    // date. The caller passes a next action only when it is up to aLastDate.
    const Date earliest = aPreviousDate ? *aPreviousDate + DayDuration{1} : days.front().mDate;
    const Date latest = aNextActionDate ? *aNextActionDate - DayDuration{1} : aLastDate;

    // A trade at most kBrokerDateMarginDays before the next action's row may come after that
    // action, which the broker booked later; the next action's own question checks it. Only a
    // split has such a question: every trade up to a merger's row must fit.
    const std::optional<Date> checkedUntil =
        aNextActionDate && aNextAsksItsTrades
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
                            bool aAskSameEvent,
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
        .mAskSameEvent = aAskSameEvent,
    };

    for (const auto& event : aAction.mEvents)
    {
        const auto& action = actionOf(aInstruments, event);
        const auto identity = transactionIdentity(action.mMetadata);
        CorporateActionSourceFacts facts{.mBroker = brokerOf(action),
                                         .mDate = action.mMetadata.mTaxDate};

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

/// The identity question of a step whose rows may be one action or several.
DecisionQuestion identityQuestionOf(const ResolvedCorporateAction& aAction,
                                    const std::vector<IdentityRow>& aRows,
                                    const std::vector<TradeInstrument>& aInstruments) {
    CorporateActionIdentityQuestion data{.mIsin = aAction.mIsin, .mName = aAction.mName};

    for (const auto& row : aRows)
    {
        data.mRows.push_back(row.mFacts);
    }

    return DecisionQuestion{
        .mType = RequestType::CorporateActionIdentityRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mSources = sourcesOf(aInstruments, aAction),
        .mTaxDate = aAction.mEffectiveDate,
        .mIsin = aAction.mIsin,
        .mData = std::move(data),
    };
}

/// Asks the identity question of a step, or, when the question was closed without an answer,
/// blocks the sales of the ISIN. Returns the index of the open request, if there is one.
std::optional<std::size_t> askIdentity(ResolvedCorporateAction& aAction,
                                       DecisionQuestion aQuestion,
                                       const AnswerValue* aAnswer,
                                       const std::vector<TradeInstrument>& aInstruments,
                                       CorporateActionResult& aResult) {
    // The session accepts only a grouping or a cancel, and a grouping is already applied.
    if (aAnswer != nullptr)
    {
        aAction.mState = CorporateActionState::Cancelled;
        aResult.mDiagnostics.push_back(
            actionDiagnostic(ProcessingDiagnosticCode::CorporateActionAmbiguous,
                             makeDiagnosticMessage(MessageKey::CorporateActionAmbiguousCancelled),
                             aAction,
                             aInstruments));

        return std::nullopt;
    }

    aAction.mState = CorporateActionState::WaitingForIdentity;

    RequestInput request{.mQuestion = std::move(aQuestion)};

    request.mProcessingDiagnostics.push_back(aResult.mDiagnostics.size());
    aResult.mDiagnostics.push_back(actionDiagnostic(
        ProcessingDiagnosticCode::CorporateActionAmbiguous,
        makeDiagnosticMessage(MessageKey::CorporateActionAmbiguousIdentityRequired),
        aAction,
        aInstruments));
    aResult.mRequests.push_back(std::move(request));

    return aResult.mRequests.size() - 1;
}

/// The checks that every action of an ISIN needs before its ratio can be asked: a merger is
/// unsupported, and actions that the user grouped to start on one day have no known order.
/// Returns false after adding the error.
bool checkActionRows(ResolvedCorporateAction& aAction,
                     const ActionGroup& aGroup,
                     const std::vector<TradeInstrument>& aInstruments,
                     CorporateActionResult& aResult) {
    const auto blocked =
        std::find_if(aAction.mEvents.begin(),
                     aAction.mEvents.end(),
                     [&aInstruments](const StatementEventReference& aEvent) {
                         return isNonSplitActionType(actionOf(aInstruments, aEvent).mType);
                     });

    if (blocked != aAction.mEvents.end())
    {
        aAction.mState = CorporateActionState::Unsupported;

        // An ISIN change that was not carried out has its own error and question from its stage.
        if (actionOf(aInstruments, *blocked).mType == CorporateActionType::IsinChange)
        {
            return false;
        }

        const MessageKey key =
            actionOf(aInstruments, *blocked).mType == CorporateActionType::StockMerger
                ? MessageKey::UnsupportedCorporateActionStockMerger
                : MessageKey::UnsupportedCorporateActionMerger;

        aResult.mDiagnostics.push_back(
            actionDiagnostic(ProcessingDiagnosticCode::UnsupportedCorporateAction,
                             makeDiagnosticMessage(key),
                             aAction,
                             aInstruments));

        return false;
    }

    if (aGroup.mOrderUnknown)
    {
        aAction.mState = CorporateActionState::SameDayOrderAmbiguous;
        aResult.mDiagnostics.push_back(actionDiagnostic(
            ProcessingDiagnosticCode::CorporateActionAmbiguous,
            makeDiagnosticMessage(MessageKey::CorporateActionAmbiguousSameDayOrder),
            aAction,
            aInstruments));

        return false;
    }

    return true;
}

/// The neighbours of the first action a pool stopped at: the effective date of the action applied
/// before it, if any, the row date of the next action, if any, and the last date the action may
/// have: 31 December, or the end of the loss-analysis view for an action with a row after it.
struct NeighbourDates {
    std::optional<Date> mPrevious;
    std::optional<Date> mNext;
    Date mLast;
    /// The next action is a split, whose own question checks the trades up to
    /// kBrokerDateMarginDays before its row. A merger or an ISIN change has no such question.
    bool mNextAsksItsTrades{}; ///< Always set by the creator.
};

/// Where the date ranges of the first action start from: the row date with the open units, or,
/// for a sale with too few units just before the row, that sale's date with the units at the
/// start of its day.
struct RangeAnchor {
    Date mDate;
    Units mPosition{};
    /// Set for a sale with too few units.
    std::optional<Date> mEarlierSaleDate;
    /// That sale, up to 31 December (not a January sale): a cancel of its question means missing
    /// purchase history.
    std::optional<StatementEventReference> mShortSale;
};

/// The anchor of an action whose pool did not stop at a sale: the open units at the row date, or
/// a January sale with too few units before a row after 31 December. Empty when the units after a
/// confirmed action do not fit Units.
std::optional<RangeAnchor> rowAnchor(const ResolvedCorporateAction& aAction,
                                     const FifoIsinResult& aPool,
                                     const std::vector<TradeInstrument>& aInstruments,
                                     const ReportingYearLedger& aLedger,
                                     Units aOpenUnits) {
    const auto& laterActions = aPool.mLaterActions;
    std::size_t appliedActions = 0;
    Units position = aOpenUnits;
    Units dayStart = aOpenUnits;
    std::optional<Date> day;

    // In date order; a later action changes the units before the trades of its date, rounded once
    // as in the pool.
    const auto applyActionsUpTo = [&](Date aDate) {
        while (appliedActions < laterActions.size() &&
               laterActions[appliedActions].mEffectiveDate <= aDate)
        {
            if (position > 0)
            {
                const auto adjusted =
                    previewSplitUnits(position, laterActions[appliedActions].mRatio);

                if (!adjusted)
                {
                    return false;
                }

                position = *adjusted;
            }

            ++appliedActions;
        }

        return true;
    };

    for (const auto& event :
         tradesOf(aInstruments, aLedger, aAction.mEvents.front().mInstrumentIndex.value()))
    {
        const auto& trade = tradeOf(aInstruments, event);
        const auto date = trade.mMetadata.mTaxDate;

        if (date <= aLedger.mYearEnd)
        {
            continue;
        }

        if (date >= aAction.mEffectiveDate)
        {
            break;
        }

        if (!applyActionsUpTo(date))
        {
            return std::nullopt;
        }

        if (day != date)
        {
            day = date;
            dayStart = position;
        }

        position += trade.mTradeSide == TradeSide::Buy ? trade.mUnits : -trade.mUnits;

        if (position >= 0)
        {
            continue;
        }

        // The broker may have booked the action after it took effect before this sale, as for a
        // sale of the year. The sale is after 31 December, so a cancel is a plain cancel, not its
        // missing history. A row later than that points to missing purchases, so no units are known
        // before the action.
        if (aAction.mEffectiveDate - date <= DayDuration{kBrokerDateMarginDays})
        {
            return RangeAnchor{.mDate = date, .mPosition = dayStart, .mEarlierSaleDate = date};
        }

        return RangeAnchor{.mDate = aAction.mEffectiveDate, .mPosition = 0};
    }

    if (!applyActionsUpTo(aAction.mEffectiveDate - DayDuration{1}))
    {
        return std::nullopt;
    }

    return RangeAnchor{.mDate = aAction.mEffectiveDate, .mPosition = position};
}

/// The anchor of the pool's first action; aOpenUnits are the pool's open units. For a pool stopped
/// at a sale with too few units, the trades of the sale's day before it in the ledger are taken
/// off, because the action comes before the trades of its day. Empty as for rowAnchor.
std::optional<RangeAnchor> rangeAnchor(const ResolvedCorporateAction& aAction,
                                       const FifoIsinResult& aPool,
                                       const std::vector<TradeInstrument>& aInstruments,
                                       const ReportingYearLedger& aLedger,
                                       Units aOpenUnits) {
    if (!aPool.mShortSale)
    {
        return rowAnchor(aAction, aPool, aInstruments, aLedger, aOpenUnits);
    }

    const auto instrumentIndex = aAction.mEvents.front().mInstrumentIndex.value();
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

    return RangeAnchor{.mDate = saleDate,
                       .mPosition = position,
                       .mEarlierSaleDate = saleDate,
                       .mShortSale = aPool.mShortSale};
}

/// What resolving the first action of a pool gave.
struct FirstActionOutcome {
    /// The index of its open ratio request, if it has one.
    std::optional<std::size_t> mOpenRequest;
    /// The user said that the rows of the question are two actions, not one: the caller makes two
    /// steps of them and asks again.
    bool mSeparated{};
};

/// Resolves the first action a pool stopped at: asks its ratio, or takes a recorded answer.
FirstActionOutcome resolveFirstAction(ResolvedCorporateAction& aAction,
                                      const ActionGroup& aGroup,
                                      const FifoIsinResult& aPool,
                                      const std::vector<TradeInstrument>& aInstruments,
                                      const ReportingYearLedger& aLedger,
                                      NeighbourDates aNeighbours,
                                      RunDecisions& aDecisions,
                                      CorporateActionResult& aResult) {
    const auto instrumentIndex = aAction.mEvents.front().mInstrumentIndex.value();

    if (!checkActionRows(aAction, aGroup, aInstruments, aResult))
    {
        return {};
    }

    const auto openUnits = openUnitsOf(aPool);

    if (!std::holds_alternative<std::int64_t>(openUnits))
    {
        aAction.mState = CorporateActionState::NumericFailure;
        aResult.mDiagnostics.push_back(numericDiagnostic(std::get<NumericError>(openUnits),
                                                         actionContext(aAction, aInstruments)));

        return {};
    }

    const auto foundAnchor =
        rangeAnchor(aAction, aPool, aInstruments, aLedger, std::get<std::int64_t>(openUnits));

    if (!foundAnchor)
    {
        aAction.mState = CorporateActionState::NumericFailure;
        aResult.mDiagnostics.push_back(
            numericDiagnostic(NumericError::Overflow, actionContext(aAction, aInstruments)));

        return {};
    }

    const auto& anchor = *foundAnchor;
    auto ranges = dateRanges(aInstruments,
                             aLedger,
                             instrumentIndex,
                             anchor.mDate,
                             anchor.mPosition,
                             aNeighbours.mPrevious,
                             aNeighbours.mNext,
                             aNeighbours.mLast,
                             aNeighbours.mNextAsksItsTrades);

    // A broker row for an action while the files show no units points to missing history. Before
    // a sale with too few units the same holds when no date before it has units.
    if (anchor.mPosition <= 0 || ranges.empty())
    {
        aAction.mState = CorporateActionState::NoPosition;
        aResult.mDiagnostics.push_back(
            actionDiagnostic(ProcessingDiagnosticCode::CorporateActionMissingPosition,
                             makeDiagnosticMessage(MessageKey::CorporateActionMissingPosition),
                             aAction,
                             aInstruments));

        return {};
    }

    const auto positionBefore =
        anchor.mEarlierSaleDate ? std::nullopt : std::optional<Units>{anchor.mPosition};
    auto question = questionOf(aAction,
                               aGroup.mAskSameEvent,
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

        return {.mOpenRequest = aResult.mRequests.size() - 1};
    }

    FirstActionOutcome outcome;
    const auto apply = [&aAction, &aPool, &anchor, &aInstruments, &aResult, &outcome](
                           const auto& aAnswer) {
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

            // The question before a sale with too few units says that a cancel means missing
            // purchases, so the sale gets the error it would have without the action row.
            if (anchor.mShortSale)
            {
                aResult.mDiagnostics.push_back(
                    incompleteHistoryDiagnostic(aPool, tradeOf(aInstruments, *anchor.mShortSale)));
                return;
            }

            aResult.mDiagnostics.push_back(actionDiagnostic(
                ProcessingDiagnosticCode::CorporateActionAmbiguous,
                makeDiagnosticMessage(MessageKey::CorporateActionAmbiguousCancelled),
                aAction,
                aInstruments));
        }
        else if constexpr (std::is_same_v<Answer, SeparateEventsAnswer>)
        {
            // Two splits a few days apart: the caller asks the first one alone, then the second.
            outcome.mSeparated = true;
        }
        else
        {
            static_assert(std::is_same_v<Answer, FailureResolutionAnswer> ||
                          std::is_same_v<Answer, CorporateActionGroupingAnswer> ||
                          std::is_same_v<Answer, IsinChangeAnswer> ||
                          std::is_same_v<Answer, NoOtherActivityAnswer> ||
                          std::is_same_v<Answer, OtherActivityAnswer> ||
                          std::is_same_v<Answer, CapitalGainsAnswer> ||
                          std::is_same_v<Answer, ExcludeFromReportsAnswer> ||
                          std::is_same_v<Answer, QuantityCorrectionAnswer> ||
                          std::is_same_v<Answer, PayerChoiceAnswer> ||
                          std::is_same_v<Answer, PayerCountryAnswer> ||
                          std::is_same_v<Answer, DepositInterestTotalAnswer> ||
                          std::is_same_v<Answer, IncomeTreatmentAnswer> ||
                          std::is_same_v<Answer, BondTypeAnswer> ||
                          std::is_same_v<Answer, TreatyClaimAnswer> ||
                          std::is_same_v<Answer, DeveloperExportConfirmAnswer>);
            // The session accepts only a ratio or a cancel for a ratio question.
            throw std::logic_error("Only a ratio or a cancel can answer a ratio question.");
        }
    };

    std::visit(apply, *answer);

    return outcome;
}

/// The applied action of the pool that starts with this action row, if there is one: applied to
/// the lots, or confirmed with an effective date after 31 December for the loss rule only.
const AppliedCorporateAction* appliedActionOf(const FifoIsinResult& aPool,
                                              const StatementEventReference& aEvent) {
    for (const auto* list : {&aPool.mAppliedActions, &aPool.mLaterActions})
    {
        const auto applied = std::find_if(list->begin(),
                                          list->end(),
                                          [&aEvent](const AppliedCorporateAction& aApplied) {
                                              return aApplied.mEvents.front() == aEvent;
                                          });

        if (applied != list->end())
        {
            return &*applied;
        }
    }

    return nullptr;
}

/// Tells whether a loss sale of the selected year has its 30-day window, with the days a broker
/// may book an action late, up to aDate or later, so the loss rule needs an action on that date:
/// an action may have taken effect inside the window and be booked up to
/// kBrokerDateMarginDays after its end. Units past the holding period are not checked.
bool isInLossWindow(const FifoIsinResult& aPool, Date aDate) {
    return std::any_of(
        aPool.mMatches.begin(),
        aPool.mMatches.end(),
        [aDate](const FifoMatch& aMatch) {
            return aMatch.mGainTicks.numerator() < 0 && !isPastHoldingPeriod(aMatch) &&
                   aMatch.mSaleDate + DayDuration{kLossWindowDays + kBrokerDateMarginDays} >= aDate;
        });
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

/// Marks an action that the pool applied, or keeps for the loss rule, and adds the warning that
/// the user supplied its ratio. A row that carries its ratio from the start (an answered ISIN
/// change) has its own warning from that stage, so it gets none here.
void markApplied(ResolvedCorporateAction& aAction,
                 const AppliedCorporateAction& aApplied,
                 const std::vector<ConfirmedCorporateAction>& aConfirmedActions,
                 const std::vector<TradeInstrument>& aInstruments,
                 CorporateActionResult& aResult) {
    const auto& confirmed = confirmedActionOf(aConfirmedActions, aApplied.mEvents.front());

    aAction.mState = CorporateActionState::Applied;
    aAction.mRatio = confirmed.mRatio;
    aAction.mConfirmedDate = aApplied.mEffectiveDate;

    if (actionOf(aInstruments, aApplied.mEvents.front()).mRatio)
    {
        return;
    }

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

/// Replaces a step of rows on two dates by two steps, one for each date, which the ratio
/// questions ask in date order.
void splitByDate(std::vector<ActionGroup>& aGroups,
                 std::size_t aPosition,
                 const std::vector<TradeInstrument>& aInstruments) {
    const auto firstDate =
        actionOf(aInstruments, aGroups[aPosition].mEvents.front()).mMetadata.mTaxDate;
    const auto events = aGroups[aPosition].mEvents;
    ActionGroup later{};

    aGroups[aPosition].mEvents.clear();
    aGroups[aPosition].mAskSameEvent = false;

    for (const auto& event : events)
    {
        auto& target = actionOf(aInstruments, event).mMetadata.mTaxDate == firstDate
                           ? aGroups[aPosition].mEvents
                           : later.mEvents;

        target.push_back(event);
    }

    aGroups.insert(aGroups.begin() + static_cast<std::ptrdiff_t>(aPosition) + 1, std::move(later));
}

/// The calendar year of a date.
int yearOf(Date aDate) {
    const std::chrono::year_month_day day{std::chrono::sys_days{aDate.time_since_epoch()}};

    return static_cast<int>(day.year());
}

/// Adds a warning for each calendar year in which the action rows of the ISIN come from more than
/// one file: the files may describe the same actions differently, and only the announcements
/// can say. The rows of one file, or the copies of one row from one file, never differ this way.
void warnAboutSourceMismatch(const FifoIsinResult& aPool,
                             const std::vector<TradeInstrument>& aInstruments,
                             const std::vector<std::vector<StatementEventReference>>& aDateGroups,
                             CorporateActionResult& aResult) {
    const auto dateOf = [&](std::size_t aGroup) {
        return actionOf(aInstruments, aDateGroups[aGroup].front()).mMetadata.mTaxDate;
    };
    std::size_t group = 0;

    // The date groups are in date order, so the rows of a year are next to each other.
    while (group < aDateGroups.size())
    {
        const auto year = yearOf(dateOf(group));
        ResolvedCorporateAction rows{.mIsin = aPool.mIsin,
                                     .mName = aPool.mName,
                                     .mEffectiveDate = dateOf(group)};

        for (; group < aDateGroups.size() && yearOf(dateOf(group)) == year; ++group)
        {
            rows.mEvents.insert(rows.mEvents.end(),
                                aDateGroups[group].begin(),
                                aDateGroups[group].end());
        }

        const auto sources = sourcesOf(aInstruments, rows);
        const auto fileOf = [](const SourceReference& aSource) {
            return aSource.mInputSequence.mSourceIndex;
        };
        const bool fromSeveralFiles =
            std::any_of(sources.begin(), sources.end(), [&](const SourceReference& aSource) {
                return fileOf(aSource) != fileOf(sources.front());
            });

        if (fromSeveralFiles)
        {
            auto diagnostic = actionDiagnostic(
                ProcessingDiagnosticCode::CorporateActionSourcesMismatchPossible,
                makeDiagnosticMessage(MessageKey::CorporateActionSourcesMismatchPossible,
                                      {numberParam("year", static_cast<std::uint64_t>(year))}),
                rows,
                aInstruments);

            diagnostic.mSeverity = DiagnosticSeverity::Warning;
            aResult.mDiagnostics.push_back(std::move(diagnostic));
        }
    }
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

    // aFifo.mIsins follows the instruments, so an index is both; fifoPoolAt checks it.
    for (std::size_t index = 0; index < instruments.size(); ++index)
    {
        const auto& pool = fifoPoolAt(aFifo, instruments, index);

        // A class that cannot reach Doh-KDVP has its own class error, so its actions are not
        // asked and give no error of their own.
        if (!hasSaleInYear(instruments.at(index), aLedger) || aLedger.isLeftOut(pool.mIsin) ||
            !mayReachKdvp(instruments.at(index).mAssetClass))
        {
            continue;
        }

        const auto dateGroups = actionsByDate(instruments, aLedger, index);

        warnAboutSourceMismatch(pool, instruments, dateGroups, result);

        auto groups = actionGroupsOf(instruments, dateGroups);
        std::optional<std::size_t> openRequest;
        std::optional<Date> previousDate;
        bool reachedFirst = false;
        std::size_t position = 0;

        while (position < groups.size())
        {
            ResolvedCorporateAction action{
                .mIsin = pool.mIsin,
                .mName = pool.mName,
                .mEffectiveDate =
                    actionOf(instruments, groups[position].mEvents.front()).mMetadata.mTaxDate,
                .mEvents = groups[position].mEvents,
            };

            // Rows that may be one action or several wait for the user's grouping, which turns
            // them into the actions of the user's answer.
            std::optional<DecisionQuestion> identityQuestion;
            const AnswerValue* identityAnswer = nullptr;

            if (groups[position].mNeedsIdentity)
            {
                const auto rows = identityRowsOf(instruments, groups[position].mEvents);

                identityQuestion = identityQuestionOf(action, rows, instruments);
                identityAnswer = aDecisions.find(*identityQuestion);

                if (const auto* grouping =
                        identityAnswer ? std::get_if<CorporateActionGroupingAnswer>(identityAnswer)
                                       : nullptr)
                {
                    auto actions = actionsOfGrouping(rows, *grouping);

                    groups.erase(groups.begin() + static_cast<std::ptrdiff_t>(position));
                    groups.insert(groups.begin() + static_cast<std::ptrdiff_t>(position),
                                  actions.begin(),
                                  actions.end());
                    std::stable_sort(
                        groups.begin() + static_cast<std::ptrdiff_t>(position),
                        groups.end(),
                        [&instruments](const ActionGroup& aLeft, const ActionGroup& aRight) {
                            return actionOf(instruments, aLeft.mEvents.front()).mMetadata.mTaxDate <
                                   actionOf(instruments, aRight.mEvents.front()).mMetadata.mTaxDate;
                        });

                    continue;
                }
            }

            const bool reachesNextYear = std::any_of(
                action.mEvents.begin(),
                action.mEvents.end(),
                [&instruments, &aLedger](const StatementEventReference& aEvent) {
                    return actionOf(instruments, aEvent).mMetadata.mTaxDate > aLedger.mYearEnd;
                });
            const bool afterYear = action.mEffectiveDate > aLedger.mYearEnd;

            // A row after 31 December waits for nothing: it matters only when it is asked.
            if (reachedFirst && afterYear)
            {
                break;
            }

            if (!reachedFirst)
            {
                if (const auto* applied = appliedActionOf(pool, groups[position].mEvents.front()))
                {
                    // The first of two dates was applied alone: the user said they are two splits.
                    // Only a step of two dates can be split; a split step is never one again.
                    if (groups[position].mAskSameEvent &&
                        applied->mEvents != groups[position].mEvents)
                    {
                        splitByDate(groups, position, instruments);
                        continue;
                    }

                    markApplied(action, *applied, aConfirmedActions, instruments, result);
                    previousDate = applied->mEffectiveDate;
                    result.mActions.push_back(std::move(action));
                    ++position;
                    continue;
                }

                // A pool that was not stopped asks a split row after 31 December only when the
                // loss rule needs it; a merger row there blocks the loss sale in the loss stage.
                const bool neededByLossRule = pool.mStatus == FifoIsinStatus::Matched &&
                                              afterYear &&
                                              !hasMerger(instruments, action.mEvents) &&
                                              isInLossWindow(pool, action.mEffectiveDate);

                // The pool stopped earlier for another reason (missing history, a non-EUR trade,
                // an action that could not be applied), which has its own error.
                if (pool.mStatus != FifoIsinStatus::CorporateAction && !neededByLossRule)
                {
                    break;
                }

                const Date lastDate = reachesNextYear ? aLedger.mLossWindowEnd : aLedger.mYearEnd;
                const auto nextRowDate =
                    position + 1 < groups.size()
                        ? std::optional<Date>{actionOf(instruments,
                                                       groups[position + 1].mEvents.front())
                                                  .mMetadata.mTaxDate}
                        : std::nullopt;
                const auto nextActionDate =
                    nextRowDate && *nextRowDate <= lastDate ? nextRowDate : std::nullopt;

                reachedFirst = true;

                if (identityQuestion)
                {
                    openRequest = askIdentity(action,
                                              std::move(*identityQuestion),
                                              identityAnswer,
                                              instruments,
                                              result);
                    result.mActions.push_back(std::move(action));
                    ++position;
                    continue;
                }

                const auto outcome = resolveFirstAction(
                    action,
                    groups[position],
                    pool,
                    instruments,
                    aLedger,
                    {.mPrevious = previousDate,
                     .mNext = nextActionDate,
                     .mLast = lastDate,
                     .mNextAsksItsTrades =
                         nextActionDate && !hasMerger(instruments, groups[position + 1].mEvents)},
                    aDecisions,
                    result);

                // "Not one split" gives the two dates as two steps; the first is asked again.
                if (outcome.mSeparated)
                {
                    splitByDate(groups, position, instruments);
                    reachedFirst = false;
                    continue;
                }

                openRequest = outcome.mOpenRequest;
                result.mActions.push_back(std::move(action));
                ++position;
                continue;
            }

            // Whether the rows are one action or several is unknown, or they are a merger,
            // whatever happens to the earlier action.
            if (identityQuestion)
            {
                askIdentity(action,
                            std::move(*identityQuestion),
                            identityAnswer,
                            instruments,
                            result);
                result.mActions.push_back(std::move(action));
                ++position;
                continue;
            }

            if (!checkActionRows(action, groups[position], instruments, result))
            {
                result.mActions.push_back(std::move(action));
                ++position;
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
            ++position;
        }
    }

    return result;
}

CapitalPools matchCapitalPools(const MergedStatement& aStatement,
                               const ReportingYearLedger& aLedger,
                               RunDecisions& aDecisions) {
    const auto& instruments = aStatement.mPresentation.mTradeInstruments;
    const std::size_t rows = std::accumulate(
        instruments.begin(),
        instruments.end(),
        std::size_t{0},
        [](std::size_t aCount, const TradeInstrument& aInstrument) {
            return aCount + aInstrument.mCorporateActions.size() + aInstrument.mTransactions.size();
        });
    std::vector<ConfirmedCorporateAction> confirmed;
    std::vector<QuantityCorrection> corrections;
    std::optional<MergedStatement> corrected;

    // A row with a verified ratio (the ISIN change stage leaves one for a ratio other than 1 / 1)
    // is confirmed from the start, with the date of its row.
    for (std::size_t instrument = 0; instrument < instruments.size(); ++instrument)
    {
        for (std::size_t row = 0; row < instruments[instrument].mCorporateActions.size(); ++row)
        {
            const CorporateAction& action = instruments[instrument].mCorporateActions[row];

            if (action.mRatio)
            {
                confirmed.push_back({
                    .mEvents = {{StatementEventKind::CorporateAction, instrument, row}},
                    .mRatio = *action.mRatio,
                    .mEffectiveDate = action.mMetadata.mTaxDate,
                });
            }
        }
    }

    // A round that confirms nothing new and corrects nothing returns. An action is confirmed once
    // and a sale is corrected once, so one round per action row and sale and a last one are
    // enough.
    for (std::size_t round = 0; round <= rows; ++round)
    {
        const auto& statement = corrected ? *corrected : aStatement;
        auto fifo = FifoMatcher::match(statement, aLedger, confirmed, corrections);
        auto actions =
            CorporateActionResolver::resolve(statement, aLedger, fifo, confirmed, aDecisions);
        auto quantities =
            QuantityReconciler::reconcile(statement, aLedger, fifo, corrections, aDecisions);
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

        if (confirmed.size() == confirmedBefore && quantities.mNewCorrections.empty())
        {
            return CapitalPools{.mFifo = std::move(fifo),
                                .mActions = std::move(actions),
                                .mQuantities = std::move(quantities),
                                .mCorrectedStatement = std::move(corrected)};
        }

        corrections.insert(corrections.end(),
                           quantities.mNewCorrections.begin(),
                           quantities.mNewCorrections.end());
        corrected = withQuantityCorrections(aStatement, corrections);
    }

    throw std::logic_error("The corporate actions or quantities were still changing after one "
                           "round per row.");
}

} // namespace taxbroker
