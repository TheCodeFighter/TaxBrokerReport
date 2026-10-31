#include "processors/loss_eligibility.hpp"
#include "taxbroker/exact_arithmetic.hpp"
#include "taxbroker/split_ratio.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <map>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace taxbroker {
namespace {

constexpr DayDuration kWindowDays{kLossWindowDays};

enum class IsinEventKind {
    Purchase,
    Sale,
    CorporateAction,
};

/// One trade or corporate action of an ISIN, in ledger order.
struct IsinEvent {
    IsinEventKind mKind{};
    StatementEventReference mReference;
    Date mDate;
    Units mUnits{};
};

/// A purchase that may replace the loss units of a sale, with the units already used, both in
/// comparable units (see UnitScale).
struct Candidate {
    /// Position in the ISIN's events, to tell whether it comes after a sale.
    std::size_t mPosition{};
    StatementEventReference mPurchase;
    Date mDate;
    WideInteger mUnits{};
    WideInteger mUsedUnits{};
};

/// A confirmed action of the pool. Trades dated before it are in the units before it, trades on or
/// after its date in the units after it, as in the FIFO pool.
struct PoolAction {
    Date mDate;
    SplitRatio mRatio;
};

/// Whole numbers that compare units across the pool's confirmed actions: a trade's units times the
/// old numbers of the actions dated on or before it and the new numbers of the others. Example:
/// with `2 / 1` on 2024-01-15, 1 unit bought before it (times 2) and 2 units bought on or after it
/// (times 1) are the same number: one unit before the split is two after it.
struct UnitScale {
    /// In date order.
    std::vector<PoolAction> mActions;
    /// By the number of actions dated on or before the trade, so one more than mActions.
    std::vector<WideInteger> mMultipliers;
};

/// Largest product of the new (or of the old) numbers of an ISIN's confirmed ratios: 2^64.
constexpr unsigned kMaxRatioProductBits = 64;

/// The pool's confirmed actions as a scale, or empty when the product of their new numbers or of
/// their old numbers is above 2^64: every comparable unit then stays far inside 256 bits.
std::optional<UnitScale> unitScaleOf(const FifoIsinResult& aPool) {
    const WideInteger maxProduct = WideInteger{1} << kMaxRatioProductBits;
    UnitScale scale;

    for (const auto* list : {&aPool.mAppliedActions, &aPool.mLaterActions})
    {
        for (const auto& action : *list)
        {
            scale.mActions.push_back({.mDate = action.mEffectiveDate, .mRatio = action.mRatio});
        }
    }

    std::stable_sort(scale.mActions.begin(),
                     scale.mActions.end(),
                     [](const PoolAction& aLeft, const PoolAction& aRight) {
                         return aLeft.mDate < aRight.mDate;
                     });

    WideInteger newProduct{1};
    WideInteger oldProduct{1};

    for (const auto& action : scale.mActions)
    {
        newProduct *= action.mRatio.mNewShares;
        oldProduct *= action.mRatio.mOldShares;

        if (newProduct > maxProduct || oldProduct > maxProduct)
        {
            return std::nullopt;
        }
    }

    for (std::size_t level = 0; level <= scale.mActions.size(); ++level)
    {
        WideInteger multiplier{1};

        for (std::size_t index = 0; index < scale.mActions.size(); ++index)
        {
            const auto& ratio = scale.mActions[index].mRatio;

            multiplier *= index < level ? ratio.mOldShares : ratio.mNewShares;
        }

        scale.mMultipliers.push_back(multiplier);
    }

    return scale;
}

/// The multiplier of a trade on aDate: an action on that date comes before the trade.
const WideInteger& multiplierOn(const UnitScale& aScale, Date aDate) {
    const auto level =
        std::count_if(aScale.mActions.begin(),
                      aScale.mActions.end(),
                      [aDate](const PoolAction& aAction) { return aAction.mDate <= aDate; });

    return aScale.mMultipliers.at(static_cast<std::size_t>(level));
}

/// The units of a trade on aDate as comparable units.
WideInteger comparableUnits(const UnitScale& aScale, Units aUnits, Date aDate) {
    return WideInteger{aUnits} * multiplierOn(aScale, aDate);
}

/// Comparable units in the units on aDate, rounded up to 8 decimals: a replacement that does not
/// fit 8 decimals blocks at most 0.00000001 unit more (the safe side; maintainer decision,
/// 2026-10-29). aComparable is at most the sale's loss units, so the result fits Units.
Units unitsOn(const UnitScale& aScale, const WideInteger& aComparable, Date aDate) {
    const auto& multiplier = multiplierOn(aScale, aDate);

    return ((aComparable + multiplier - 1) / multiplier).convert_to<Units>();
}

/// The events of one ISIN: every trade up to 31 December, then the trades and corporate actions
/// of the loss-analysis view after it. Sales after 31 December are left out: they never take a
/// replacement before a sale of the year.
struct IsinHistory {
    std::vector<IsinEvent> mEvents;
    /// Corporate actions after 31 December, in ledger order.
    std::vector<IsinEvent> mLaterActions;
};

std::vector<IsinHistory> collectHistories(const std::vector<TradeInstrument>& aInstruments,
                                          const ReportingYearLedger& aLedger) {
    std::vector<IsinHistory> histories(aInstruments.size());

    const auto add = [&aInstruments, &histories](const StatementEventReference& aEvent,
                                                 bool aAfterYear) {
        const auto instrumentIndex = aEvent.mInstrumentIndex.value();
        const auto& instrument = aInstruments.at(instrumentIndex);
        auto& history = histories.at(instrumentIndex);

        if (aEvent.mKind == StatementEventKind::CorporateAction)
        {
            // A corporate action up to 31 December stops the pool, so only later ones matter.
            if (aAfterYear)
            {
                const auto& action = instrument.mCorporateActions.at(aEvent.mEventIndex);

                history.mLaterActions.push_back({.mKind = IsinEventKind::CorporateAction,
                                                 .mReference = aEvent,
                                                 .mDate = action.mMetadata.mTaxDate});
            }

            return;
        }

        const auto& trade = instrument.mTransactions.at(aEvent.mEventIndex);

        if (trade.mTradeSide == TradeSide::Sell && aAfterYear)
        {
            return;
        }

        history.mEvents.push_back({.mKind = trade.mTradeSide == TradeSide::Buy
                                                ? IsinEventKind::Purchase
                                                : IsinEventKind::Sale,
                                   .mReference = aEvent,
                                   .mDate = trade.mMetadata.mTaxDate,
                                   .mUnits = trade.mUnits});
    };

    for (const auto& event : aLedger.mCapitalEvents)
    {
        add(event, false);
    }

    for (const auto& event : aLedger.mLossAnalysisEvents)
    {
        const auto& instrument = aInstruments.at(event.mInstrumentIndex.value());
        const auto& metadata = event.mKind == StatementEventKind::CorporateAction
                                   ? instrument.mCorporateActions.at(event.mEventIndex).mMetadata
                                   : instrument.mTransactions.at(event.mEventIndex).mMetadata;

        if (metadata.mTaxDate > aLedger.mYearEnd)
        {
            add(event, true);
        }
    }

    return histories;
}

/// The matches of each sale, by the sale's event index, earlier years included.
std::map<std::size_t, std::vector<const FifoMatch*>> matchesBySale(const FifoIsinResult& aPool) {
    std::map<std::size_t, std::vector<const FifoMatch*>> matches;

    for (const auto* list : {&aPool.mEarlierMatches, &aPool.mMatches})
    {
        for (const auto& match : *list)
        {
            matches[match.mSale.mEventIndex].push_back(&match);
        }
    }

    return matches;
}

/// The units of the loss-making matches within the holding period. A loss on units past the
/// holding period cannot reduce the tax base, so it is not checked.
Units lossUnitsOf(const std::vector<const FifoMatch*>& aMatches) {
    return std::accumulate(aMatches.begin(),
                           aMatches.end(),
                           Units{0},
                           [](Units aSum, const FifoMatch* aMatch) {
                               return aMatch->mGainTicks.numerator() < 0 &&
                                              !isPastHoldingPeriod(*aMatch)
                                          ? aSum + aMatch->mUnits
                                          : aSum;
                           });
}

/// Takes unused units of the sale's replacement candidates, earliest purchase first, up to its
/// loss units. The units of each use are in the sale's units.
std::vector<LossReplacementUse> takeReplacements(std::vector<Candidate>& aCandidates,
                                                 const IsinEvent& aSale,
                                                 std::size_t aSalePosition,
                                                 Date aOriginalDate,
                                                 bool aEmptiesPool,
                                                 Units aLossUnits,
                                                 const UnitScale& aScale) {
    const Date windowStart = aSale.mDate - kWindowDays;
    const Date windowEnd = aSale.mDate + kWindowDays;
    std::vector<LossReplacementUse> uses;
    WideInteger remaining = comparableUnits(aScale, aLossUnits, aSale.mDate);
    Units usedSaleUnits = 0;

    for (auto& candidate : aCandidates)
    {
        if (remaining == 0)
        {
            break;
        }

        // The purchase of the sale's first lot, and any purchase before the sale on or before that
        // purchase's date, is the sale's own purchase, not a replacement. A purchase after the sale
        // is a replacement even on that date (a rebuy the same day). A sale that leaves no units
        // keeps its loss against the purchases before it.
        const bool inWindow = candidate.mDate >= windowStart && candidate.mDate <= windowEnd;
        const bool afterOriginal =
            candidate.mDate > aOriginalDate || candidate.mPosition > aSalePosition;
        const bool countsForEmptyPool = !aEmptiesPool || candidate.mPosition > aSalePosition;
        const WideInteger available = candidate.mUnits - candidate.mUsedUnits;

        if (!inWindow || !afterOriginal || !countsForEmptyPool || available == 0)
        {
            continue;
        }

        const WideInteger taken = std::min(remaining, available);

        // Rounded up, so the uses of the sale never add up to more than its loss units.
        const Units units =
            std::min(unitsOn(aScale, taken, aSale.mDate), aLossUnits - usedSaleUnits);

        candidate.mUsedUnits += taken;
        remaining -= taken;
        usedSaleUnits += units;

        // Earlier uses rounded up may already reach the loss units: the purchase is used, but
        // a use of 0 units is not listed.
        if (units == 0)
        {
            continue;
        }

        uses.push_back(
            {.mPurchase = candidate.mPurchase, .mDate = candidate.mDate, .mUnits = units});
    }

    return uses;
}

/// Tells whether the action row belongs to a confirmed action of the pool.
bool isConfirmed(const FifoIsinResult& aPool, const StatementEventReference& aEvent) {
    const auto hasRow = [&aEvent](const AppliedCorporateAction& aAction) {
        return std::find(aAction.mEvents.begin(), aAction.mEvents.end(), aEvent) !=
               aAction.mEvents.end();
    };

    return std::any_of(aPool.mAppliedActions.begin(), aPool.mAppliedActions.end(), hasRow) ||
           std::any_of(aPool.mLaterActions.begin(), aPool.mLaterActions.end(), hasRow);
}

/// A sale of the selected year with a loss, before its replacements are counted.
LossSaleEligibility lossSale(const IsinEvent& aSale, Units aLossUnits) {
    return LossSaleEligibility{
        .mSale = aSale.mReference,
        .mSaleDate = aSale.mDate,
        .mWindowStart = aSale.mDate - kWindowDays,
        .mWindowEnd = aSale.mDate + kWindowDays,
        .mLossUnits = aLossUnits,
    };
}

/// Applies the rule to every sale of one ISIN up to 31 December, in ledger order, and returns
/// the sales of the selected year with a loss. Empty when the units cannot be compared across the
/// pool's confirmed actions.
std::optional<std::vector<LossSaleEligibility>> checkIsin(const IsinHistory& aHistory,
                                                          const FifoIsinResult& aPool,
                                                          const ReportingYearLedger& aLedger) {
    const auto scale = unitScaleOf(aPool);

    if (!scale)
    {
        return std::nullopt;
    }

    const auto matches = matchesBySale(aPool);
    std::vector<Candidate> candidates;

    for (std::size_t position = 0; position < aHistory.mEvents.size(); ++position)
    {
        const auto& event = aHistory.mEvents[position];

        if (event.mKind == IsinEventKind::Purchase)
        {
            candidates.push_back({.mPosition = position,
                                  .mPurchase = event.mReference,
                                  .mDate = event.mDate,
                                  .mUnits = comparableUnits(*scale, event.mUnits, event.mDate)});
        }
    }

    // The stock is needed only up to the last sale, where FIFO has already checked that the open
    // units fit. Every sale is up to 31 December, so the purchases after it are not added.
    const auto lastSale =
        std::find_if(aHistory.mEvents.rbegin(),
                     aHistory.mEvents.rend(),
                     [](const IsinEvent& aEvent) { return aEvent.mKind == IsinEventKind::Sale; });
    const auto walkedEvents =
        static_cast<std::size_t>(std::distance(lastSale, aHistory.mEvents.rend()));
    std::vector<LossSaleEligibility> sales;
    Units stock = 0;
    std::size_t appliedActions = 0;

    for (std::size_t position = 0; position < walkedEvents; ++position)
    {
        const auto& event = aHistory.mEvents[position];

        // A confirmed action changes the units held before the trades of its day, rounded once as
        // in the FIFO pool, so the stock is always the pool's open units.
        while (appliedActions < scale->mActions.size() &&
               scale->mActions[appliedActions].mDate <= event.mDate)
        {
            if (stock > 0)
            {
                const auto adjusted =
                    previewSplitUnits(stock, scale->mActions[appliedActions].mRatio);

                if (!adjusted)
                {
                    return std::nullopt;
                }

                stock = *adjusted;
            }

            ++appliedActions;
        }

        if (event.mKind == IsinEventKind::Purchase)
        {
            stock += event.mUnits;
            continue;
        }

        stock -= event.mUnits;

        // A matched pool has matches for every sale up to 31 December.
        const auto& saleMatches = matches.at(event.mReference.mEventIndex);
        const Units lossUnits = lossUnitsOf(saleMatches);

        if (lossUnits == 0)
        {
            continue;
        }

        auto replacements = takeReplacements(candidates,
                                             event,
                                             position,
                                             saleMatches.front()->mPurchaseDate,
                                             stock == 0,
                                             lossUnits,
                                             *scale);

        if (event.mDate < aLedger.mYearStart)
        {
            continue;
        }

        auto sale = lossSale(event, lossUnits);

        for (const auto& replacement : replacements)
        {
            sale.mKnownIneligibleUnits += replacement.mUnits;
        }

        sale.mReplacements = std::move(replacements);

        const auto action = std::find_if(aHistory.mLaterActions.begin(),
                                         aHistory.mLaterActions.end(),
                                         [&sale, &aPool](const IsinEvent& aAction) {
                                             return aAction.mDate <= sale.mWindowEnd &&
                                                    !isConfirmed(aPool, aAction.mReference);
                                         });

        if (action != aHistory.mLaterActions.end())
        {
            sale.mState = LossSaleState::CorporateActionInWindow;
            sale.mCorporateAction = action->mReference;
        }
        else if (sale.mKnownIneligibleUnits == lossUnits)
        {
            sale.mState = LossSaleState::Decided;
        }

        sales.push_back(std::move(sale));
    }

    return sales;
}

/// The loss sales of the selected year of an ISIN whose units cannot be compared.
std::vector<LossSaleEligibility> failedSales(const IsinHistory& aHistory,
                                             const FifoIsinResult& aPool,
                                             const ReportingYearLedger& aLedger) {
    const auto matches = matchesBySale(aPool);
    std::vector<LossSaleEligibility> sales;

    for (const auto& event : aHistory.mEvents)
    {
        if (event.mKind != IsinEventKind::Sale || event.mDate < aLedger.mYearStart)
        {
            continue;
        }

        const Units lossUnits = lossUnitsOf(matches.at(event.mReference.mEventIndex));

        if (lossUnits == 0)
        {
            continue;
        }

        auto sale = lossSale(event, lossUnits);

        sale.mState = LossSaleState::NumericFailure;
        sales.push_back(std::move(sale));
    }

    return sales;
}

/// Tells whether sales of this class can reach Doh-KDVP: shares, funds, and an unknown class,
/// which the user may still classify as one of them. The capital report gates block the others.
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

const TradeTransaction& tradeOf(const std::vector<TradeInstrument>& aInstruments,
                                const StatementEventReference& aEvent) {
    return aInstruments.at(aEvent.mInstrumentIndex.value()).mTransactions.at(aEvent.mEventIndex);
}

/// The error of an open or cancelled question, on one sale. No units or amounts in the message.
ProcessingDiagnostic
lossDiagnostic(MessageKey aKey, const TradeTransaction& aSale, const LossEligibilityIsin& aIsin) {
    auto message = makeDiagnosticMessage(aKey);

    return ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::LossEligibilityUnresolved,
        .mContext =
            {
                .mStage = PipelineStage::FifoTax,
                .mAffectedReports = {ReportType::CapitalGains},
                .mSources = aSale.mMetadata.mSources,
                .mTaxDate = aSale.mMetadata.mTaxDate,
                .mInstrumentName = aIsin.mName,
                .mIsin = aIsin.mIsin,
            },
        .mMessage = std::move(message.mEnglish),
        .mMessageKey = message.mKey,
        .mMessageParams = std::move(message.mParams),
    };
}

