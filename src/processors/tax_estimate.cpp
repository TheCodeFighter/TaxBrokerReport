#include "processors/tax_estimate.hpp"

#include "taxbroker/exact_arithmetic.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <variant>

namespace taxbroker {

namespace {

constexpr int kBracketCount = 5; ///< Under 5, 5 to 10, 10 to 15, 15 to 20 and over 20 years.
constexpr int kYearsPerBracket = 5;

/// The rates of a range of years, in per mille.
struct YearRates {
    int mFirstYear;
    int mLastYear;
    std::array<int, kBracketCount> mGain; ///< By holding period.
    int mIncome;                          ///< Dividends and interest.
};

constexpr std::array<YearRates, 3> kYearRates{{
    {2016, 2019, {250, 150, 100, 50, 0}, 250},
    {2020, 2021, {275, 200, 150, 100, 0}, 275},
    {2022, 2025, {250, 200, 150, 0, 0}, 250},
}};

/// The foreign tax counts up to this share of the gross, in per mille.
constexpr int kForeignTaxCreditPerMille = 150;
/// Normalized costs: purchase value x 1.01 and sale value x 0.99.
constexpr int kPurchaseFactor = 101; ///< Per 100.
constexpr int kSaleFactor = 99;      ///< Per 100.
/// A tax up to this many cents is not assessed.
constexpr std::int64_t kNotAssessedCents = 1000;
constexpr Money kMoneyPerCent = MONEY_SCALE / 100;

/// 1 per mille of a Money amount at MONEY_SCALE.
constexpr std::int64_t kPerMilleScale = 1000;

const YearRates* ratesOf(int aYear) {
    const auto found =
        std::find_if(kYearRates.begin(), kYearRates.end(), [aYear](const YearRates& aRow) {
            return aYear >= aRow.mFirstYear && aYear <= aRow.mLastYear;
        });

    return found == kYearRates.end() ? nullptr : &*found;
}

/// The anniversary after aYears; a 29 February becomes 28 February.
std::chrono::sys_days anniversary(const Date& aDate, int aYears) {
    const std::chrono::year_month_day start{std::chrono::sys_days{aDate}};
    const std::chrono::year year = start.year() + std::chrono::years{aYears};
    std::chrono::year_month_day result = year / start.month() / start.day();

    if (!result.ok())
    {
        result = year / start.month() / std::chrono::day{28};
    }

    return std::chrono::sys_days{result};
}

/// The holding-period bracket; a sale on an anniversary is still inside it.
std::size_t bracketOf(const Date& aPurchase, const Date& aSale) {
    const std::chrono::sys_days sale{aSale};

    for (int bracket = 0; bracket + 1 < kBracketCount; ++bracket)
    {
        if (sale <= anniversary(aPurchase, (bracket + 1) * kYearsPerBracket))
        {
            return static_cast<std::size_t>(bracket);
        }
    }

    return kBracketCount - 1;
}

/// An estimate of aNumerator / aDenominator EUR, rounded once to cents.
FormTaxEstimate
estimateOf(ReportType aForm, const WideInteger& aNumerator, const WideInteger& aDenominator) {
    FormTaxEstimate estimate{.mForm = aForm, .mIncomplete = true};
    // The denominator is never 0 (the callers pass a positive one), so the fraction always exists.
    const auto created = ExactRational::create(aNumerator, aDenominator);
    const auto& exact = std::get<ExactRational>(created);

    const DestinationRange range{.mMinimum = 0,
                                 .mMaximum =
                                     std::numeric_limits<std::int64_t>::max() / kMoneyPerCent};
    const auto cents = roundToScaled(exact, 100, range);

    if (!std::holds_alternative<std::int64_t>(cents))
    {
        return estimate;
    }

    const std::int64_t value = std::get<std::int64_t>(cents);

    estimate.mTax = value * kMoneyPerCent;
    estimate.mIncomplete = false;
    estimate.mNotAssessed = value > 0 && value <= kNotAssessedCents;

    return estimate;
}

/// An estimate with no amount.
FormTaxEstimate incompleteEstimate(ReportType aForm) {
    return {.mForm = aForm, .mIncomplete = true};
}

/// Purchase units that no sale has used yet.
struct OpenPurchase {
    Date mDate;
    Units mUnits{};
    UnitPrice mUnitPrice{};
};

/// Gains and losses at 1e-18 EUR.
struct CapitalSums {
    std::array<WideInteger, kBracketCount> mGainByBracket{};
    WideInteger mGains{};
    WideInteger mLosses{};
    bool mComplete{true};
};

/// Takes a sale's units from the oldest open purchases and adds each part's gain or loss.
void addSaleRow(const KdvpSaleRow& aSale, std::deque<OpenPurchase>& aOpen, CapitalSums& aSums) {
    Units remaining = aSale.mUnits;

    while (remaining > 0)
    {
        if (aOpen.empty())
        {
            // The rows sell units they never bought (a developer export).
            aSums.mComplete = false;

            return;
        }

        OpenPurchase& purchase = aOpen.front();
        const Units part = std::min(remaining, purchase.mUnits);
        const WideInteger saleValue = WideInteger{part} * WideInteger{aSale.mUnitPrice};
        const WideInteger purchaseValue = WideInteger{part} * WideInteger{purchase.mUnitPrice};
        // 99 % of the sale value less 101 % of the purchase value, times 100.
        const WideInteger gain = kSaleFactor * saleValue - kPurchaseFactor * purchaseValue;

        if (gain > 0)
        {
            aSums.mGainByBracket[bracketOf(purchase.mDate, aSale.mDate)] += gain;
            aSums.mGains += gain;
        }
        else if (aSale.mLossRule != KdvpLossRule::Blocked)
        {
            aSums.mLosses -= gain;
        }

        remaining -= part;
        purchase.mUnits -= part;

        if (purchase.mUnits == 0)
        {
            aOpen.pop_front();
        }
    }
}

/// Doh-KDVP: gains by holding period, lowered by the losses in proportion.
FormTaxEstimate estimateCapital(const YearRates& aRates, const CapitalGainsReportKDVP& aCapital) {
    CapitalSums sums;

    for (const KdvpInventoryList& list : aCapital.mLists)
    {
        if (list.mUnsafe)
        {
            return incompleteEstimate(ReportType::CapitalGains);
        }

        std::deque<OpenPurchase> open;

        for (const KdvpRow& row : list.mRows)
        {
            if (const auto* purchase = std::get_if<KdvpPurchaseRow>(&row.mEntry))
            {
                open.push_back({purchase->mDate, purchase->mUnits, purchase->mUnitPrice});
            }
            else
            {
                addSaleRow(std::get<KdvpSaleRow>(row.mEntry), open, sums);
            }
        }
    }

    if (!sums.mComplete)
    {
        return incompleteEstimate(ReportType::CapitalGains);
    }

    // Losses lower every gain by the same share: tax = weighted gains x (gains - losses) / gains.
    if (sums.mGains <= sums.mLosses)
    {
        return estimateOf(ReportType::CapitalGains, 0, 1);
    }

    WideInteger weighted{};

    for (std::size_t bracket = 0; bracket < kBracketCount; ++bracket)
    {
        weighted += WideInteger{aRates.mGain[bracket]} * sums.mGainByBracket[bracket];
    }

    // The gains are at 1e-18 EUR and the rates in per mille.
    constexpr std::int64_t kGainScale = 1'000'000'000'000'000'000;

    return estimateOf(ReportType::CapitalGains,
                      (sums.mGains - sums.mLosses) * weighted,
                      sums.mGains * kPerMilleScale * kGainScale);
}

/// The tax of an income row, in per mille of a Money unit.
WideInteger incomeRowTax(const YearRates& aRates, Money aGross, Money aForeignTax) {
    const WideInteger slovenian = WideInteger{aRates.mIncome} * WideInteger{aGross};
    const WideInteger credit =
        std::min(WideInteger{kPerMilleScale} * WideInteger{aForeignTax},
                 WideInteger{kForeignTaxCreditPerMille} * WideInteger{aGross});

    return slovenian - credit;
}

/// The estimate of a tax in per mille of Money units.
FormTaxEstimate estimateOfPerMilleMoney(ReportType aForm, const WideInteger& aTax) {
    return estimateOf(aForm, aTax, WideInteger{kPerMilleScale} * WideInteger{MONEY_SCALE});
}

FormTaxEstimate estimateDividends(const YearRates& aRates, const DividendReportDiv& aDividends) {
    const WideInteger startSum{};
    const WideInteger tax = std::accumulate(
        aDividends.mRows.begin(),
        aDividends.mRows.end(),
        startSum,
        [&aRates](const WideInteger& aSum, const DivDividendRow& aRow) {
            return aSum + incomeRowTax(aRates, aRow.mGrossIncome, aRow.mWithholding.mForeignTax);
        });

    return estimateOfPerMilleMoney(ReportType::Dividends, tax);
}

FormTaxEstimate estimateOtherInterest(const YearRates& aRates,
                                      const OtherInterestReportObr& aOther) {
    const WideInteger startSum{};
    const WideInteger tax = std::accumulate(
        aOther.mRows.begin(),
        aOther.mRows.end(),
        startSum,
        [&aRates](const WideInteger& aSum, const ObrInterestRow& aRow) {
            return aSum + incomeRowTax(aRates, aRow.mGrossInterest, aRow.mWithholding.mForeignTax);
        });

    return estimateOfPerMilleMoney(ReportType::OtherInterest, tax);
}

/// Doh-DHO: only the interest above the allowance is taxed.
FormTaxEstimate estimateDeposit(const YearRates& aRates, const DepositInterestSummary& aDeposit) {
    const WideInteger above =
        std::max(WideInteger{0},
                 WideInteger{aDeposit.mImportedQualifying} - WideInteger{aDeposit.mAllowance});

    return estimateOfPerMilleMoney(ReportType::BankDepositInterest,
                                   WideInteger{aRates.mIncome} * above);
}

/// Runs an estimate; a sum beyond 256 bits makes the form incomplete.
template <typename Estimate> FormTaxEstimate guarded(ReportType aForm, Estimate&& aEstimate) {
    try
    { return aEstimate(); } catch (const std::overflow_error&)
    { return incompleteEstimate(aForm); }
}

} // namespace

TaxEstimate estimateTax(int aYear,
                        const CapitalGainsReportKDVP& aCapital,
                        const DividendReportDiv& aDividends,
                        const std::optional<DepositInterestSummary>& aDeposit,
                        const OtherInterestReportObr& aOther) {
    const YearRates* rates = ratesOf(aYear);

    if (rates == nullptr)
    {
        return {};
    }

    TaxEstimate estimate{.mYearSupported = true};

    if (!aCapital.mLists.empty())
    {
        estimate.mForms.push_back(
            guarded(ReportType::CapitalGains, [&] { return estimateCapital(*rates, aCapital); }));
    }

    if (!aDividends.mRows.empty())
    {
        estimate.mForms.push_back(
            guarded(ReportType::Dividends, [&] { return estimateDividends(*rates, aDividends); }));
    }

    if (aDeposit)
    {
        estimate.mForms.push_back(guarded(ReportType::BankDepositInterest,
                                          [&] { return estimateDeposit(*rates, *aDeposit); }));
    }

    if (!aOther.mRows.empty())
    {
        estimate.mForms.push_back(guarded(ReportType::OtherInterest,
                                          [&] { return estimateOtherInterest(*rates, aOther); }));
    }

    return estimate;
}

TaxEstimate completeTaxEstimate(TaxEstimate aEstimate,
                                const std::array<ReportStatus, 4>& aStatuses) {
    if (!aEstimate.mYearSupported)
    {
        // A year without rates is worth a message only when a form has data.
        const bool hasData =
            std::any_of(aStatuses.begin(), aStatuses.end(), [](ReportStatus aStatus) {
                return aStatus != ReportStatus::NoData;
            });

        return {.mYearSupported = !hasData};
    }

    TaxEstimate result{.mYearSupported = true};

    for (std::size_t index = 0; index < aStatuses.size(); ++index)
    {
        const ReportType type = static_cast<ReportType>(index);

        if (aStatuses[index] == ReportStatus::NoData)
        {
            continue;
        }

        FormTaxEstimate form = incompleteEstimate(type);

        if (aStatuses[index] == ReportStatus::Generated)
        {
            const auto found =
                std::find_if(aEstimate.mForms.begin(),
                             aEstimate.mForms.end(),
                             [type](const FormTaxEstimate& aForm) { return aForm.mForm == type; });

            if (found != aEstimate.mForms.end())
            {
                form = *found;
            }
        }

        result.mForms.push_back(form);

        if (form.mIncomplete)
        {
            result.mTotalPartial = true;
        }
        else if (!form.mNotAssessed)
        {
            const auto sum = checkedAdd(result.mTotal, *form.mTax);

            if (std::holds_alternative<std::int64_t>(sum))
            {
                result.mTotal = std::get<std::int64_t>(sum);
            }
            else
            {
                result.mTotalPartial = true;
            }
        }
    }

    return result;
}

} // namespace taxbroker
