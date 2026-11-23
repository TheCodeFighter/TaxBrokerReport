#include "processors/income_payers.hpp"

#include "taxbroker/currency.hpp"
#include "utils/country_codes.hpp"
#include "utils/tax_number.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace taxbroker {
namespace {

constexpr std::string_view kSlovenia = "SI";
constexpr std::size_t kCountryLength = 2;

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

/// The reference row the user picked among rows that disagree, or null for a skip or an open
/// question (which is asked and warned about, with aCountry as the country that stays).
const PayerEntry* askPayer(const PayerForm& aForm,
                           const PayerSinks& aSinks,
                           const Isin& aIsin,
                           const IsinPayments& aPayments,
                           const PayerLookup& aLookup,
                           RunDecisions& aDecisions,
                           const std::optional<std::string>& aCountry) {
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
        addPayerNotFound(aForm, aSinks, aIsin, aPayments, aCountry);

        return nullptr;
    }

    // The session accepts only a payer choice, and only a position among the options; std::get and
    // at() throw for anything else.
    const auto& choice = std::get<PayerChoiceAnswer>(*answer);

    if (!choice.mPickedOption)
    {
        addPayerNotFound(aForm, aSinks, aIsin, aPayments, aCountry);

        return nullptr;
    }

    const auto& picked = data.mOptions.at(*choice.mPickedOption);
    const auto entry = std::find_if(
        aLookup.mEntries.begin(),
        aLookup.mEntries.end(),
        [&picked](const PayerEntry* aEntry) { return aEntry->mSource == picked.mSource; });
    const auto position = static_cast<std::size_t>(entry - aLookup.mEntries.begin());

    return aLookup.mEntries.at(position);
}

/// Whether every reference row of the lookup has the residence country SI. Nothing found is not.
bool isSlovenianLookup(const PayerLookup& aLookup) {
    return !aLookup.mEntries.empty() &&
           std::all_of(aLookup.mEntries.begin(),
                       aLookup.mEntries.end(),
                       [](const PayerEntry* aEntry) { return aEntry->mCountry == kSlovenia; });
}

/// The country of an income row and where it came from.
struct CountryChoice {
    std::optional<std::string> mCountry;
    bool mFromIsin{};
};

/// The first of: the export country, the country of the reference row in use, the one country of
/// all rows of the ISIN, the ISIN prefix when it is a country.
CountryChoice chooseCountry(const std::optional<std::string>& aExportCountry,
                            const PayerEntry* aUsed,
                            const PayerLookup& aLookup,
                            const Isin& aIsin) {
    if (aExportCountry)
    {
        return {.mCountry = aExportCountry, .mFromIsin = false};
    }

    if (aUsed != nullptr)
    {
        return {.mCountry = aUsed->mCountry, .mFromIsin = false};
    }

    const auto& entries = aLookup.mEntries;
    const auto sameAsFirst = [&entries](const PayerEntry* aEntry) {
        return aEntry->mCountry == entries.front()->mCountry;
    };

    if (!entries.empty() && std::all_of(entries.begin(), entries.end(), sameAsFirst))
    {
        return {.mCountry = entries.front()->mCountry, .mFromIsin = false};
    }

    // Rows that disagree on the country give no country; the ISIN prefix is the last step.
    const auto prefix = aIsin.substr(0, kCountryLength);

    if (isIsoCountryCode(prefix))
    {
        return {.mCountry = prefix, .mFromIsin = true};
    }

    return {};
}

/// The warning that the country of the rows of an ISIN is the first two letters of the ISIN.
ProcessingDiagnostic countryFromIsinWarning(const PayerForm& aForm,
                                            const IsinPayments& aPayments,
                                            const Isin& aIsin,
                                            const std::string& aCountry) {
    return incomeDiagnostic(
        aForm.mReport,
        DiagnosticSeverity::Warning,
        ProcessingDiagnosticCode::PayerCountryFromIsin,
        makeDiagnosticMessage(MessageKey::PayerCountryFromIsin, {textParam("country", aCountry)}),
        aPayments.mSources,
        aPayments.mFirstDate,
        aPayments.mName,
        aIsin);
}