/// The ISIN's question: the facts of each sale whose loss the files cannot decide.
DecisionQuestion questionOf(const LossEligibilityIsin& aIsin,
                            const std::vector<std::size_t>& aOpenSales,
                            const std::vector<TradeInstrument>& aInstruments) {
    LossEligibilityQuestion data{.mIsin = aIsin.mIsin, .mName = aIsin.mName};
    DecisionQuestion question{
        .mType = RequestType::LossEligibilityConfirmationRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mIsin = aIsin.mIsin,
    };

    for (const auto index : aOpenSales)
    {
        const auto& sale = aIsin.mSales[index];
        const auto& trade = tradeOf(aInstruments, sale.mSale);
        LossSaleFacts facts{
            .mSaleDate = sale.mSaleDate,
            .mSaleTransaction = transactionIdentity(trade.mMetadata),
            .mSaleUnits = trade.mUnits,
            .mLossUnits = sale.mLossUnits,
            .mWindowStart = sale.mWindowStart,
            .mWindowEnd = sale.mWindowEnd,
            .mKnownIneligibleUnits = sale.mKnownIneligibleUnits,
            .mOpenUnits = sale.mLossUnits - sale.mKnownIneligibleUnits,
        };

        for (const auto& replacement : sale.mReplacements)
        {
            facts.mKnownReplacements.push_back(
                {.mDate = replacement.mDate, .mUnits = replacement.mUnits});
        }

        question.mSources.insert(question.mSources.end(),
                                 trade.mMetadata.mSources.begin(),
                                 trade.mMetadata.mSources.end());
        data.mSales.push_back(std::move(facts));
    }

    question.mTaxDate = aIsin.mSales[aOpenSales.front()].mSaleDate;
    question.mData = std::move(data);

    return question;
}

