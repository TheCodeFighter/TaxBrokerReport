#include "processors/capital_report_gates.hpp"
#include "taxbroker/currency.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace taxbroker {
namespace {

/// The sales of the selected year, by instrument index, in ledger order.
std::vector<std::vector<StatementEventReference>>
selectedYearSales(const std::vector<TradeInstrument>& aInstruments,
                  const ReportingYearLedger& aLedger) {
    std::vector<std::vector<StatementEventReference>> sales(aInstruments.size());

    for (std::size_t position = aLedger.mHistoricalCapitalEventCount;
         position < aLedger.mCapitalEvents.size();
         ++position)
    {
        const auto& event = aLedger.mCapitalEvents.at(position);

        if (event.mKind != StatementEventKind::Trade)
        {
            continue;
        }

        const auto instrumentIndex = event.mInstrumentIndex.value();
        const auto& trade = aInstruments.at(instrumentIndex).mTransactions.at(event.mEventIndex);

        if (trade.mTradeSide == TradeSide::Sell)
        {
            sales.at(instrumentIndex).push_back(event);
        }
    }

    return sales;
}

const TradeTransaction& tradeOf(const std::vector<TradeInstrument>& aInstruments,
                                const StatementEventReference& aEvent) {
    return aInstruments.at(aEvent.mInstrumentIndex.value()).mTransactions.at(aEvent.mEventIndex);
}

/// One diagnostic for the capital report, with the message rendered from its key.
ProcessingDiagnostic capitalDiagnostic(DiagnosticSeverity aSeverity,
                                       ProcessingDiagnosticCode aCode,
                                       PipelineStage aStage,
                                       DiagnosticMessage aMessage,
                                       std::vector<SourceReference> aSources,
                                       Date aTaxDate,
                                       std::optional<std::string> aName,
                                       std::optional<Isin> aIsin) {
    return ProcessingDiagnostic{
        .mSeverity = aSeverity,
        .mCode = aCode,
        .mContext =
            {
                .mStage = aStage,
                .mAffectedReports = {ReportType::CapitalGains},
                .mSources = std::move(aSources),
                .mTaxDate = aTaxDate,
                .mInstrumentName = std::move(aName),
                .mIsin = std::move(aIsin),
            },
        .mMessage = std::move(aMessage.mEnglish),
        .mMessageKey = aMessage.mKey,
        .mMessageParams = std::move(aMessage.mParams),
    };
}

/// The class name for a message parameter. Only a derivative and a private fund get a name; the
/// other classes never get here (checkClassification handles them first).
std::optional<std::string_view> unsupportedClassName(AssetClass aAssetClass) {
    switch (aAssetClass)
    {
    case AssetClass::Stock:
    case AssetClass::Fund:
    case AssetClass::Bond:
    case AssetClass::Crypto:
    case AssetClass::Unknown:
        return std::nullopt;
    case AssetClass::Derivative:
        return "derivative";
    case AssetClass::PrivateFund:
        return "private_fund";
    }

    return std::nullopt;
}

/// One error for an ISIN with a sale of the year whose class has no capital treatment: a
/// derivative or a private fund. The sources are those of all its sales of the year; the date is
/// the first sale's. A bond or crypto instrument must already be left out by the classification
/// stage, so one that reaches this gate is a logic error.
void checkClassification(const TradeInstrument& aInstrument,
                         const std::vector<StatementEventReference>& aYearSales,
                         const std::vector<TradeInstrument>& aInstruments,
                         std::vector<ProcessingDiagnostic>& aDiagnostics) {
    // Shares and fund units are reported; an unknown class is asked or blocked by the
    // classification stage, and an answered one is reported as capital gains.
    if (aInstrument.mAssetClass == AssetClass::Stock ||
        aInstrument.mAssetClass == AssetClass::Fund ||
        aInstrument.mAssetClass == AssetClass::Unknown)
    {
        return;
    }

    if (aInstrument.mAssetClass == AssetClass::Bond ||
        aInstrument.mAssetClass == AssetClass::Crypto)
    {
        throw std::logic_error("A bond or crypto instrument must be left out by the classification "
                               "stage.");
    }

    // Only a derivative and a private fund are left, so the name is always there.
    const auto className = unsupportedClassName(aInstrument.mAssetClass).value();

    std::vector<SourceReference> sources;

    for (const auto& sale : aYearSales)
    {
        const auto& saleSources = tradeOf(aInstruments, sale).mMetadata.mSources;
        sources.insert(sources.end(), saleSources.begin(), saleSources.end());
    }

    const auto firstSaleDate = tradeOf(aInstruments, aYearSales.front()).mMetadata.mTaxDate;

    aDiagnostics.push_back(
        capitalDiagnostic(DiagnosticSeverity::Error,
                          ProcessingDiagnosticCode::UnsupportedTaxTreatment,
                          PipelineStage::FifoTax,
                          makeDiagnosticMessage(MessageKey::UnsupportedTaxTreatmentCapitalClass,
                                                {textParam("assetClass", std::string{className})}),
                          std::move(sources),
                          firstSaleDate,
                          aInstrument.mName,
                          aInstrument.mIsin));
}

/// The error for a blocked ISIN whose pool stopped at a foreign trade: the error of its missing
/// rate (aRates), made for this investment. A trade without a currency has its own error. A
/// price that could not be converted already has its own error. A pool stopped for missing
/// history or a numeric failure already has its FIFO diagnostic, and one stopped at a corporate
/// action has its diagnostics from the corporate-action stage.
void checkStoppedPool(const FifoIsinResult& aPool,
                      const std::vector<TradeInstrument>& aInstruments,
                      const OfficialRateSelection& aRates,
                      std::vector<ProcessingDiagnostic>& aDiagnostics) {
    switch (aPool.mStatus)
    {
    case FifoIsinStatus::Matched:
    case FifoIsinStatus::InsufficientUnits:
    case FifoIsinStatus::NumericFailure:
    case FifoIsinStatus::CorporateAction:
        return;
    case FifoIsinStatus::NonEurTrade: {
        const auto& trade = tradeOf(aInstruments, aPool.mStoppedAt.value());

        if (trade.mCurrency == Currency::Unknown)
        {
            aDiagnostics.push_back(
                capitalDiagnostic(DiagnosticSeverity::Error,
                                  ProcessingDiagnosticCode::MissingExchangeRate,
                                  PipelineStage::FifoTax,
                                  makeDiagnosticMessage(MessageKey::MissingExchangeRateTrade, {}),
                                  trade.mMetadata.mSources,
                                  trade.mMetadata.mTaxDate,
                                  aPool.mName,
                                  aPool.mIsin));

            return;
        }

        const auto* missing = aRates.findMissing(trade.mCurrency, trade.mMetadata.mTaxDate);

        if (missing == nullptr)
        {
            return;
        }

        // The error of the currency and date, for this investment and this trade.
        auto diagnostic = *missing;

        diagnostic.mContext.mSources = trade.mMetadata.mSources;
        diagnostic.mContext.mInstrumentName = aPool.mName;
        diagnostic.mContext.mIsin = aPool.mIsin;
        aDiagnostics.push_back(std::move(diagnostic));

        return;
    }
    }
}

/// The first merger row of the ISIN on the date of this action row, if any. The corporate-action
/// stage asks the ratio of every other row after 31 December that a loss window needs, or gives
/// it its own error, so only a date with a merger is not asked there.
std::optional<StatementEventReference>
mergerOnDate(const std::vector<TradeInstrument>& aInstruments,
             const StatementEventReference& aEvent) {
    const auto& actions = aInstruments.at(aEvent.mInstrumentIndex.value()).mCorporateActions;
    const auto date = actions.at(aEvent.mEventIndex).mMetadata.mTaxDate;

    const auto merger =
        std::find_if(actions.begin(), actions.end(), [date](const CorporateAction& aAction) {
            return aAction.mType == CorporateActionType::Merger &&
                   aAction.mMetadata.mTaxDate == date;
        });

    if (merger == actions.end())
    {
        return std::nullopt;
    }

    return StatementEventReference{StatementEventKind::CorporateAction,
                                   aEvent.mInstrumentIndex,
                                   static_cast<std::size_t>(merger - actions.begin())};
}

/// Errors for loss sales that cannot be written yet: one per merger date in a loss window, and one
/// per sale whose blocked units exceed its loss units. Open and cancelled loss questions, a split
/// row in a loss window (its ratio question or error comes from the corporate-action stage) and a
/// numeric failure already have theirs.
void checkLossSales(const KdvpBlockedIsin& aBlocked,
                    const LossEligibilityIsin& aLoss,
                    const std::vector<TradeInstrument>& aInstruments,
                    std::vector<ProcessingDiagnostic>& aDiagnostics) {
    std::vector<StatementEventReference> reportedActions;

    for (const auto& sale : aBlocked.mSales)
    {
        // The builder blocks a sale for the loss rule only when the rule has a result for it.
        const auto loss = std::find_if(
            aLoss.mSales.begin(),
            aLoss.mSales.end(),
            [&sale](const LossSaleEligibility& aEntry) { return aEntry.mSale == sale; });

        if (loss == aLoss.mSales.end())
        {
            continue;
        }

        switch (loss->mState)
        {
        case LossSaleState::WaitingForAnswer:
        case LossSaleState::Cancelled:
        case LossSaleState::NumericFailure:
            break;
        case LossSaleState::Decided: {
            // The builder lists a decided sale only when the rule blocks more units than the
            // sale has loss units.
            const auto& trade = tradeOf(aInstruments, sale);

            aDiagnostics.push_back(capitalDiagnostic(
                DiagnosticSeverity::Error,
                ProcessingDiagnosticCode::LossEligibilityUnresolved,
                PipelineStage::FifoTax,
                makeDiagnosticMessage(MessageKey::LossEligibilityUnresolvedUnitsMismatch),
                trade.mMetadata.mSources,
                trade.mMetadata.mTaxDate,
                aBlocked.mName,
                aBlocked.mIsin));
            break;
        }
        case LossSaleState::CorporateActionInWindow: {
            const auto merger = mergerOnDate(aInstruments, loss->mCorporateAction.value());

            if (!merger || std::find(reportedActions.begin(), reportedActions.end(), *merger) !=
                               reportedActions.end())
            {
                break;
            }

            reportedActions.push_back(*merger);

            const auto& action = aInstruments.at(merger->mInstrumentIndex.value())
                                     .mCorporateActions.at(merger->mEventIndex);

            aDiagnostics.push_back(capitalDiagnostic(
                DiagnosticSeverity::Error,
                ProcessingDiagnosticCode::CorporateActionNotApplied,
                PipelineStage::CorporateAction,
                makeDiagnosticMessage(MessageKey::CorporateActionNotAppliedLossWindow),
                action.mMetadata.mSources,
                action.mMetadata.mTaxDate,
                aBlocked.mName,
                aBlocked.mIsin));
            break;
        }
        }
    }
}

/// The years joined with ", " in ascending order, such as "2023, 2024".
std::string yearsText(const std::set<int>& aYears) {
    std::string text;

    for (const auto year : aYears)
    {
        text += (text.empty() ? "" : ", ") + std::to_string(year);
    }

    return text;
}

/// One warning that lists the years of the rate files to load (or replace), with the sources of
/// the trades that need them. Nothing when there are no years.
void addRateFilesWarning(MessageKey aKey,
                         const std::set<int>& aYears,
                         const std::set<SourceReference, StableSourceOrder>& aSources,
                         std::vector<ProcessingDiagnostic>& aDiagnostics) {
    if (aYears.empty())
    {
        return;
    }

    auto message = makeDiagnosticMessage(aKey, {textParam("years", yearsText(aYears))});

    aDiagnostics.push_back(ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Warning,
        .mCode = ProcessingDiagnosticCode::OfficialRateFilesRequired,
        .mContext =
            {
                .mStage = PipelineStage::FifoTax,
                .mAffectedReports = {ReportType::CapitalGains},
                .mSources = {aSources.begin(), aSources.end()},
            },
        .mMessage = std::move(message.mEnglish),
        .mMessageKey = message.mKey,
        .mMessageParams = std::move(message.mParams),
    });
}