/// The payer facts for a number from a reference row. A Slovenian payer's number is its tax number
/// when it is valid, otherwise nothing, and it never has an identification number (the form
/// rejects one). Any other payer's number is its identification number.
PayerOutcome payerOutcomeOfNumber(const std::string& aName,
                                  const std::string& aAddress,
                                  const std::string& aCountry,
                                  const std::optional<std::string>& aNumber,
                                  const CatalogueSource& aSource) {
    PayerOutcome outcome;
    const bool isSlovenian = aCountry == kSlovenia;
    std::optional<std::string> taxNumber;
    std::optional<std::string> identificationNumber = aNumber;

    if (isSlovenian)
    {
        if (aNumber && isSlovenianTaxNumber(*aNumber))
        {
            taxNumber = aNumber;
        }

        identificationNumber = std::nullopt;
        outcome.mSlovenianTaxNumberMissing = !taxNumber;
    }

    outcome.mPayer = IncomePayer{.mName = aName,
                                 .mAddress = aAddress,
                                 .mCountry = aCountry,
                                 .mTaxNumber = taxNumber,
                                 .mIdentificationNumber = identificationNumber,
                                 .mCatalogueRow = aSource};
    outcome.mIsSlovenian = isSlovenian;
    outcome.mIdentificationNumberMissing = !isSlovenian && !identificationNumber;

    return outcome;
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
    return payerOutcomeOfNumber(aEntry.mName,
                                aEntry.mAddress,
                                aEntry.mCountry,
                                aEntry.mTaxNumber,
                                aEntry.mSource);
}

PayerOutcome payerOutcomeOf(const BrokerPayerEntry& aEntry) {
    return payerOutcomeOfNumber(aEntry.mName,
                                aEntry.mAddress,
                                aEntry.mCountry,
                                aEntry.mTaxNumber,
                                aEntry.mSource);
}

ProcessingDiagnostic slovenianTaxNumberMissingWarning(ReportType aReport,
                                                      std::vector<SourceReference> aSources,
                                                      std::optional<Date> aTaxDate,
                                                      std::optional<std::string> aName,
                                                      std::optional<Isin> aIsin) {
    return incomeDiagnostic(aReport,
                            DiagnosticSeverity::Warning,
                            ProcessingDiagnosticCode::SlovenianPayerTaxNumberMissing,
                            makeDiagnosticMessage(MessageKey::SlovenianPayerTaxNumberMissing),
                            std::move(aSources),
                            aTaxDate,
                            std::move(aName),
                            std::move(aIsin));
}

ProcessingDiagnostic payerNumberMissingWarning(ReportType aReport,
                                               std::vector<SourceReference> aSources,
                                               std::optional<Date> aTaxDate,
                                               std::optional<std::string> aName,
                                               std::optional<Isin> aIsin) {
    return incomeDiagnostic(aReport,
                            DiagnosticSeverity::Warning,
                            ProcessingDiagnosticCode::InterestPayerNumberMissing,
                            makeDiagnosticMessage(MessageKey::InterestPayerNumberMissing),
                            std::move(aSources),
                            aTaxDate,
                            std::move(aName),
                            std::move(aIsin));
}

std::optional<std::pair<std::string, CatalogueSource>> treatyTextOf(
    const ReferenceCatalogue& aCatalogue, const std::string& aCountry, TreatyIncome aIncome) {
    const auto rows = aCatalogue.reliefStatements(aCountry, aIncome);

    if (rows.empty())
    {
        return std::nullopt;
    }

    const auto text = trimmedTreatyStatement(rows.front()->mStatement);
    const auto differs = [text](const ReliefStatementEntry* aRow) {
        return trimmedTreatyStatement(aRow->mStatement) != text;
    };

    // A supplied text that no claim may carry (too long, a control character) is no text.
    if (std::any_of(rows.begin(), rows.end(), differs) || !isValidTreatyStatement(text))
    {
        return std::nullopt;
    }

    return std::pair{std::string{text}, rows.front()->mSource};
}