/// The warning of an ISIN whose open units were assumed blocked without an answer, with the
/// sources and date of its question. No units or amounts in the message.
ProcessingDiagnostic assumedBlockedDiagnostic(const DecisionQuestion& aQuestion,
                                              const LossEligibilityIsin& aIsin) {
    auto message = makeDiagnosticMessage(MessageKey::LossEligibilityUnresolvedAssumedBlocked);

    return ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Warning,
        .mCode = ProcessingDiagnosticCode::LossEligibilityUnresolved,
        .mContext =
            {
                .mStage = PipelineStage::FifoTax,
                .mAffectedReports = {ReportType::CapitalGains},
                .mSources = aQuestion.mSources,
                .mTaxDate = aQuestion.mTaxDate,
                .mInstrumentName = aIsin.mName,
                .mIsin = aIsin.mIsin,
            },
        .mMessage = std::move(message.mEnglish),
        .mMessageKey = message.mKey,
        .mMessageParams = std::move(message.mParams),
    };
}

/// One numeric error for an ISIN whose units cannot be compared, on its first loss sale. No
/// units or amounts in the message.
void failIsin(const LossEligibilityIsin& aIsin,
              const std::vector<TradeInstrument>& aInstruments,
              LossEligibilityResult& aResult) {
    if (aIsin.mSales.empty())
    {
        return;
    }

    const auto& trade = tradeOf(aInstruments, aIsin.mSales.front().mSale);

    aResult.mDiagnostics.push_back(
        numericDiagnostic(NumericError::Overflow,
                          {
                              .mStage = PipelineStage::FifoTax,
                              .mAffectedReports = {ReportType::CapitalGains},
                              .mSources = trade.mMetadata.mSources,
                              .mTaxDate = trade.mMetadata.mTaxDate,
                              .mInstrumentName = aIsin.mName,
                              .mIsin = aIsin.mIsin,
                          }));
}

