#include "processors/dividend_payments.hpp"

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

constexpr std::string_view kSlovenia = "SI";

/// One dividend of the reporting year with its place in the statement.
struct Payment {
    StatementEventReference mReference;
    const DividendInstrument* mInstrument{};
    const DividendTransaction* mTransaction{};
};

/// What the payments of the year share when they have the same ISIN.
struct IsinPayments {
    std::string mName;
    std::vector<SourceReference> mSources;
    Date mFirstDate;
};

/// The payer of an ISIN as the rows will carry it.
struct PayerOutcome {
    IncomePayer mPayer; ///< Empty when the payer is not known.
    bool mIsSlovenian{};
};

/// One diagnostic of the dividend stage; it affects Doh-Div only.
ProcessingDiagnostic dividendDiagnostic(DiagnosticSeverity aSeverity,
                                        ProcessingDiagnosticCode aCode,
                                        DiagnosticMessage aMessage,
                                        std::vector<SourceReference> aSources,
                                        std::optional<Date> aTaxDate,
                                        std::optional<std::string> aName,
                                        std::optional<Isin> aIsin) {
    return ProcessingDiagnostic{
        .mSeverity = aSeverity,
        .mCode = aCode,
        .mContext =
            {
                .mStage = PipelineStage::FifoTax,
                .mAffectedReports = {ReportType::Dividends},
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

/// The payer facts of a reference row. A foreign payer's number is its identification number.
PayerOutcome outcomeOf(const PayerEntry& aEntry) {
    PayerOutcome outcome;

    outcome.mPayer = IncomePayer{.mName = aEntry.mName,
                                 .mAddress = aEntry.mAddress,
                                 .mCountry = aEntry.mCountry,
                                 .mTaxNumber = std::nullopt,
                                 .mIdentificationNumber = aEntry.mTaxNumber,
                                 .mCatalogueRow = aEntry.mSource};
    outcome.mIsSlovenian = aEntry.mCountry == kSlovenia;

    return outcome;
}

/// The warning that the payer has no facts: not found, or several rows and none picked.
void addPayerNotFound(DividendPaymentsResult& aResult,
                      const Isin& aIsin,
                      const IsinPayments& aPayments) {
    aResult.mDiagnostics.push_back(
        dividendDiagnostic(DiagnosticSeverity::Warning,
                           ProcessingDiagnosticCode::DividendPayerNotFound,
                           makeDiagnosticMessage(MessageKey::DividendPayerNotFound),
                           aPayments.mSources,
                           aPayments.mFirstDate,
                           aPayments.mName,
                           aIsin));
}

/// The reference rows of a conflict as the options of a payer choice question, sorted.
PayerChoiceQuestion
questionOf(const Isin& aIsin, const IsinPayments& aPayments, const PayerLookup& aLookup) {
    PayerChoiceQuestion question{.mIsin = aIsin, .mName = aPayments.mName, .mOptions = {}};

    question.mOptions.reserve(aLookup.mEntries.size());

    for (const auto* entry : aLookup.mEntries)
    {
        question.mOptions.push_back(PayerChoiceOption{.mName = entry->mName,
                                                      .mAddress = entry->mAddress,
                                                      .mCountry = entry->mCountry,
                                                      .mTaxNumber = entry->mTaxNumber,
                                                      .mSource = entry->mSource});
    }

    return std::get<PayerChoiceQuestion>(normalizedData(std::move(question)));
}

/// The payer of an ISIN whose reference rows disagree: the row the user picked, or no facts for a
/// skip or an open question (which is asked and warned about).
PayerOutcome askPayer(const Isin& aIsin,
                      const IsinPayments& aPayments,
                      const PayerLookup& aLookup,
                      RunDecisions& aDecisions,
                      DividendPaymentsResult& aResult) {
    auto data = questionOf(aIsin, aPayments, aLookup);
    DecisionQuestion question{
        .mType = RequestType::IncomeDetailsRequired,
        .mAffectedReports = {ReportType::Dividends},
        .mSources = aPayments.mSources,
        .mTaxDate = aPayments.mFirstDate,
        .mIsin = aIsin,
        .mData = data,
    };
    const auto* answer = aDecisions.find(question);

    if (answer == nullptr)
    {
        aResult.mRequests.push_back(
            RequestInput{.mQuestion = std::move(question),
                         .mProcessingDiagnostics = {aResult.mDiagnostics.size()}});
        addPayerNotFound(aResult, aIsin, aPayments);

        return {};
    }

    // The session accepts only a payer choice, and only a position among the options; std::get and
    // at() throw for anything else.
    const auto& choice = std::get<PayerChoiceAnswer>(*answer);

    if (!choice.mPickedOption)
    {
        addPayerNotFound(aResult, aIsin, aPayments);

        return {};
    }

    const auto& picked = data.mOptions.at(*choice.mPickedOption);
    const auto entry = std::find_if(
        aLookup.mEntries.begin(),
        aLookup.mEntries.end(),
        [&picked](const PayerEntry* aEntry) { return aEntry->mSource == picked.mSource; });
    const auto position = static_cast<std::size_t>(entry - aLookup.mEntries.begin());

    return outcomeOf(*aLookup.mEntries.at(position));
}

/// The payer of an ISIN from the catalogue and the user's answer. A payer that is not found, or
/// that the user skipped, has no facts.
PayerOutcome resolvePayer(const Isin& aIsin,
                          const IsinPayments& aPayments,
                          const ReferenceCatalogue& aCatalogue,
                          RunDecisions& aDecisions,
                          DividendPaymentsResult& aResult) {
    const auto lookup = aCatalogue.findPayer(aIsin);

    switch (lookup.mStatus)
    {
    case PayerLookupStatus::Found:
        return outcomeOf(*lookup.mEntries.front());
    case PayerLookupStatus::Conflict:
        return askPayer(aIsin, aPayments, lookup, aDecisions, aResult);
    case PayerLookupStatus::NotFound:
        break;
    }

    addPayerNotFound(aResult, aIsin, aPayments);

    return {};
}

/// The warning and the excluded item of the dividends of a Slovenian payer.
void addSlovenianExclusion(DividendPaymentsResult& aResult,
                           const Isin& aIsin,
                           const IsinPayments& aPayments) {
    const auto position = aResult.mDiagnostics.size();

    aResult.mDiagnostics.push_back(dividendDiagnostic(
        DiagnosticSeverity::Warning,
        ProcessingDiagnosticCode::TaxCategoryNotReported,
        makeDiagnosticMessage(MessageKey::TaxCategoryNotReportedSlovenianTaxSettled),
        aPayments.mSources,
        aPayments.mFirstDate,
        aPayments.mName,
        aIsin));
    aResult.mExcludedItems.push_back(ExcludedItemInput{
        .mItem = ExcludedItem{.mIsin = aIsin,
                              .mName = aPayments.mName,
                              .mCategory = ExcludedCategory::Dividend,
                              .mReason = ExcludedReason::SlovenianTaxSettled},
        .mSources = aPayments.mSources,
        .mProcessingDiagnostics = {position},
    });
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

        if (transaction.mCurrency != Currency::EUR)
        {
            needs.push_back({.mCurrency = transaction.mCurrency,
                             .mTaxDate = transaction.mMetadata.mTaxDate,
                             .mUse = RateUse::IncomeGross,
                             .mReport = ReportType::Dividends,
                             .mSources = transaction.mMetadata.mSources});
        }

        // A tax of 0 is 0 in every currency and needs no rate.
        if (transaction.mTaxPaid && *transaction.mTaxPaid != 0 &&
            transaction.mTaxCurrency != Currency::EUR)
        {
            needs.push_back({.mCurrency = transaction.mTaxCurrency,
                             .mTaxDate = transaction.mMetadata.mTaxDate,
                             .mUse = RateUse::ForeignTax,
                             .mReport = ReportType::Dividends,
                             .mSources = transaction.mMetadata.mSources});
        }
    }

    return needs;
}

/// Converts the amounts of the reported payments and collects the rows. Keeps the rates it used.
class RowBuilder {
  public:
    RowBuilder(const OfficialRateSelection& aRates, DividendPaymentsResult& aResult)
        : mRates{aRates}, mResult{aResult} {}

    /// The row of a payment, or none when an amount cannot be converted (the reason is already in
    /// the result or in the rate selection).
    std::optional<DivDividendRow> build(const Payment& aPayment, const PayerOutcome& aPayer) {
        const auto& transaction = *aPayment.mTransaction;
        const auto& metadata = transaction.mMetadata;
        const auto gross =
            toEur(aPayment, transaction.mGrossAmount, transaction.mCurrency, "amount");
        std::optional<Money> tax = Money{0};

        if (transaction.mTaxPaid && *transaction.mTaxPaid != 0)
        {
            tax = toEur(aPayment, *transaction.mTaxPaid, transaction.mTaxCurrency, "tax");
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

    /// The used rates, one per currency and day, by date, then code.
    std::vector<UsedOfficialRate> usedRates() const {
        std::vector<UsedOfficialRate> rates;

        rates.reserve(mUsed.size());

        for (const auto& [key, rate] : mUsed)
        {
            rates.push_back(rate);
        }

        return rates;
    }

  private:
    /// aAmount in EUR, or none when the rate is missing or the amount does not fit.
    std::optional<Money>
    toEur(const Payment& aPayment, Money aAmount, Currency aCurrency, const char* aField) {
        if (aCurrency == Currency::EUR)
        {
            return aAmount;
        }

        const auto& metadata = aPayment.mTransaction->mMetadata;
        const auto* rate = mRates.find(aCurrency, metadata.mTaxDate);

        // The selection holds the error of a pair without a rate.
        if (rate == nullptr)
        {
            return std::nullopt;
        }

        const auto converted = convertOfficialMoney(aAmount, *rate);

        if (const auto* error = std::get_if<NumericError>(&converted))
        {
            mResult.mDiagnostics.push_back(
                numericDiagnostic(*error,
                                  {.mStage = PipelineStage::FifoTax,
                                   .mAffectedReports = {ReportType::Dividends},
                                   .mSources = metadata.mSources,
                                   .mTaxDate = metadata.mTaxDate,
                                   .mField = aField,
                                   .mInstrumentName = aPayment.mInstrument->mName,
                                   .mIsin = aPayment.mInstrument->mIsin}));

            return std::nullopt;
        }

        mUsed.emplace(std::make_pair(metadata.mTaxDate, std::string{currencyCode(aCurrency)}),
                      UsedOfficialRate{.mCurrency = aCurrency,
                                       .mTaxDate = metadata.mTaxDate,
                                       .mRate = *rate});

        return std::get<Money>(converted);
    }

    const OfficialRateSelection& mRates;
    DividendPaymentsResult& mResult;
    std::map<std::pair<Date, std::string>, UsedOfficialRate> mUsed;
};

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
        const auto payer = resolvePayer(isin, group, aCatalogue, aDecisions, result);

        if (payer.mIsSlovenian)
        {
            addSlovenianExclusion(result, isin, group);
        }

        payers.emplace(isin, payer);
    }

    // The rate needs of the payers that are reported. The selection's warnings and its errors,
    // which block Doh-Div, go on as they are.
    const auto needs = needsOf(payments, payers);
    OfficialRateSelection selection;

    if (!needs.empty())
    {
        selection = selectOfficialRates(needs, aRates);
        result.mDiagnostics.insert(result.mDiagnostics.end(),
                                   selection.mDiagnostics.begin(),
                                   selection.mDiagnostics.end());
    }

    RowBuilder builder{selection, result};
    std::vector<SourceReference> taxSources;

    for (const auto& payment : payments)
    {
        const auto& payer = payers.at(payment.mInstrument->mIsin);

        if (payer.mIsSlovenian)
        {
            continue;
        }

        auto row = builder.build(payment, payer);

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
            dividendDiagnostic(DiagnosticSeverity::Warning,
                               ProcessingDiagnosticCode::ForeignTaxEvidenceRequired,
                               makeDiagnosticMessage(MessageKey::ForeignTaxEvidenceRequired),
                               std::move(taxSources),
                               std::nullopt,
                               std::nullopt,
                               std::nullopt));
    }

    result.mUsedRates = builder.usedRates();

    return result;
}

void appendDividendPayments(ApplicationResultInput& aInput, DividendPaymentsResult&& aResult) {
    const auto offset = aInput.mProcessingDiagnostics.size();

    for (auto& item : aResult.mExcludedItems)
    {
        std::transform(item.mProcessingDiagnostics.begin(),
                       item.mProcessingDiagnostics.end(),
                       item.mProcessingDiagnostics.begin(),
                       [offset](std::size_t aPosition) { return aPosition + offset; });

        aInput.mExcludedItems.push_back(std::move(item));
    }

    appendStageOutput(aInput, std::move(aResult.mDiagnostics), std::move(aResult.mRequests));
}

} // namespace taxbroker