std::optional<std::string> exportCountryOf(const IsinPayments& aPayments) {
    if (aPayments.mExportCountries.size() != 1)
    {
        return std::nullopt;
    }

    return *aPayments.mExportCountries.begin();
}

void addPayerNotFound(const PayerForm& aForm,
                      const PayerSinks& aSinks,
                      const Isin& aIsin,
                      const IsinPayments& aPayments,
                      const std::optional<std::string>& aCountry) {
    auto message = aCountry ? makeDiagnosticMessage(aForm.mNotFoundCountryKnownKey,
                                                    {textParam("country", *aCountry)})
                            : makeDiagnosticMessage(aForm.mNotFoundKey);

    aSinks.mDiagnostics.push_back(incomeDiagnostic(aForm.mReport,
                                                   DiagnosticSeverity::Warning,
                                                   aForm.mNotFoundCode,
                                                   std::move(message),
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
    const auto exportCountry = exportCountryOf(aPayments);
    // The country that stays when no reference row is used.
    const auto withoutRow = chooseCountry(exportCountry, nullptr, lookup, aIsin);
    const PayerEntry* used = nullptr;

    switch (lookup.mStatus)
    {
    case PayerLookupStatus::Found:
        used = lookup.mEntries.front();
        break;
    case PayerLookupStatus::Conflict:
        // Every row says SI: a Slovenian company settles the tax itself, whichever row is meant.
        if (aForm.mSlovenianExclusion && isSlovenianLookup(lookup))
        {
            return PayerOutcome{.mPayer = {}, .mIsSlovenian = true};
        }

        used = askPayer(aForm, aSinks, aIsin, aPayments, lookup, aDecisions, withoutRow.mCountry);
        break;
    case PayerLookupStatus::NotFound:
        addPayerNotFound(aForm, aSinks, aIsin, aPayments, withoutRow.mCountry);
        break;
    }

    PayerOutcome outcome;

    if (used != nullptr)
    {
        // The country of the row's number is the winning country; the exclusion of a Slovenian
        // payer is decided by the reference row alone.
        const auto country = chooseCountry(exportCountry, used, lookup, aIsin);

        outcome = payerOutcomeOfNumber(used->mName,
                                       used->mAddress,
                                       *country.mCountry,
                                       used->mTaxNumber,
                                       used->mSource);
        outcome.mIsSlovenian = used->mCountry == kSlovenia;
    }
    else
    {
        outcome.mPayer.mCountry = withoutRow.mCountry;

        if (withoutRow.mFromIsin)
        {
            aSinks.mDiagnostics.push_back(
                countryFromIsinWarning(aForm, aPayments, aIsin, *withoutRow.mCountry));
        }
    }

    // A form that reports a Slovenian payer's income too treats the payer as any other.
    outcome.mIsSlovenian = outcome.mIsSlovenian && aForm.mSlovenianExclusion.has_value();

    // A row that is written needs the tax number of a Slovenian payer.
    if (outcome.mSlovenianTaxNumberMissing && !outcome.mIsSlovenian)
    {
        aSinks.mDiagnostics.push_back(slovenianTaxNumberMissingWarning(aForm.mReport,
                                                                       aPayments.mSources,
                                                                       aPayments.mFirstDate,
                                                                       aPayments.mName,
                                                                       aIsin));
    }

    // A foreign payer needs a number in the form of Doh-Obr; the row is still written without it.
    if (aForm.mNeedsPayerNumber && outcome.mIdentificationNumberMissing)
    {
        aSinks.mDiagnostics.push_back(payerNumberMissingWarning(aForm.mReport,
                                                                aPayments.mSources,
                                                                aPayments.mFirstDate,
                                                                aPayments.mName,
                                                                aIsin));
    }

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