/// Asks the ISIN's question, applies its recorded answer to the open sales, or, without an
/// answer and with OpenLossQuestions::AssumeBlocked, treats the open units as blocked.
void resolveOpenSales(LossEligibilityIsin& aIsin,
                      const std::vector<TradeInstrument>& aInstruments,
                      RunDecisions& aDecisions,
                      OpenLossQuestions aOpenQuestions,
                      LossEligibilityResult& aResult) {
    std::vector<std::size_t> openSales;

    for (std::size_t index = 0; index < aIsin.mSales.size(); ++index)
    {
        if (aIsin.mSales[index].mState == LossSaleState::WaitingForAnswer)
        {
            openSales.push_back(index);
        }
    }

    if (openSales.empty())
    {
        return;
    }

    auto question = questionOf(aIsin, openSales, aInstruments);
    const auto* answer = aDecisions.find(question);

    if (answer == nullptr && aOpenQuestions == OpenLossQuestions::AssumeBlocked)
    {
        for (const auto index : openSales)
        {
            auto& sale = aIsin.mSales[index];

            // As with the answer `other_activity`: the safe side, every open unit is blocked.
            sale.mDeclaredIneligibleUnits = sale.mLossUnits - sale.mKnownIneligibleUnits;
            sale.mAssumedBlocked = true;
            sale.mState = LossSaleState::Decided;
        }

        aResult.mDiagnostics.push_back(assumedBlockedDiagnostic(question, aIsin));

        return;
    }

    if (answer == nullptr)
    {
        RequestInput request{.mQuestion = std::move(question)};

        for (const auto index : openSales)
        {
            const auto& trade = tradeOf(aInstruments, aIsin.mSales[index].mSale);

            request.mProcessingDiagnostics.push_back(aResult.mDiagnostics.size());
            aResult.mDiagnostics.push_back(
                lossDiagnostic(MessageKey::LossEligibilityUnresolvedConfirmationRequired,
                               trade,
                               aIsin));
        }

        aResult.mRequests.push_back(std::move(request));

        return;
    }

    const auto apply = [&aIsin, &openSales, &aInstruments, &aResult](const auto& aAnswer) {
        using Answer = std::decay_t<decltype(aAnswer)>;

        for (const auto index : openSales)
        {
            auto& sale = aIsin.mSales[index];

            if constexpr (std::is_same_v<Answer, NoOtherActivityAnswer>)
            {
                sale.mState = LossSaleState::Decided;
            }
            else if constexpr (std::is_same_v<Answer, OtherActivityAnswer>)
            {
                // The safe side: every open unit is blocked; the user corrects it in eDavki.
                sale.mDeclaredIneligibleUnits = sale.mLossUnits - sale.mKnownIneligibleUnits;
                sale.mState = LossSaleState::Decided;
            }
            else if constexpr (std::is_same_v<Answer, CancelAnswer>)
            {
                sale.mState = LossSaleState::Cancelled;
                aResult.mDiagnostics.push_back(
                    lossDiagnostic(MessageKey::LossEligibilityUnresolvedCancelled,
                                   tradeOf(aInstruments, sale.mSale),
                                   aIsin));
            }
            else if constexpr (std::is_same_v<Answer, FailureResolutionAnswer>)
            {
                // The session accepts only a loss answer or a cancel for a loss question.
                throw std::logic_error("A failure choice cannot answer a loss question.");
            }
            else
            {
                static_assert(std::is_same_v<Answer, SplitRatioAnswer> ||
                              std::is_same_v<Answer, SeparateEventsAnswer>);
                throw std::logic_error("A corporate-action answer cannot answer a loss question.");
            }
        }
    };

    std::visit(apply, *answer);
}

} // namespace

