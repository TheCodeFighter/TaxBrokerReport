#include "processors/dividend_payments.hpp"

#include "processors/income_payers.hpp"
#include "taxbroker/currency.hpp"
#include "taxbroker/diagnostic_messages.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace taxbroker {
namespace {

/// The dividend form's share of the payer helpers.
const PayerForm kForm{
    .mReport = ReportType::Dividends,
    .mNotFoundCode = ProcessingDiagnosticCode::DividendPayerNotFound,
    .mNotFoundKey = MessageKey::DividendPayerNotFound,
    .mSlovenianExclusion = ExcludedCategory::Dividend,
};

/// One dividend of the reporting year with its place in the statement.
struct Payment {
    StatementEventReference mReference;
    const DividendInstrument* mInstrument{};
    const DividendTransaction* mTransaction{};
};

/// The dividends dated inside the reporting year, in ledger order.
std::vector<Payment> paymentsOf(const MergedStatement& aStatement,
                                const ReportingYearLedger& aLedger) {
    const auto& instruments = aStatement.mPresentation.mDividendInstruments;
    std::vector<Payment> payments;

    for (const auto& reference : aStatement.mChronologicalOrder)
    {
        if (reference.mKind != StatementEventKind::Dividend)
        {
            continue;
        }

        const auto& instrument = instruments.at(reference.mInstrumentIndex.value());
        const auto& transaction = instrument.mTransactions.at(reference.mEventIndex);
        const auto date = transaction.mMetadata.mTaxDate;

        if (date < aLedger.mYearStart || date > aLedger.mYearEnd)
        {
            continue;
        }

        payments.push_back(
            {.mReference = reference, .mInstrument = &instrument, .mTransaction = &transaction});
    }

    return payments;
}

/// The name, sources and first date of the year's payments of each ISIN.
std::map<Isin, IsinPayments> groupByIsin(const std::vector<Payment>& aPayments) {
    std::map<Isin, IsinPayments> groups;

    for (const auto& payment : aPayments)
    {
        const auto& metadata = payment.mTransaction->mMetadata;
        auto [position, inserted] = groups.try_emplace(payment.mInstrument->mIsin);
        auto& group = position->second;

        if (inserted)
        {
            group.mName = payment.mInstrument->mName;
            group.mFirstDate = metadata.mTaxDate;
        }

        group.mSources.insert(group.mSources.end(),
                              metadata.mSources.begin(),
                              metadata.mSources.end());
        group.mFirstDate = std::min(group.mFirstDate, metadata.mTaxDate);
    }

    return groups;
}

/// The rate needs of the payments of the payers that are reported.
std::vector<RateNeed> needsOf(const std::vector<Payment>& aPayments,
                              const std::map<Isin, PayerOutcome>& aPayers) {
    std::vector<RateNeed> needs;

    for (const auto& payment : aPayments)
    {
        const auto& transaction = *payment.mTransaction;

        if (aPayers.at(payment.mInstrument->mIsin).mIsSlovenian)
        {
            continue;
        }

        addIncomeRateNeeds(needs,
                           ReportType::Dividends,
                           transaction.mMetadata,
                           transaction.mCurrency,
                           transaction.mTaxPaid,
                           transaction.mTaxCurrency);
    }

    return needs;
}

/// The row of a payment, or none when an amount cannot be converted: a missing rate or an amount
/// that does not fit gives an error of this payment and its ISIN.
std::optional<DivDividendRow> buildRow(IncomeEurConverter& aConverter,
                                       const OfficialRateSelection& aRates,
                                       std::vector<ProcessingDiagnostic>& aDiagnostics,
                                       const Payment& aPayment,
                                       const PayerOutcome& aPayer) {
    const auto& transaction = *aPayment.mTransaction;
    const auto& metadata = transaction.mMetadata;
    const auto& name = aPayment.mInstrument->mName;
    const std::optional<Isin> isin = aPayment.mInstrument->mIsin;

    if (addPaymentMissingRateErrors(aRates,
                                    ReportType::Dividends,
                                    metadata,
                                    name,
                                    isin,
                                    transaction.mCurrency,
                                    transaction.mTaxPaid,
                                    transaction.mTaxCurrency,
                                    aDiagnostics))
    {
        return std::nullopt;
    }

    const auto gross = aConverter.toEur(ReportType::Dividends,
                                        metadata,
                                        name,
                                        isin,
                                        transaction.mGrossAmount,
                                        transaction.mCurrency,
                                        "amount");
    std::optional<Money> tax = Money{0};

    if (transaction.mTaxPaid && *transaction.mTaxPaid != 0)
    {
        tax = aConverter.toEur(ReportType::Dividends,
                               metadata,
                               name,
                               isin,
                               *transaction.mTaxPaid,
                               transaction.mTaxCurrency,
                               "tax");
    }

    if (!gross || !tax)
    {
        return std::nullopt;
    }

    return DivDividendRow{
        .mPayment = aPayment.mReference,
        .mIsin = aPayment.mInstrument->mIsin,
        .mDate = metadata.mTaxDate,
        .mPayer = aPayer.mPayer,
        .mKind = DividendKind::Ordinary,
        .mGrossIncome = *gross,
        .mWithholding = {.mForeignTax = *tax, .mBlankInExport = !transaction.mTaxPaid},
        .mSourceCountry = aPayer.mPayer.mCountry,
        .mTreatyClaim = std::nullopt,
    };
}

} // namespace

