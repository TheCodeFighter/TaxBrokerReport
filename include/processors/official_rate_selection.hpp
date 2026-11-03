#pragma once

#include "taxbroker/exact_arithmetic.hpp"
#include "taxbroker/official_rates.hpp"
#include "taxbroker/processing_diagnostics.hpp"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace taxbroker {

/// The farthest back from a tax date that the last published rate may be taken. It covers the
/// longest run of days without an ECB rate (Good Friday to Easter Monday, with a weekend).
inline constexpr int kMaxRateDaysBack = 4;

/// What a rate converts. A fee is never a use: fees never need an official rate.
enum class RateUse {
    PurchasePrice,
    SalePrice,
    IncomeGross,
    ForeignTax,
};

/// One value of a report that needs `1 EUR = rate` units of its currency on its tax date.
struct RateNeed {
    Currency mCurrency{Currency::Unknown};
    Date mTaxDate{};
    RateUse mUse{RateUse::PurchasePrice}; ///< Documents the need; the lookup does not read it.
    ReportType mReport{ReportType::CapitalGains};
    std::vector<SourceReference> mSources; ///< The events that hold the value.
};

/// Why a rate file cannot give a rate.
enum class RateFileProblem {
    NotLoaded, ///< No file of that year is loaded.
    EndsEarly, ///< The file of that year ends before the day.
};

/// The rate file that a missing rate needs. Loading it may fix the rate.
struct RateFileNeed {
    int mYear{}; ///< The year of the file, which can be the year before the tax date.
    RateFileProblem mProblem{RateFileProblem::NotLoaded};
};

/// The official rates found for the needs, and one diagnostic per currency and tax date that was
/// not found exactly or not found at all.
struct OfficialRateSelection {
    /// Keyed by currency code and the tax date; the rate's own date can be earlier.
    std::map<std::pair<std::string, Date>, OfficialRate> mSelected;
    /// A warning for a rate taken from an earlier date or a used file that is not a whole year; an
    /// error for a rate that was not found.
    std::vector<ProcessingDiagnostic> mDiagnostics;

    /// The error of each currency and tax date without a rate (the same as in mDiagnostics).
    std::map<std::pair<std::string, Date>, ProcessingDiagnostic> mMissing;

    /// The file that would fix each missing pair, for the pairs a file can fix.
    std::map<std::pair<std::string, Date>, RateFileNeed> mMissingFiles;

    /// The rate selected for this currency and tax date, or null. EUR has none.
    [[nodiscard]] const OfficialRate* find(Currency aCurrency, Date aTaxDate) const;

    /// The rate file that would fix this missing pair, or null (no file can fix it, or no error).
    [[nodiscard]] const RateFileNeed* findMissingFile(Currency aCurrency, Date aTaxDate) const;

    /// The error for this currency and tax date without a rate, or null.
    [[nodiscard]] const ProcessingDiagnostic* findMissing(Currency aCurrency, Date aTaxDate) const;
};

/**
 * Finds the official rate of every foreign currency and tax date in the needs. EUR needs are
 * skipped. When the tax date has no ECB rate, the last earlier ECB rate within kMaxRateDaysBack
 * days is used with a warning, unless the ECB published rates that day without this currency.
 * Otherwise the pair gets a blocking error: the euro replaced the currency, a rate file is not
 * loaded or ends before the day, or the loaded files have no rate. Pair diagnostics are ordered
 * by tax date, then currency code; then come warnings for a used file that starts late in its
 * year or is accepted as a whole year though it ends before the year does. A need with an
 * unknown currency throws std::logic_error.
 */
[[nodiscard]] OfficialRateSelection selectOfficialRates(const std::vector<RateNeed>& aNeeds,
                                                        const OfficialRateTable& aTable);

/// EUR value of a foreign amount at an official rate: divides by the rate, rounds once to Money.
[[nodiscard]] NumericResult<Money> convertOfficialMoney(Money aForeignMoney,
                                                        const OfficialRate& aRate,
                                                        const DestinationRange& aRange = {});

/// EUR value of a foreign unit price at an official rate, rounded once to UnitPrice.
[[nodiscard]] NumericResult<UnitPrice> convertOfficialUnitPrice(
    UnitPrice aForeignUnitPrice, const OfficialRate& aRate, const DestinationRange& aRange = {});

} // namespace taxbroker
