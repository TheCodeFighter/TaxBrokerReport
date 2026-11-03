#include "processors/quantity_reconciliation.hpp"

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace taxbroker {
namespace {

/// Most fractional digits of a source decimal that the exact comparison reads: even the largest
/// units (about 9.2e10) times 10^30 stay far inside 256 bits, also summed over every trade.
constexpr std::size_t kMaxSourceFractionDigits = 30;

/// The digits of an unsigned decimal without the point, and how many of them are fractional.
struct SourceDecimal {
    std::string mDigits;
    std::size_t mFractionDigits{};
};

/// Reads the canonical source decimal of a trade without its sign. Absent when it is not
/// `[sign]digits[.digits]` or has more fractional digits than kMaxSourceFractionDigits.
std::optional<SourceDecimal> sourceMagnitude(std::string_view aCanonical) {
    if (!aCanonical.empty() && (aCanonical.front() == '-' || aCanonical.front() == '+'))
    {
        aCanonical.remove_prefix(1);
    }

    const auto point = aCanonical.find('.');
    const auto whole = aCanonical.substr(0, point);
    const auto fraction =
        point == std::string_view::npos ? std::string_view{} : aCanonical.substr(point + 1);
    const bool wellFormed =
        !whole.empty() && (point == std::string_view::npos || !fraction.empty()) &&
        fraction.size() <= kMaxSourceFractionDigits &&
        std::all_of(whole.begin(),
                    whole.end(),
                    [](char aCharacter) { return aCharacter >= '0' && aCharacter <= '9'; }) &&
        std::all_of(fraction.begin(), fraction.end(), [](char aCharacter) {
            return aCharacter >= '0' && aCharacter <= '9';
        });

    if (!wellFormed)
    {
        return std::nullopt;
    }

    return SourceDecimal{std::string{whole} + std::string{fraction}, fraction.size()};
}

/// The decimal as a whole number of 10^-aFractionDigits. The digits are read one by one: a text
/// with a leading zero would be read as octal.
WideInteger scaled(const SourceDecimal& aDecimal, std::size_t aFractionDigits) {
    WideInteger value = std::accumulate(
        aDecimal.mDigits.begin(),
        aDecimal.mDigits.end(),
        WideInteger{0},
        [](const WideInteger& aSum, char aDigit) { return aSum * 10 + (aDigit - '0'); });

    for (std::size_t missing = aDecimal.mFractionDigits; missing < aFractionDigits; ++missing)
    {
        value *= 10;
    }

    return value;
}

/// Tells whether the exact source decimals prove that the shortage of the sale is a rounding
/// difference: the exact units of the earlier trades of the ISIN, in ledger order, cover the
/// exact units of the sale. Not so when a confirmed action changed the units, because the exact
/// source sum is in the units before it, or when a trade has no usable source evidence.
bool provenByDiscardedDigits(const TradeInstrument& aInstrument,
                             std::size_t aInstrumentIndex,
                             const ReportingYearLedger& aLedger,
                             const FifoIsinResult& aPool) {
    if (!aPool.mAppliedActions.empty())
    {
        return false;
    }

    std::vector<std::pair<SourceDecimal, TradeSide>> trades;

    for (const auto& event : aLedger.mCapitalEvents)
    {
        if (event.mKind != StatementEventKind::Trade || event.mInstrumentIndex != aInstrumentIndex)
        {
            continue;
        }

        const auto& trade = aInstrument.mTransactions.at(event.mEventIndex);

        if (trade.mUnitEvidence.empty())
        {
            return false;
        }

        auto decimal = sourceMagnitude(trade.mUnitEvidence.front().mCanonicalValue);

        if (!decimal)
        {
            return false;
        }

        trades.emplace_back(std::move(*decimal), trade.mTradeSide);

        // The sale is the last trade that counts.
        if (event == *aPool.mStoppedAt)
        {
            break;
        }
    }

    const std::size_t fractionDigits =
        std::accumulate(trades.begin(),
                        trades.end(),
                        std::size_t{0},
                        [](std::size_t aMost, const auto& aTrade) {
                            return std::max(aMost, aTrade.first.mFractionDigits);
                        });

    // The last trade is the sale itself: the position before it must cover it.
    WideInteger position = 0;

    for (std::size_t index = 0; index + 1 < trades.size(); ++index)
    {
        const auto units = scaled(trades[index].first, fractionDigits);

        position += trades[index].second == TradeSide::Buy ? units : -units;
    }

    return position >= scaled(trades.back().first, fractionDigits);
}

/// The incomplete_history error of a sale with a quantity question or a cancelled one.
ProcessingDiagnostic
correctionError(const FifoIsinResult& aPool, const TradeTransaction& aSale, MessageKey aKey) {
    auto diagnostic = incompleteHistoryDiagnostic(aPool, aSale);
    auto message = makeDiagnosticMessage(aKey);

    diagnostic.mMessage = std::move(message.mEnglish);
    diagnostic.mMessageKey = message.mKey;
    diagnostic.mMessageParams = std::move(message.mParams);

    return diagnostic;
}

/// Whether aSale is one of the corrections.
bool isCorrected(const StatementEventReference& aSale,
                 const std::vector<QuantityCorrection>& aCorrections) {
    return std::any_of(
        aCorrections.begin(),
        aCorrections.end(),
        [&aSale](const QuantityCorrection& aCorrection) { return aCorrection.mSale == aSale; });
}

} // namespace

std::optional<ExactRational> correctionUnitValue(const TradeTransaction& aSale,
                                                 const std::vector<FifoLot>& aLots) {
    if (aSale.mCurrency != Currency::EUR)
    {
        return std::nullopt;
    }

    const auto price =
        ExactRational::create(WideInteger{aSale.mUnitPrice}, WideInteger{UNIT_PRICE_SCALE});
    ExactRational highest = std::get<ExactRational>(price);

    for (const auto& lot : aLots)
    {
        // An open lot has units, so its value per unit exists.
        const auto value = std::get<ExactRational>(exactLotUnitValue(lot.mState));

        if (compareExact(value, highest) > 0)
        {
            highest = value;
        }
    }

    return highest;
}

MergedStatement withQuantityCorrections(MergedStatement aStatement,
                                        const std::vector<QuantityCorrection>& aCorrections) {
    for (const auto& correction : aCorrections)
    {
        aStatement.mPresentation.mTradeInstruments.at(correction.mSale.mInstrumentIndex.value())
            .mTransactions.at(correction.mSale.mEventIndex)
            .mUnits = correction.mCorrectedUnits;
    }

    return aStatement;
}

QuantityReconciliationResult
QuantityReconciler::reconcile(const MergedStatement& aStatement,
                              const ReportingYearLedger& aLedger,
                              FifoMatchingResult& aFifo,
                              const std::vector<QuantityCorrection>& aApplied,
                              RunDecisions& aDecisions) {
    QuantityReconciliationResult result;
    const auto& instruments = aStatement.mPresentation.mTradeInstruments;

    for (const auto& pool : aFifo.mIsins)
    {
        if (pool.mStatus != FifoIsinStatus::InsufficientUnits ||
            isCorrected(*pool.mStoppedAt, aApplied))
        {
            continue;
        }

        const auto& sale = *pool.mStoppedAt;
        const auto instrumentIndex = sale.mInstrumentIndex.value();
        const auto& instrument = instruments.at(instrumentIndex);

        if (instrument.mIsin != pool.mIsin)
        {
            throw std::logic_error("A FIFO pool stopped at a sale of another ISIN.");
        }

        const auto& trade = instrument.mTransactions.at(sale.mEventIndex);
        const Units available = pool.mAvailableUnits.value();
        const Units shortage = trade.mUnits - available;
        const auto unitValue = correctionUnitValue(trade, pool.mOpenLots);

        // Nothing open means no sale is left; a shortage of 0.00000100 or more is missing history.
        if (available <= 0 || shortage >= kReconciliationLimitUnits || !unitValue)
        {
            continue;
        }

        // The smallest possible correction removes only the shortage; a larger one costs more.
        const auto total =
            addExact(pool.mAdjustmentImpact, removedUnitsImpact(shortage, *unitValue));

        if (reachesAdjustmentBudget(std::get<ExactRational>(total)))
        {
            continue;
        }

        if (provenByDiscardedDigits(instrument, instrumentIndex, aLedger, pool))
        {
            result.mNewCorrections.push_back({.mSale = sale,
                                              .mOriginalUnits = trade.mUnits,
                                              .mCorrectedUnits = available,
                                              .mKind = QuantityCorrectionKind::RoundingEvidence,
                                              .mImpact = removedUnitsImpact(shortage, *unitValue)});

            continue;
        }

        const DecisionQuestion question{
            .mType = RequestType::QuantityCorrectionRequired,
            .mAffectedReports = {ReportType::CapitalGains},
            .mSources = trade.mMetadata.mSources,
            .mTaxDate = trade.mMetadata.mTaxDate,
            .mIsin = pool.mIsin,
            .mData = QuantityCorrectionQuestion{
                .mIsin = pool.mIsin,
                .mName = pool.mName,
                .mSaleDate = trade.mMetadata.mTaxDate,
                .mSaleTransaction = transactionIdentity(trade.mMetadata),
                .mOriginalUnits = trade.mUnits,
                .mOpenUnits = available,
                .mUnitValue = *unitValue,
                .mBudgetUsed = pool.mAdjustmentImpact,
            }};
        const auto* answer = aDecisions.find(question);

        if (answer != nullptr && !std::holds_alternative<CancelAnswer>(*answer))
        {
            const Units corrected = std::get<QuantityCorrectionAnswer>(*answer).mCorrectedUnits;

            result.mNewCorrections.push_back(
                {.mSale = sale,
                 .mOriginalUnits = trade.mUnits,
                 .mCorrectedUnits = corrected,
                 .mKind = QuantityCorrectionKind::UserConfirmed,
                 .mImpact = removedUnitsImpact(trade.mUnits - corrected, *unitValue)});

            continue;
        }

        // The error replaces the one of the FIFO run, which has no question.
        std::erase_if(aFifo.mDiagnostics, [&pool](const ProcessingDiagnostic& aDiagnostic) {
            return aDiagnostic.mCode == ProcessingDiagnosticCode::IncompleteHistory &&
                   aDiagnostic.mContext.mIsin == pool.mIsin;
        });

        if (answer == nullptr)
        {
            result.mRequests.push_back(
                RequestInput{.mQuestion = question,
                             .mProcessingDiagnostics = {result.mDiagnostics.size()}});
        }

        result.mDiagnostics.push_back(
            correctionError(pool,
                            trade,
                            answer == nullptr ? MessageKey::IncompleteHistoryCorrectionRequired
                                              : MessageKey::IncompleteHistoryCorrectionCancelled));
    }

    return result;
}

} // namespace taxbroker