DividendPaymentsResult prepareDividendPayments(const MergedStatement& aStatement,
                                               const ReportingYearLedger& aLedger,
                                               const ReferenceCatalogue& aCatalogue,
                                               const OfficialRateTable& aRates,
                                               RunDecisions& aDecisions) {
    DividendPaymentsResult result;
    const auto payments = paymentsOf(aStatement, aLedger);

    if (payments.empty())
    {
        return result;
    }

    // One payer per ISIN, in ISIN order.
    const auto groups = groupByIsin(payments);
    std::map<Isin, PayerOutcome> payers;

    for (const auto& [isin, group] : groups)
    {
        const PayerSinks sinks{result.mDiagnostics, result.mRequests, result.mExcludedItems};
        const auto payer = resolvePayer(kForm, sinks, isin, group, aCatalogue, aDecisions);

        if (payer.mIsSlovenian)
        {
            addSlovenianExclusion(kForm, sinks, isin, group);
        }

        payers.emplace(isin, payer);
    }

    // The rate needs of the payers that are reported. Only the selection's warnings go on; the
    // error of a missing rate is made for each payment, with its ISIN.
    const auto needs = needsOf(payments, payers);
    OfficialRateSelection selection;

    if (!needs.empty())
    {
        selection = selectOfficialRates(needs, aRates);

        for (const auto& diagnostic : selection.mDiagnostics)
        {
            if (diagnostic.mSeverity == DiagnosticSeverity::Warning)
            {
                result.mDiagnostics.push_back(diagnostic);
            }
        }
    }

    IncomeEurConverter converter{selection, result.mDiagnostics};
    std::vector<SourceReference> taxSources;

    for (const auto& payment : payments)
    {
        const auto& payer = payers.at(payment.mInstrument->mIsin);

        if (payer.mIsSlovenian)
        {
            continue;
        }

        auto row = buildRow(converter, selection, result.mDiagnostics, payment, payer);

        if (!row)
        {
            continue;
        }

        const auto& metadata = payment.mTransaction->mMetadata;

        if (row->mWithholding.mForeignTax > 0)
        {
            taxSources.insert(taxSources.end(), metadata.mSources.begin(), metadata.mSources.end());
        }

        result.mReport.mRows.push_back(std::move(*row));
    }

    // One notice for the run: the user attaches the evidence of the tax in eDavki.
    if (!taxSources.empty())
    {
        result.mDiagnostics.push_back(
            incomeDiagnostic(ReportType::Dividends,
                             DiagnosticSeverity::Warning,
                             ProcessingDiagnosticCode::ForeignTaxEvidenceRequired,
                             makeDiagnosticMessage(MessageKey::ForeignTaxEvidenceRequired),
                             std::move(taxSources),
                             std::nullopt,
                             std::nullopt,
                             std::nullopt));
    }

    result.mUsedRates = converter.usedRates();

    return result;
}

void excludeDividends(DividendReportDiv& aReport, const std::vector<Isin>& aExcluded) {
    std::erase_if(aReport.mRows, [&aExcluded](const DivDividendRow& aRow) {
        return std::find(aExcluded.begin(), aExcluded.end(), aRow.mIsin) != aExcluded.end();
    });
}

void appendDividendPayments(ApplicationResultInput& aInput, DividendPaymentsResult&& aResult) {
    appendExcludedItems(aInput,
                        std::move(aResult.mExcludedItems),
                        aInput.mProcessingDiagnostics.size());
    appendStageOutput(aInput, std::move(aResult.mDiagnostics), std::move(aResult.mRequests));
}

} // namespace taxbroker
