#pragma once

#include "processors/trade_conversion.hpp"
#include "taxbroker/application_result.hpp"
#include "taxbroker/decision_session.hpp"
#include "taxbroker/diagnostic_messages.hpp"
#include "taxbroker/income_report_models.hpp"
#include "taxbroker/reference_catalogue.hpp"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace taxbroker {

/// What the year's payments of one ISIN share.
struct IsinPayments {
    std::string mName;
    std::vector<SourceReference> mSources;
    Date mFirstDate;
};

/// The payer of an ISIN as the rows will carry it.
struct PayerOutcome {
    IncomePayer mPayer; ///< Empty when the payer is not known.
    /// The payer's country is SI. `resolvePayer` keeps it only for a form with a
    /// `mSlovenianExclusion`, where such a payer settles the tax itself.
    bool mIsSlovenian{};
    /// The payer's country is SI but its reference row has no valid Slovenian tax number, which
    /// the forms need for such a payer. `resolvePayer` warns once for a row that is written.
    bool mSlovenianTaxNumberMissing{};
};

/// What differs between the forms that look a payer up by ISIN.
struct PayerForm {
    ReportType mReport{};                     ///< The form the diagnostics and questions affect.
    ProcessingDiagnosticCode mNotFoundCode{}; ///< The warning for a payer without facts.
    MessageKey mNotFoundKey{};                ///< Its message.
    /// The category of the excluded item of a Slovenian payer. Empty when the form reports the
    /// income of a Slovenian payer too, because no Slovenian payer settles the tax on it.
    std::optional<ExcludedCategory> mSlovenianExclusion;
};

/// Where the payer helpers put what they find.
struct PayerSinks {
    std::vector<ProcessingDiagnostic>& mDiagnostics;
    std::vector<RequestInput>& mRequests;
    std::vector<ExcludedItemInput>& mExcludedItems;
};

/// A diagnostic of an income stage that affects one form.
[[nodiscard]] ProcessingDiagnostic incomeDiagnostic(ReportType aReport,
                                                    DiagnosticSeverity aSeverity,
                                                    ProcessingDiagnosticCode aCode,
                                                    DiagnosticMessage aMessage,
                                                    std::vector<SourceReference> aSources,
                                                    std::optional<Date> aTaxDate,
                                                    std::optional<std::string> aName,
                                                    std::optional<Isin> aIsin);

/// The payer facts of a reference row. A foreign payer's number is its identification number; a
/// Slovenian payer's number is its tax number when it is valid, otherwise nothing, and it never
/// has an identification number.
[[nodiscard]] PayerOutcome payerOutcomeOf(const PayerEntry& aEntry);

/// The payer facts of a broker company that pays interest, with the same number rule.
[[nodiscard]] PayerOutcome payerOutcomeOf(const BrokerPayerEntry& aEntry);

/// The warning that a written row has a Slovenian payer without a valid Slovenian tax number.
[[nodiscard]] ProcessingDiagnostic
slovenianTaxNumberMissingWarning(ReportType aReport,
                                 std::vector<SourceReference> aSources,
                                 std::optional<Date> aTaxDate,
                                 std::optional<std::string> aName,
                                 std::optional<Isin> aIsin);

/// The warning that the payer has no facts: not found, or several rows and none picked.
void addPayerNotFound(const PayerForm& aForm,
                      const PayerSinks& aSinks,
                      const Isin& aIsin,
                      const IsinPayments& aPayments);

/// The payer of an ISIN from the catalogue and the user's answer. A payer that is not found, or
/// that the user skipped, has no facts and a warning; several rows are a question that never
/// blocks. For a form that leaves a Slovenian payer out, a payer whose every row has the residence
/// country SI is Slovenian without a question.
[[nodiscard]] PayerOutcome resolvePayer(const PayerForm& aForm,
                                        const PayerSinks& aSinks,
                                        const Isin& aIsin,
                                        const IsinPayments& aPayments,
                                        const ReferenceCatalogue& aCatalogue,
                                        RunDecisions& aDecisions);

/// The warning and the excluded item of the payments of a Slovenian payer.
void addSlovenianExclusion(const PayerForm& aForm,
                           const PayerSinks& aSinks,
                           const Isin& aIsin,
                           const IsinPayments& aPayments);

/// The trimmed treaty text of a country for a kind of income and its reference row, or none when
/// the country has no row of that kind, its rows give different texts or the text is not a valid
/// claim text. A text of another kind of income is never used.
[[nodiscard]] std::optional<std::pair<std::string, CatalogueSource>> treatyTextOf(
    const ReferenceCatalogue& aCatalogue, const std::string& aCountry, TreatyIncome aIncome);

/// Adds the rate needs of one income payment: the gross, and the tax when it is above 0 and
/// foreign.
void addIncomeRateNeeds(std::vector<RateNeed>& aNeeds,
                        ReportType aReport,
                        const EventMetadata& aMetadata,
                        Currency aGrossCurrency,
                        const std::optional<Money>& aTax,
                        Currency aTaxCurrency);

/// Adds the error of each currency of one payment that has no official rate on its receipt date.
bool addPaymentMissingRateErrors(const OfficialRateSelection& aRates,
                                 ReportType aReport,
                                 const EventMetadata& aMetadata,
                                 const std::string& aName,
                                 const std::optional<Isin>& aIsin,
                                 Currency aGrossCurrency,
                                 const std::optional<Money>& aTax,
                                 Currency aTaxCurrency,
                                 std::vector<ProcessingDiagnostic>& aDiagnostics);

/// Converts income amounts to EUR with the selected official rates and keeps the rates it used.
class IncomeEurConverter {
  public:
    IncomeEurConverter(const OfficialRateSelection& aRates,
                       std::vector<ProcessingDiagnostic>& aDiagnostics);

    /// aAmount in EUR, or none when the rate is missing (the selection holds the error) or the
    /// amount does not fit (a diagnostic of aReport is added).
    [[nodiscard]] std::optional<Money> toEur(ReportType aReport,
                                             const EventMetadata& aMetadata,
                                             const std::string& aName,
                                             const std::optional<Isin>& aIsin,
                                             Money aAmount,
                                             Currency aCurrency,
                                             const char* aField);

    /// The used rates, one per currency and day, by date, then code.
    [[nodiscard]] std::vector<UsedOfficialRate> usedRates() const;

  private:
    const OfficialRateSelection& mRates;
    std::vector<ProcessingDiagnostic>& mDiagnostics;
    std::map<std::pair<Date, std::string>, UsedOfficialRate> mUsed;
};

/// Moves the positions of the excluded items' diagnostics to the numbering of aInput and appends
/// them.
void appendExcludedItems(ApplicationResultInput& aInput,
                         std::vector<ExcludedItemInput>&& aItems,
                         std::size_t aDiagnosticOffset);

} // namespace taxbroker