LossEligibilityResult LossEligibilityChecker::check(const MergedStatement& aStatement,
                                                    const ReportingYearLedger& aLedger,
                                                    const FifoMatchingResult& aFifo,
                                                    RunDecisions& aDecisions,
                                                    OpenLossQuestions aOpenQuestions) {
    const auto& instruments = aStatement.mPresentation.mTradeInstruments;
    const auto histories = collectHistories(instruments, aLedger);
    LossEligibilityResult result;

    result.mIsins.reserve(instruments.size());

    // aFifo.mIsins follows the instruments, so an index is both.
    for (std::size_t index = 0; index < instruments.size(); ++index)
    {
        const auto& pool = aFifo.mIsins.at(index);
        LossEligibilityIsin isin{.mIsin = pool.mIsin, .mName = pool.mName};

        if (pool.mStatus == FifoIsinStatus::Matched &&
            mayReachKdvp(instruments.at(index).mAssetClass))
        {
            auto sales = checkIsin(histories.at(index), pool, aLedger);

            if (sales)
            {
                isin.mSales = std::move(*sales);
                resolveOpenSales(isin, instruments, aDecisions, aOpenQuestions, result);
            }
            else
            {
                isin.mSales = failedSales(histories.at(index), pool, aLedger);
                failIsin(isin, instruments, result);
            }
        }

        result.mIsins.push_back(std::move(isin));
    }

    return result;
}

} // namespace taxbroker
