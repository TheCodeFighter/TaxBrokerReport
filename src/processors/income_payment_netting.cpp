#include "processors/income_payment_netting.hpp"
#include "taxbroker/diagnostic_messages.hpp"
#include "taxbroker/exact_arithmetic.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <tuple>
#include <utility>
#include <variant>

namespace taxbroker {
namespace {

/// The rows of one payment, by kind.
struct PaymentGroup {
    std::vector<const IncomePaymentRow*> mAll;
    std::vector<const IncomePaymentRow*> mDividends;
    std::vector<const IncomePaymentRow*> mReversals;
    std::vector<const IncomePaymentRow*> mTaxes;
    std::vector<const IncomePaymentRow*> mRefunds;
};

/// A payment is one broker, one payment group ID and one ISIN.
using GroupKey = std::tuple<Broker, std::string, Isin>;

int yearOf(Date aDate) {
    const std::chrono::year_month_day day{std::chrono::sys_days{aDate.time_since_epoch()}};

    return static_cast<int>(day.year());
}

void addToGroup(PaymentGroup& aGroup, const IncomePaymentRow& aRow) {
    aGroup.mAll.push_back(&aRow);

    switch (aRow.mKind)
    {
    case IncomePaymentKind::Dividend:
        aGroup.mDividends.push_back(&aRow);
        break;
    case IncomePaymentKind::DividendReversal:
        aGroup.mReversals.push_back(&aRow);
        break;
    case IncomePaymentKind::Tax:
        aGroup.mTaxes.push_back(&aRow);
        break;
    case IncomePaymentKind::TaxRefund:
        aGroup.mRefunds.push_back(&aRow);
        break;
    }
}

/// The sources of every row of the payment, in stable source order without repeats.
std::vector<SourceReference> sourcesOf(const PaymentGroup& aGroup) {
    std::vector<SourceReference> sources;

    for (const auto* row : aGroup.mAll)
    {
        sources.insert(sources.end(),
                       row->mMetadata.mSources.begin(),
                       row->mMetadata.mSources.end());
    }

    std::sort(sources.begin(), sources.end(), StableSourceOrder{});
    sources.erase(std::unique(sources.begin(), sources.end()), sources.end());

    return sources;
}

/// The name of the first row that has one; empty when no row names the instrument.
std::string nameOf(const PaymentGroup& aGroup) {
    const auto named =
        std::find_if(aGroup.mAll.begin(), aGroup.mAll.end(), [](const IncomePaymentRow* aRow) {
            return !aRow->mName.empty();
        });

    return named == aGroup.mAll.end() ? std::string{} : (*named)->mName;
}

MergeDiagnostic diagnosticOf(const PaymentGroup& aGroup,
                             DiagnosticSeverity aSeverity,
                             MergeDiagnosticCode aCode,
                             DiagnosticMessage aMessage,
                             Date aTaxDate) {
    const auto name = nameOf(aGroup);

    return MergeDiagnostic{
        .mSeverity = aSeverity,
        .mCode = aCode,
        .mMessage = std::move(aMessage.mEnglish),
        .mMessageKey = aMessage.mKey,
        .mMessageParams = std::move(aMessage.mParams),
        .mSources = sourcesOf(aGroup),
        .mTaxDate = aTaxDate,
        .mInstrumentName = name.empty() ? std::nullopt : std::optional<std::string>{name},
        .mIsin = aGroup.mAll.front()->mIsin,
        .mEventKinds = {StatementEventKind::Dividend},
    };
}

/// The sum of the amounts, or nothing when it does not fit.
std::optional<Money> sumOf(const std::vector<const IncomePaymentRow*>& aRows) {
    Money total = 0;

    for (const auto* row : aRows)
    {
        const auto sum = checkedAdd(total, row->mAmount);

        if (!std::holds_alternative<Money>(sum))
        {
            return std::nullopt;
        }

        total = std::get<Money>(sum);
    }

    return total;
}

/// True when every row of both lists has the currency of the first row.
bool sameCurrency(const std::vector<const IncomePaymentRow*>& aFirst,
                  const std::vector<const IncomePaymentRow*>& aSecond) {
    const auto* head =
        aFirst.empty() ? (aSecond.empty() ? nullptr : aSecond.front()) : aFirst.front();

    if (head == nullptr)
    {
        return true;
    }

    const auto matches = [head](const IncomePaymentRow* aRow) {
        return aRow->mCurrency == head->mCurrency;
    };

    return std::all_of(aFirst.begin(), aFirst.end(), matches) &&
           std::all_of(aSecond.begin(), aSecond.end(), matches);
}

/// The first country that a dividend or reversal row names; none when no such row names one. The
/// rows of tax are not read: a tax row may carry the country of the one who withheld it.
std::optional<std::string> countryOf(const std::vector<const IncomePaymentRow*>& aDividends,
                                     const std::vector<const IncomePaymentRow*>& aReversals) {
    const auto hasCountry = [](const IncomePaymentRow* aRow) {
        return aRow->mPayerCountry.has_value();
    };

    for (const auto* rows : {&aDividends, &aReversals})
    {
        const auto found = std::find_if(rows->begin(), rows->end(), hasCountry);

        if (found != rows->end())
        {
            return (*found)->mPayerCountry;
        }
    }

    return std::nullopt;
}

/// True when no dividend or reversal row names a country other than the one that countryOf finds.
bool sameCountry(const std::vector<const IncomePaymentRow*>& aDividends,
                 const std::vector<const IncomePaymentRow*>& aReversals) {
    const auto country = countryOf(aDividends, aReversals);
    const auto matches = [&country](const IncomePaymentRow* aRow) {
        return !aRow->mPayerCountry || aRow->mPayerCountry == country;
    };

    return std::all_of(aDividends.begin(), aDividends.end(), matches) &&
           std::all_of(aReversals.begin(), aReversals.end(), matches);
}

bool samePayDate(const std::vector<const IncomePaymentRow*>& aDividends) {
    return std::all_of(aDividends.begin(), aDividends.end(), [&aDividends](const auto* aRow) {
        return aRow->mMetadata.mTaxDate == aDividends.front()->mMetadata.mTaxDate;
    });
}

/// The latest year in which a row of the payment was booked; nothing when no row has a date.
std::optional<int> latestBookedYear(const PaymentGroup& aGroup) {
    std::optional<int> latest;

    for (const auto* row : aGroup.mAll)
    {
        if (row->mBookedDate)
        {
            latest = std::max(latest.value_or(0), yearOf(*row->mBookedDate));
        }
    }

    return latest;
}

/// Adds the dividend of one payment, or the diagnostic that says why there is none.
void netGroup(const PaymentGroup& aGroup, IncomePaymentNettingResult& aResult) {
    if (aGroup.mDividends.empty())
    {
        aResult.mDiagnostics.push_back(
            diagnosticOf(aGroup,
                         DiagnosticSeverity::Warning,
                         MergeDiagnosticCode::DividendReversalWithoutPayment,
                         makeDiagnosticMessage(MessageKey::DividendReversalWithoutPayment),
                         aGroup.mAll.front()->mMetadata.mTaxDate));

        return;
    }

    const auto payDate = aGroup.mDividends.front()->mMetadata.mTaxDate;
    const auto gross = sumOf(aGroup.mDividends);
    const auto reversed = sumOf(aGroup.mReversals);
    const auto tax = sumOf(aGroup.mTaxes);
    const auto refunded = sumOf(aGroup.mRefunds);

    const bool fits = sameCurrency(aGroup.mDividends, aGroup.mReversals) &&
                      sameCurrency(aGroup.mTaxes, aGroup.mRefunds) &&
                      samePayDate(aGroup.mDividends) &&
                      sameCountry(aGroup.mDividends, aGroup.mReversals) && gross && reversed &&
                      tax && refunded && *refunded <= *tax;

    if (!fits)
    {
        aResult.mDiagnostics.push_back(
            diagnosticOf(aGroup,
                         DiagnosticSeverity::Error,
                         MergeDiagnosticCode::IncomePaymentRowsDisagree,
                         makeDiagnosticMessage(MessageKey::IncomePaymentRowsDisagree),
                         payDate));

        return;
    }

    const Money net = *gross - *reversed;

    if (net <= 0)
    {
        aResult.mDiagnostics.push_back(
            diagnosticOf(aGroup,
                         DiagnosticSeverity::Warning,
                         MergeDiagnosticCode::DividendNetNotPositive,
                         makeDiagnosticMessage(MessageKey::DividendNetNotPositive),
                         payDate));

        return;
    }

    const bool hasTaxRows = !aGroup.mTaxes.empty() || !aGroup.mRefunds.empty();
    const auto currency = aGroup.mDividends.front()->mCurrency;
    const auto taxCurrency =
        aGroup.mTaxes.empty()
            ? (aGroup.mRefunds.empty() ? currency : aGroup.mRefunds.front()->mCurrency)
            : aGroup.mTaxes.front()->mCurrency;

    DividendTransaction dividend{
        .mMetadata = {.mTaxDate = payDate, .mSources = sourcesOf(aGroup)},
        .mGrossAmount = net,
        .mTaxPaid = hasTaxRows ? std::optional<Money>{*tax - *refunded} : std::nullopt,
        .mCurrency = currency,
        .mTaxCurrency = taxCurrency,
        .mPayerList = std::nullopt, // the payment rows name no payer list
        .mPayerCountry = countryOf(aGroup.mDividends, aGroup.mReversals),
    };

    for (const auto* row : aGroup.mAll)
    {
        dividend.mInstrumentEvidence.insert(dividend.mInstrumentEvidence.end(),
                                            row->mInstrumentEvidence.begin(),
                                            row->mInstrumentEvidence.end());
    }

    aResult.mDividends.push_back(NettedDividend{
        .mName = nameOf(aGroup),
        .mIsin = aGroup.mAll.front()->mIsin,
        .mEvent = std::move(dividend),
    });

    const bool isSubstitute =
        std::any_of(aGroup.mAll.begin(), aGroup.mAll.end(), [](const auto* aRow) {
            return aRow->mSubstitutePayment;
        });

    if (isSubstitute)
    {
        aResult.mDiagnostics.push_back(
            diagnosticOf(aGroup,
                         DiagnosticSeverity::Warning,
                         MergeDiagnosticCode::SubstituteDividendPayment,
                         makeDiagnosticMessage(MessageKey::SubstituteDividendPayment),
                         payDate));
    }

    const auto bookedYear = latestBookedYear(aGroup);

    if (bookedYear && *bookedYear > yearOf(payDate))
    {
        aResult.mDiagnostics.push_back(
            diagnosticOf(aGroup,
                         DiagnosticSeverity::Warning,
                         MergeDiagnosticCode::IncomeCorrectionLaterYear,
                         makeDiagnosticMessage(
                             MessageKey::IncomeCorrectionLaterYear,
                             {numberParam("payYear", static_cast<std::uint64_t>(yearOf(payDate))),
                              numberParam("bookedYear", static_cast<std::uint64_t>(*bookedYear))}),
                         payDate));
    }
}

} // namespace

IncomePaymentNettingResult netIncomePayments(const std::vector<IncomePaymentRow>& aRows) {
    std::map<GroupKey, PaymentGroup> groups;

    for (const auto& row : aRows)
    {
        const auto broker = primarySource(row.mMetadata).mBroker;

        addToGroup(groups[GroupKey{broker, row.mPaymentGroupId, row.mIsin}], row);
    }

    IncomePaymentNettingResult result;

    for (const auto& [key, group] : groups)
    {
        netGroup(group, result);
    }

    return result;
}

} // namespace taxbroker
