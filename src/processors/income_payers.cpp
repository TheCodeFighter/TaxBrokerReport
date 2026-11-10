#include "processors/income_payers.hpp"

#include "taxbroker/currency.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <variant>

namespace taxbroker {
namespace {

constexpr std::string_view kSlovenia = "SI";

/// Eight digits after the prefix `SI`, not all zero (the form's tax number is a positive integer),
/// the form of a Slovenian VAT identification number.
bool isSlovenianVatId(const std::string& aNumber) {
    constexpr std::size_t kDigits = 8;

    const auto isDigit = [](char aCharacter) { return aCharacter >= '0' && aCharacter <= '9'; };
    const auto isZero = [](char aCharacter) { return aCharacter == '0'; };
    const auto digits = aNumber.begin() + kSlovenia.size();

    return aNumber.size() == kSlovenia.size() + kDigits &&
           aNumber.compare(0, kSlovenia.size(), kSlovenia) == 0 &&
           std::all_of(digits, aNumber.end(), isDigit) &&
           !std::all_of(digits, aNumber.end(), isZero);
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
PayerOutcome askPayer(const PayerForm& aForm,
                      const PayerSinks& aSinks,
                      const Isin& aIsin,
                      const IsinPayments& aPayments,
                      const PayerLookup& aLookup,
                      RunDecisions& aDecisions) {
    auto data = questionOf(aIsin, aPayments, aLookup);
    DecisionQuestion question{
        .mType = RequestType::IncomeDetailsRequired,
        .mAffectedReports = {aForm.mReport},
        .mSources = aPayments.mSources,
        .mTaxDate = aPayments.mFirstDate,
        .mIsin = aIsin,
        .mData = data,
    };
    const auto* answer = aDecisions.find(question);

    if (answer == nullptr)
    {
        aSinks.mRequests.push_back(
            RequestInput{.mQuestion = std::move(question),
                         .mProcessingDiagnostics = {aSinks.mDiagnostics.size()}});
        addPayerNotFound(aForm, aSinks, aIsin, aPayments);

        return {};
    }

    // The session accepts only a payer choice, and only a position among the options; std::get and
    // at() throw for anything else.
    const auto& choice = std::get<PayerChoiceAnswer>(*answer);

    if (!choice.mPickedOption)
    {
        addPayerNotFound(aForm, aSinks, aIsin, aPayments);

        return {};
    }

    const auto& picked = data.mOptions.at(*choice.mPickedOption);
    const auto entry = std::find_if(
        aLookup.mEntries.begin(),
        aLookup.mEntries.end(),
        [&picked](const PayerEntry* aEntry) { return aEntry->mSource == picked.mSource; });
    const auto position = static_cast<std::size_t>(entry - aLookup.mEntries.begin());

    return payerOutcomeOf(*aLookup.mEntries.at(position));
}

/// Whether every reference row of the lookup has the residence country SI. Nothing found is not.
bool isSlovenianLookup(const PayerLookup& aLookup) {
    return !aLookup.mEntries.empty() &&
           std::all_of(aLookup.mEntries.begin(),
                       aLookup.mEntries.end(),
                       [](const PayerEntry* aEntry) { return aEntry->mCountry == kSlovenia; });
}

} // namespace

ProcessingDiagnostic incomeDiagnostic(ReportType aReport,
                                      DiagnosticSeverity aSeverity,
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
                .mAffectedReports = {aReport},
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

PayerOutcome payerOutcomeOf(const PayerEntry& aEntry) {
    PayerOutcome outcome;
    std::optional<std::string> taxNumber;
    std::optional<std::string> identificationNumber = aEntry.mTaxNumber;

    // A Slovenian payer's reference number is `SI` and its eight digits: the forms want the eight
    // digits as the tax number, and no other identification number.
    if (aEntry.mCountry == kSlovenia && aEntry.mTaxNumber && isSlovenianVatId(*aEntry.mTaxNumber))
    {
        taxNumber = aEntry.mTaxNumber->substr(kSlovenia.size());
        identificationNumber = std::nullopt;
    }

    outcome.mPayer = IncomePayer{.mName = aEntry.mName,
                                 .mAddress = aEntry.mAddress,
                                 .mCountry = aEntry.mCountry,
                                 .mTaxNumber = taxNumber,
                                 .mIdentificationNumber = identificationNumber,
                                 .mCatalogueRow = aEntry.mSource};
    outcome.mIsSlovenian = aEntry.mCountry == kSlovenia;

    return outcome;
}

PayerOutcome payerOutcomeOf(const BrokerPayerEntry& aEntry) {
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

void addPayerNotFound(const PayerForm& aForm,
                      const PayerSinks& aSinks,
                      const Isin& aIsin,
                      const IsinPayments& aPayments) {
    aSinks.mDiagnostics.push_back(incomeDiagnostic(aForm.mReport,
                                                   DiagnosticSeverity::Warning,
                                                   aForm.mNotFoundCode,
                                                   makeDiagnosticMessage(aForm.mNotFoundKey),
                                                   aPayments.mSources,
                                                   aPayments.mFirstDate,
                                                   aPayments.mName,
                                                   aIsin));
}

PayerOutcome resolvePayer(const PayerForm& aForm,
                          const PayerSinks& aSinks,
                          const Isin& aIsin,
                          const IsinPayments& aPayments,
                          const ReferenceCatalogue& aCatalogue,
                          RunDecisions& aDecisions) {
    const auto lookup = aCatalogue.findPayer(aIsin);
    PayerOutcome outcome;

    switch (lookup.mStatus)
    {
    case PayerLookupStatus::Found:
        outcome = payerOutcomeOf(*lookup.mEntries.front());
        break;
    case PayerLookupStatus::Conflict:
        // Every row says SI: a Slovenian company settles the tax itself, whichever row is meant.
        if (aForm.mSlovenianExclusion && isSlovenianLookup(lookup))
        {
            return PayerOutcome{.mPayer = {}, .mIsSlovenian = true};
        }

        outcome = askPayer(aForm, aSinks, aIsin, aPayments, lookup, aDecisions);
        break;
    case PayerLookupStatus::NotFound:
        addPayerNotFound(aForm, aSinks, aIsin, aPayments);
        break;
    }

    // A form that reports a Slovenian payer's income too treats the payer as any other.
    outcome.mIsSlovenian = outcome.mIsSlovenian && aForm.mSlovenianExclusion.has_value();

    return outcome;
}

void addSlovenianExclusion(const PayerForm& aForm,
                           const PayerSinks& aSinks,
                           const Isin& aIsin,
                           const IsinPayments& aPayments) {
    // A tax rule leaves the payer out on purpose: an item for the summary, no warning.
    aSinks.mExcludedItems.push_back(ExcludedItemInput{
        .mItem = ExcludedItem{.mIsin = aIsin,
                              .mName = aPayments.mName,
                              .mCategory = aForm.mSlovenianExclusion.value(),
                              .mReason = ExcludedReason::SlovenianTaxSettled},
        .mSources = aPayments.mSources,
        .mProcessingDiagnostics = {},
    });
}

void addIncomeRateNeeds(std::vector<RateNeed>& aNeeds,
                        ReportType aReport,
                        const EventMetadata& aMetadata,
                        Currency aGrossCurrency,
                        const std::optional<Money>& aTax,
                        Currency aTaxCurrency) {
    if (aGrossCurrency != Currency::EUR)
    {
        aNeeds.push_back({.mCurrency = aGrossCurrency,
                          .mTaxDate = aMetadata.mTaxDate,
                          .mUse = RateUse::IncomeGross,
                          .mReport = aReport,
                          .mSources = aMetadata.mSources});
    }

    // A tax of 0 is 0 in every currency and needs no rate.
    if (aTax && *aTax != 0 && aTaxCurrency != Currency::EUR)
    {
        aNeeds.push_back({.mCurrency = aTaxCurrency,
                          .mTaxDate = aMetadata.mTaxDate,
                          .mUse = RateUse::ForeignTax,
                          .mReport = aReport,
                          .mSources = aMetadata.mSources});
    }
}

IncomeEurConverter::IncomeEurConverter(const OfficialRateSelection& aRates,
                                       std::vector<ProcessingDiagnostic>& aDiagnostics)
    : mRates{aRates}, mDiagnostics{aDiagnostics} {}

std::optional<Money> IncomeEurConverter::toEur(ReportType aReport,
                                               const EventMetadata& aMetadata,
                                               const std::string& aName,
                                               const std::optional<Isin>& aIsin,
                                               Money aAmount,
                                               Currency aCurrency,
                                               const char* aField) {
    if (aCurrency == Currency::EUR)
    {
        return aAmount;
    }

    const auto* rate = mRates.find(aCurrency, aMetadata.mTaxDate);

    // The selection holds the error of a pair without a rate.
    if (rate == nullptr)
    {
        return std::nullopt;
    }

    const auto converted = convertOfficialMoney(aAmount, *rate);

    if (const auto* error = std::get_if<NumericError>(&converted))
    {
        mDiagnostics.push_back(numericDiagnostic(*error,
                                                 {.mStage = PipelineStage::FifoTax,
                                                  .mAffectedReports = {aReport},
                                                  .mSources = aMetadata.mSources,
                                                  .mTaxDate = aMetadata.mTaxDate,
                                                  .mField = aField,
                                                  .mInstrumentName = aName,
                                                  .mIsin = aIsin}));

        return std::nullopt;
    }

    mUsed.emplace(
        std::make_pair(aMetadata.mTaxDate, std::string{currencyCode(aCurrency)}),
        UsedOfficialRate{.mCurrency = aCurrency, .mTaxDate = aMetadata.mTaxDate, .mRate = *rate});

    return std::get<Money>(converted);
}

std::vector<UsedOfficialRate> IncomeEurConverter::usedRates() const {
    std::vector<UsedOfficialRate> rates;

    rates.reserve(mUsed.size());

    for (const auto& [key, rate] : mUsed)
    {
        rates.push_back(rate);
    }

    return rates;
}

void appendExcludedItems(ApplicationResultInput& aInput,
                         std::vector<ExcludedItemInput>&& aItems,
                         std::size_t aDiagnosticOffset) {
    for (auto& item : aItems)
    {
        std::transform(
            item.mProcessingDiagnostics.begin(),
            item.mProcessingDiagnostics.end(),
            item.mProcessingDiagnostics.begin(),
            [aDiagnosticOffset](std::size_t aPosition) { return aPosition + aDiagnosticOffset; });

        aInput.mExcludedItems.push_back(std::move(item));
    }
}

bool addPaymentMissingRateErrors(const OfficialRateSelection& aRates,
                                 ReportType aReport,
                                 const EventMetadata& aMetadata,
                                 const std::string& aName,
                                 const std::optional<Isin>& aIsin,
                                 Currency aGrossCurrency,
                                 const std::optional<Money>& aTax,
                                 Currency aTaxCurrency,
                                 std::vector<ProcessingDiagnostic>& aDiagnostics) {
    std::vector<Currency> currencies{aGrossCurrency};

    // A tax of 0 is 0 in every currency and needs no rate.
    if (aTax && *aTax != 0 && aTaxCurrency != aGrossCurrency)
    {
        currencies.push_back(aTaxCurrency);
    }

    bool missing = false;

    for (const auto currency : currencies)
    {
        if (currency == Currency::EUR || aRates.find(currency, aMetadata.mTaxDate) != nullptr)
        {
            continue;
        }

        const auto* error = aRates.findMissing(currency, aMetadata.mTaxDate);

        if (error == nullptr)
        {
            throw std::logic_error("A payment without a rate needs the error of its pair.");
        }

        auto diagnostic = *error;

        diagnostic.mContext.mAffectedReports = {aReport};
        diagnostic.mContext.mSources = aMetadata.mSources;
        diagnostic.mContext.mInstrumentName = aName;
        diagnostic.mContext.mIsin = aIsin;
        aDiagnostics.push_back(std::move(diagnostic));
        missing = true;
    }

    return missing;
}

} // namespace taxbroker