/// The years of the ECB rate files that the foreign trades of the investments sold in the year
/// need, whatever stopped the pool first: up to two warnings, the files that are not loaded and
/// those that end too early. A rate that no file can fix names no year.
void checkRateFiles(const std::vector<TradeInstrument>& aInstruments,
                    const ReportingYearLedger& aLedger,
                    const std::vector<std::vector<StatementEventReference>>& aYearSales,
                    const OfficialRateSelection& aRates,
                    std::vector<ProcessingDiagnostic>& aDiagnostics) {
    std::set<int> notLoaded;
    std::set<int> endsEarly;
    std::set<SourceReference, StableSourceOrder> notLoadedSources;
    std::set<SourceReference, StableSourceOrder> endsEarlySources;

    for (std::size_t index = 0; index < aInstruments.size(); ++index)
    {
        if (aYearSales.at(index).empty() || aLedger.isLeftOut(aInstruments.at(index).mIsin))
        {
            continue;
        }

        for (const auto& trade : aInstruments.at(index).mTransactions)
        {
            if (trade.mCurrency == Currency::EUR || trade.mCurrency == Currency::Unknown ||
                trade.mMetadata.mTaxDate > aLedger.mYearEnd)
            {
                continue;
            }

            const auto* need = aRates.findMissingFile(trade.mCurrency, trade.mMetadata.mTaxDate);

            if (need == nullptr)
            {
                continue;
            }

            const bool isNotLoaded = need->mProblem == RateFileProblem::NotLoaded;
            auto& years = isNotLoaded ? notLoaded : endsEarly;
            auto& sources = isNotLoaded ? notLoadedSources : endsEarlySources;

            years.insert(need->mYear);
            sources.insert(trade.mMetadata.mSources.begin(), trade.mMetadata.mSources.end());
        }
    }

    addRateFilesWarning(MessageKey::OfficialRateFilesRequiredNotLoaded,
                        notLoaded,
                        notLoadedSources,
                        aDiagnostics);
    addRateFilesWarning(MessageKey::OfficialRateFilesRequiredEndsEarly,
                        endsEarly,
                        endsEarlySources,
                        aDiagnostics);
}

