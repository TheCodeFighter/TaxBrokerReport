#include "processors/trade_conversion.hpp"

#include "taxbroker/currency.hpp"

#include <map>
#include <string>
#include <utility>
#include <variant>

namespace taxbroker {
namespace {

/// Tells whether the trade is a foreign trade that needs a rate.
bool needsRate(const TradeInstrument& aInstrument,
               const TradeTransaction& aTrade,
               const ReportingYearLedger& aLedger) {
    return aTrade.mCurrency != Currency::EUR && aTrade.mCurrency != Currency::Unknown &&
           aTrade.mMetadata.mTaxDate <= aLedger.mYearEnd && !aLedger.isLeftOut(aInstrument.mIsin);
}

} // namespace

TradeConversionResult convertCapitalTrades(const MergedStatement& aStatement,
                                           const ReportingYearLedger& aLedger,
                                           const OfficialRateTable& aTable) {
    const auto& instruments = aStatement.mPresentation.mTradeInstruments;
    std::vector<RateNeed> needs;

    for (const auto& instrument : instruments)
    {
        for (const auto& trade : instrument.mTransactions)
        {
            if (!needsRate(instrument, trade, aLedger))
            {
                continue;
            }

            needs.push_back({.mCurrency = trade.mCurrency,
                             .mTaxDate = trade.mMetadata.mTaxDate,
                             .mUse = trade.mTradeSide == TradeSide::Buy ? RateUse::PurchasePrice
                                                                        : RateUse::SalePrice,
                             .mReport = ReportType::CapitalGains,
                             .mSources = trade.mMetadata.mSources});
        }
    }

    TradeConversionResult result;

    // Nothing to convert: no copy of the statement.
    if (needs.empty())
    {
        return result;
    }

    result.mRates = selectOfficialRates(needs, aTable);

    // The errors of the missing rates are given per investment later, so only warnings go on.
    for (const auto& diagnostic : result.mRates.mDiagnostics)
    {
        if (diagnostic.mSeverity == DiagnosticSeverity::Warning)
        {
            result.mDiagnostics.push_back(diagnostic);
        }
    }

    auto converted = aStatement;
    std::map<std::pair<Date, std::string>, UsedOfficialRate> used;

    for (std::size_t instrumentIndex = 0; instrumentIndex < instruments.size(); ++instrumentIndex)
    {
        const auto& instrument = instruments[instrumentIndex];

        for (std::size_t tradeIndex = 0; tradeIndex < instrument.mTransactions.size(); ++tradeIndex)
        {
            const auto& trade = instrument.mTransactions[tradeIndex];

            if (!needsRate(instrument, trade, aLedger))
            {
                continue;
            }

            const auto* rate = result.mRates.find(trade.mCurrency, trade.mMetadata.mTaxDate);

            if (rate == nullptr)
            {
                continue;
            }

            const auto eurPrice = convertOfficialUnitPrice(trade.mUnitPrice, *rate);

            if (const auto* error = std::get_if<NumericError>(&eurPrice))
            {
                result.mDiagnostics.push_back(
                    numericDiagnostic(*error,
                                      {.mStage = PipelineStage::FifoTax,
                                       .mAffectedReports = {ReportType::CapitalGains},
                                       .mSources = trade.mMetadata.mSources,
                                       .mTaxDate = trade.mMetadata.mTaxDate,
                                       .mField = "unit_price",
                                       .mInstrumentName = instrument.mName,
                                       .mIsin = instrument.mIsin}));

                continue;
            }

            auto& target = converted.mPresentation.mTradeInstruments.at(instrumentIndex)
                               .mTransactions.at(tradeIndex);

            target.mUnitPrice = std::get<UnitPrice>(eurPrice);
            target.mCurrency = Currency::EUR;

            result.mConverted.push_back({.mTrade = {.mKind = StatementEventKind::Trade,
                                                    .mInstrumentIndex = instrumentIndex,
                                                    .mEventIndex = tradeIndex},
                                         .mOriginalCurrency = trade.mCurrency,
                                         .mOriginalUnitPrice = trade.mUnitPrice,
                                         .mEurUnitPrice = target.mUnitPrice,
                                         .mRate = *rate});

            used.emplace(std::make_pair(trade.mMetadata.mTaxDate,
                                        std::string{currencyCode(trade.mCurrency)}),
                         UsedOfficialRate{.mCurrency = trade.mCurrency,
                                          .mTaxDate = trade.mMetadata.mTaxDate,
                                          .mRate = *rate});
        }
    }

    for (const auto& [key, entry] : used)
    {
        result.mUsedRates.push_back(entry);
    }

    if (!result.mConverted.empty())
    {
        result.mConvertedStatement = std::move(converted);
    }

    return result;
}

} // namespace taxbroker
