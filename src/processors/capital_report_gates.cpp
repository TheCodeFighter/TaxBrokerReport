#include "processors/capital_report_gates.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
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

/// The class name for a message parameter, or nothing for a class the report supports.
std::optional<std::string_view> unsupportedClassName(AssetClass aAssetClass) {
    switch (aAssetClass)
    {
    case AssetClass::Stock:
    case AssetClass::Fund:
    case AssetClass::Unknown:
        return std::nullopt;
    case AssetClass::Bond:
        return "bond";
    case AssetClass::Derivative:
        return "derivative";
    case AssetClass::Crypto:
        return "crypto";
    case AssetClass::PrivateFund:
        return "private_fund";
    }

    return std::nullopt;
}

std::string_view currencyCode(Currency aCurrency) {
    switch (aCurrency)
    {
    case Currency::EUR:
        return "EUR";
    case Currency::USD:
        return "USD";
    case Currency::GBP:
        return "GBP";
    case Currency::CHF:
        return "CHF";
    case Currency::JPY:
        return "JPY";
    case Currency::Unknown:
        break;
    }

    return "unknown";
}

/// One error for an ISIN with a sale of the year whose class is not Stock or Fund. The sources
/// are those of all its sales of the year; the date is the first sale's.
void checkClassification(const TradeInstrument& aInstrument,
                         const std::vector<StatementEventReference>& aYearSales,
                         const std::vector<TradeInstrument>& aInstruments,
                         std::vector<ProcessingDiagnostic>& aDiagnostics) {
    if (aInstrument.mAssetClass == AssetClass::Stock || aInstrument.mAssetClass == AssetClass::Fund)
    {
        return;
    }

    std::vector<SourceReference> sources;

    for (const auto& sale : aYearSales)
    {
        const auto& saleSources = tradeOf(aInstruments, sale).mMetadata.mSources;
        sources.insert(sources.end(), saleSources.begin(), saleSources.end());
    }

    const auto firstSaleDate = tradeOf(aInstruments, aYearSales.front()).mMetadata.mTaxDate;
    const auto className = unsupportedClassName(aInstrument.mAssetClass);

    if (className)
    {
        aDiagnostics.push_back(capitalDiagnostic(
            DiagnosticSeverity::Error,
            ProcessingDiagnosticCode::UnsupportedTaxTreatment,
            PipelineStage::FifoTax,
            makeDiagnosticMessage(MessageKey::UnsupportedTaxTreatmentCapitalClass,
                                  {textParam("assetClass", std::string{*className})}),
            std::move(sources),
            firstSaleDate,
            aInstrument.mName,
            aInstrument.mIsin));

        return;
    }

    aDiagnostics.push_back(
        capitalDiagnostic(DiagnosticSeverity::Error,
                          ProcessingDiagnosticCode::TaxClassificationUnknown,
                          PipelineStage::FifoTax,
                          makeDiagnosticMessage(MessageKey::TaxClassificationUnknownCapital),
                          std::move(sources),
                          firstSaleDate,
                          aInstrument.mName,
                          aInstrument.mIsin));
}

/// The error for a blocked ISIN whose pool stopped at a non-EUR trade. A pool stopped for missing
/// history or a numeric failure already has its FIFO diagnostic, and one stopped at a corporate
/// action has its diagnostics from the corporate-action stage.
void checkStoppedPool(const FifoIsinResult& aPool,
                      const std::vector<TradeInstrument>& aInstruments,
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

        aDiagnostics.push_back(capitalDiagnostic(
            DiagnosticSeverity::Error,
            ProcessingDiagnosticCode::MissingExchangeRate,
            PipelineStage::FifoTax,
            makeDiagnosticMessage(
                MessageKey::MissingExchangeRateTrade,
                {textParam("currency", std::string{currencyCode(trade.mCurrency)})}),
            trade.mMetadata.mSources,
            trade.mMetadata.mTaxDate,
            aPool.mName,
            aPool.mIsin));

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
                                                            const CapitalGainsBuildResult& aBuilt) {
    const auto& instruments = aStatement.mPresentation.mTradeInstruments;
    const auto yearSales = selectedYearSales(instruments, aLedger);
    std::vector<ProcessingDiagnostic> diagnostics;

    for (std::size_t index = 0; index < instruments.size(); ++index)
    {
        if (!yearSales.at(index).empty())
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
                checkStoppedPool(*pool, instruments, diagnostics);
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

    warnAboutPrivateMarketEvents(aStatement, aLedger, diagnostics);

    return diagnostics;
}

} // namespace taxbroker