/// One warning for each private-market event of the selected year, in chronological order. Such
/// an event has only a cash amount, no units or price, so it is skipped.
void warnAboutPrivateMarketEvents(const MergedStatement& aStatement,
                                  const ReportingYearLedger& aLedger,
                                  std::vector<ProcessingDiagnostic>& aDiagnostics) {
    for (const auto& reference : aStatement.mChronologicalOrder)
    {
        if (reference.mKind != StatementEventKind::PrivateMarket)
        {
            continue;
        }

        const auto& event = aStatement.mPresentation.mPrivateMarketEvents.at(reference.mEventIndex);
        const auto date = event.mMetadata.mTaxDate;

        if (date < aLedger.mYearStart || date > aLedger.mYearEnd)
        {
            continue;
        }

        aDiagnostics.push_back(capitalDiagnostic(
            DiagnosticSeverity::Warning,
            ProcessingDiagnosticCode::UnsupportedTaxTreatment,
            PipelineStage::FifoTax,
            makeDiagnosticMessage(MessageKey::UnsupportedTaxTreatmentPrivateMarketSkipped),
            event.mMetadata.mSources,
            date,
            event.mName.empty() ? std::nullopt : std::optional<std::string>{event.mName},
            event.mIsin));
    }
}

} // namespace

std::vector<ProcessingDiagnostic> CapitalReportGates::check(const MergedStatement& aStatement,
                                                            const ReportingYearLedger& aLedger,
                                                            const FifoMatchingResult& aFifo,
                                                            const LossEligibilityResult& aLoss,
                                                            const CapitalGainsBuildResult& aBuilt,
                                                            const OfficialRateSelection& aRates) {
    const auto& instruments = aStatement.mPresentation.mTradeInstruments;
    const auto yearSales = selectedYearSales(instruments, aLedger);
    std::vector<ProcessingDiagnostic> diagnostics;

    for (std::size_t index = 0; index < instruments.size(); ++index)
    {
        if (!yearSales.at(index).empty() && !aLedger.isLeftOut(instruments.at(index).mIsin))
        {
            checkClassification(instruments.at(index),
                                yearSales.at(index),
                                instruments,
                                diagnostics);
        }
    }

    for (const auto& blocked : aBuilt.mBlocked)
    {
        switch (blocked.mReason)
        {
        case KdvpBlockReason::FifoStopped: {
            // The builder blocks only ISINs of the FIFO result, so the pool is always there.
            const auto pool = std::find_if(
                aFifo.mIsins.begin(),
                aFifo.mIsins.end(),
                [&blocked](const FifoIsinResult& aPool) { return aPool.mIsin == blocked.mIsin; });

            if (pool != aFifo.mIsins.end())
            {
                checkStoppedPool(*pool, instruments, aRates, diagnostics);
            }

            break;
        }
        case KdvpBlockReason::LossNotReportable: {
            // The builder blocks only ISINs of the loss result, so the entry is always there.
            const auto loss = std::find_if(aLoss.mIsins.begin(),
                                           aLoss.mIsins.end(),
                                           [&blocked](const LossEligibilityIsin& aIsin) {
                                               return aIsin.mIsin == blocked.mIsin;
                                           });

            if (loss != aLoss.mIsins.end())
            {
                checkLossSales(blocked, *loss, instruments, diagnostics);
            }

            break;
        }
        case KdvpBlockReason::UnitValueTooLarge: {
            // The error points at the first sale that uses a unit value that does not fit F4.
            const auto& sale = tradeOf(instruments, blocked.mSales.front());

            diagnostics.push_back(capitalDiagnostic(
                DiagnosticSeverity::Error,
                ProcessingDiagnosticCode::CorporateActionUnrepresentable,
                PipelineStage::FifoTax,
                makeDiagnosticMessage(MessageKey::CorporateActionUnrepresentableUnitValue),
                sale.mMetadata.mSources,
                sale.mMetadata.mTaxDate,
                blocked.mName,
                blocked.mIsin));
            break;
        }
        }
    }

    checkRateFiles(instruments, aLedger, yearSales, aRates, diagnostics);
    warnAboutPrivateMarketEvents(aStatement, aLedger, diagnostics);

    return diagnostics;
}

} // namespace taxbroker
