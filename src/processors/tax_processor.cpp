#include "processors/tax_processor.hpp"
#include "taxbroker/diagnostic_messages.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace taxbroker {
namespace {

/// Resolves an index into the owned, merger-produced presentation collections.
const EventMetadata& eventMetadata(const MergedStatement& aStatement,
                                   const StatementEventReference& aReference) {
    const auto& presentation = aStatement.mPresentation;

    switch (aReference.mKind)
    {
    case StatementEventKind::Trade:
        return presentation.mTradeInstruments.at(aReference.mInstrumentIndex.value())
            .mTransactions.at(aReference.mEventIndex)
            .mMetadata;
    case StatementEventKind::CorporateAction:
        return presentation.mTradeInstruments.at(aReference.mInstrumentIndex.value())
            .mCorporateActions.at(aReference.mEventIndex)
            .mMetadata;
    case StatementEventKind::Dividend:
        return presentation.mDividendInstruments.at(aReference.mInstrumentIndex.value())
            .mTransactions.at(aReference.mEventIndex)
            .mMetadata;
    case StatementEventKind::Interest:
        return presentation.mInterestInstruments.at(aReference.mInstrumentIndex.value())
            .mTransactions.at(aReference.mEventIndex)
            .mMetadata;
    case StatementEventKind::Benefit:
        return presentation.mBenefitEvents.at(aReference.mEventIndex).mMetadata;
    case StatementEventKind::PrivateMarket:
        return presentation.mPrivateMarketEvents.at(aReference.mEventIndex).mMetadata;
    }

    throw std::logic_error{"Unknown statement event kind"};
}

} // namespace

TaxProcessingResult TaxProcessor::prepareReportingYear(TaxProcessingRequest aRequest) const {
    TaxProcessingResult result{.mMerged = std::move(aRequest.mMerged)};

    if (!aRequest.mReportingYear || *aRequest.mReportingYear < kMinimumReportingYear ||
        *aRequest.mReportingYear > kMaximumReportingYear)
    {
        auto message = makeDiagnosticMessage(MessageKey::InvalidReportingYear,
                                             {numberParam("minYear", kMinimumReportingYear),
                                              numberParam("maxYear", kMaximumReportingYear)});

        result.mDiagnostics.push_back({
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::InvalidReportingYear,
            .mContext =
                {
                    .mStage = PipelineStage::HistoricalValidation,
                    .mAffectedReports = {ReportType::CapitalGains,
                                         ReportType::Dividends,
                                         ReportType::BankDepositInterest,
                                         ReportType::OtherInterest},
                    .mField = "reporting_year",
                },
            .mMessage = std::move(message.mEnglish),
            .mMessageKey = message.mKey,
            .mMessageParams = std::move(message.mParams),
        });

        return result;
    }

    const auto year = std::chrono::year{*aRequest.mReportingYear};
    const auto start = Date{std::chrono::sys_days{year / 1 / 1}.time_since_epoch()};
    const auto end = Date{std::chrono::sys_days{year / 12 / 31}.time_since_epoch()};
    ReportingYearLedger ledger{
        .mYear = *aRequest.mReportingYear,
        .mYearStart = start,
        .mYearEnd = end,
        .mLossWindowStart = start - DayDuration{30},
        .mLossWindowEnd = end + DayDuration{30},
    };

    for (const auto& reference : result.mMerged.mStatement.mChronologicalOrder)
    {
        const auto date = eventMetadata(result.mMerged.mStatement, reference).mTaxDate;
        const bool capital = reference.mKind == StatementEventKind::Trade ||
                             reference.mKind == StatementEventKind::CorporateAction;

        // Earlier capital events establish opening inventory for the selected year.
        if (capital && date <= end)
        {
            ledger.mCapitalEvents.push_back(reference);

            if (date < start)
            {
                ++ledger.mHistoricalCapitalEventCount;
            }
        }

        if (date >= start && date <= end)
        {
            ledger.mSelectedYearEvents.push_back(reference);

            if (reference.mKind == StatementEventKind::Trade)
            {
                const auto& trade = result.mMerged.mStatement.mPresentation.mTradeInstruments
                                        .at(reference.mInstrumentIndex.value())
                                        .mTransactions.at(reference.mEventIndex);

                ledger.mHasSelectedYearSales |= trade.mTradeSide == TradeSide::Sell;
            }
        }

        if (capital && date >= ledger.mLossWindowStart && date <= ledger.mLossWindowEnd)
        {
            ledger.mLossAnalysisEvents.push_back(reference);
        }
    }

    result.mLedger = std::move(ledger);

    return result;
}

} // namespace taxbroker
