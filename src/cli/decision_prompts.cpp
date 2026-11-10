#include "cli/decision_prompts.hpp"

#include "cli_text.hpp"
#include "taxbroker/api/application_result_json.hpp"
#include "taxbroker/currency.hpp"
#include "taxbroker/split_ratio.hpp"
#include "utils/date_utils.hpp"
#include "utils/numeric_util.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace taxbroker::cli {
namespace {

constexpr std::string_view kRatioHelp =
    "  Enter the ratio from the announcement as new : old, for example 10 : 1 (every old share\n"
    "  becomes 10 shares; \"nine additional shares for every share\" is also 10 : 1; a reverse\n"
    "  split is 1 : 10), or your shares just before and just after it as before -> after, for\n"
    "  example 0,52 -> 5,2 (a decimal comma or point), or cancel.\n";

/// The text without spaces, tabs or a carriage return around it.
std::string trimmed(std::string_view aText) {
    const auto isSpace = [](char aCharacter) {
        return aCharacter == ' ' || aCharacter == '\t' || aCharacter == '\r';
    };

    while (!aText.empty() && isSpace(aText.front()))
    {
        aText.remove_prefix(1);
    }

    while (!aText.empty() && isSpace(aText.back()))
    {
        aText.remove_suffix(1);
    }

    return std::string{aText};
}

std::string lowerCase(std::string aText) {
    std::transform(aText.begin(), aText.end(), aText.begin(), [](unsigned char aCharacter) {
        return static_cast<char>(std::tolower(aCharacter));
    });

    return aText;
}

/// Writes the prompt and reads the next line, without the spaces around it. Empty when the input
/// has ended.
std::optional<std::string>
readLine(std::istream& aIn, std::ostream& aOut, std::string_view aPrompt) {
    std::string line;

    aOut << aPrompt << '\n' << std::flush;

    if (!std::getline(aIn, line))
    {
        return std::nullopt;
    }

    return trimmed(line);
}

void writeLossQuestion(std::ostream& aOut,
                       const DecisionRequest& aRequest,
                       const LossEligibilityQuestion& aQuestion) {
    aOut << "Question " << aRequest.mId << ": the 30-day loss rule for "
         << investmentText(aQuestion.mIsin, aQuestion.mName) << '\n'
         << "  Your files cannot decide the loss of "
         << countText(aQuestion.mSales.size(), "sale", "sales") << ":\n";

    for (const auto& sale : aQuestion.mSales)
    {
        aOut << "    sale " << formatCalendarDate(sale.mSaleDate) << ": "
             << unitsText(sale.mSaleUnits) << " units, " << unitsText(sale.mLossUnits)
             << " at a loss, " << unitsText(sale.mOpenUnits) << " open; window "
             << formatCalendarDate(sale.mWindowStart) << " to "
             << formatCalendarDate(sale.mWindowEnd);

        if (sale.mKnownReplacements.empty())
        {
            aOut << ", no purchase in your files counts against it\n";
            continue;
        }

        aOut << ", purchases in your files that count against it, in the units of this sale:";

        for (const auto& replacement : sale.mKnownReplacements)
        {
            aOut << ' ' << formatCalendarDate(replacement.mDate) << ' '
                 << unitsText(replacement.mUnits) << " units"
                 << (&replacement == &sale.mKnownReplacements.back() ? "\n" : ",");
        }
    }

    aOut << "  Within 30 days before or after the sale, did you also buy or sell this investment\n"
            "  anywhere that is not in your files (another broker, another account, or after the\n"
            "  export)? yes: the open loss is not deductible (F10 false). no: it is deductible.\n"
            "  cancel: no answer, and the sales cannot be reported.\n";
}

std::optional<AnswerValue> readLossAnswer(std::istream& aIn, std::ostream& aOut) {
    while (true)
    {
        const auto line = readLine(aIn, aOut, "Your answer (yes, no or cancel):");

        if (!line)
        {
            return std::nullopt;
        }

        const auto word = lowerCase(*line);

        if (word == "yes")
        {
            return OtherActivityAnswer{};
        }

        if (word == "no")
        {
            return NoOtherActivityAnswer{};
        }

        if (word == "cancel")
        {
            return CancelAnswer{};
        }

        aOut << "Answer yes, no or cancel.\n";
    }
}

/// The form that a failure question is about. Another request type throws std::logic_error.
std::string_view failureFormName(RequestType aType) {
    switch (aType)
    {
    case RequestType::CapitalGainsFailureResolutionRequired:
        return "Doh-KDVP";
    case RequestType::DividendsFailureResolutionRequired:
        return "Doh-Div";
    case RequestType::OtherInterestFailureResolutionRequired:
        return "Doh-Obr";
    default:
        throw std::logic_error("Only a failure question names its form.");
    }
}

void writeFailureQuestion(std::ostream& aOut, const DecisionRequest& aRequest) {
    const auto form = failureFormName(aRequest.mType);

    aOut << "Question " << aRequest.mId << ": " << form << " cannot include "
         << countText(aRequest.mInvestments.size(), "investment", "investments") << ":\n";

    for (const auto& investment : aRequest.mInvestments)
    {
        aOut << "  " << investmentText(investment.mIsin, investment.mName) << ": "
             << reasonCodesText(investment) << '\n';
    }

    aOut << "  exclude: write " << form
         << " without every investment listed here; they are not in the\n"
            "  XML, and the summary lists them. fail: write no "
         << form << ". cancel: no answer, and no\n  " << form << ".\n";
}

std::optional<AnswerValue> readFailureAnswer(std::istream& aIn, std::ostream& aOut) {
    while (true)
    {
        const auto line = readLine(aIn, aOut, "Your answer (exclude, fail or cancel):");

        if (!line)
        {
            return std::nullopt;
        }

        const auto word = lowerCase(*line);

        if (word == "exclude")
        {
            return FailureResolutionAnswer{FailureResolution::ExcludeFailedInstruments};
        }

        if (word == "fail")
        {
            return FailureResolutionAnswer{FailureResolution::FailReport};
        }

        if (word == "cancel")
        {
            return CancelAnswer{};
        }

        aOut << "Answer exclude, fail or cancel.\n";
    }
}

void writeClassificationQuestion(std::ostream& aOut,
                                 const DecisionRequest& aRequest,
                                 const TaxClassificationQuestion& aQuestion) {
    std::string labels;

    for (const auto& label : aQuestion.mClassLabels)
    {
        labels += (labels.empty() ? "" : ", ") + (label.empty() ? std::string{"(blank)"} : label);
    }

    aOut << "Question " << aRequest.mId << ": the asset class of "
         << investmentText(aQuestion.mIsin, aQuestion.mName) << " is missing, not recognized or "
         << "differs between rows (class in the files: " << labels << ").\n"
         << "  capital: report it as shares or fund units in Doh-KDVP.\n"
            "  exclude: leave it out of the reports; the summary lists it, and you report it\n"
            "  yourself if it is taxable. cancel: no answer, and no Doh-KDVP.\n";
}

std::optional<AnswerValue> readClassificationAnswer(std::istream& aIn, std::ostream& aOut) {
    while (true)
    {
        const auto line = readLine(aIn, aOut, "Your answer (capital, exclude or cancel):");

        if (!line)
        {
            return std::nullopt;
        }

        const auto word = lowerCase(*line);

        if (word == "capital")
        {
            return CapitalGainsAnswer{};
        }

        if (word == "exclude")
        {
            return ExcludeFromReportsAnswer{};
        }

        if (word == "cancel")
        {
            return CancelAnswer{};
        }

        aOut << "Answer capital, exclude or cancel.\n";
    }
}

void writeRatioQuestion(std::ostream& aOut,
                        const DecisionRequest& aRequest,
                        const CorporateActionRatioQuestion& aQuestion) {
    aOut << "Question " << aRequest.mId << ": a corporate action of "
         << investmentText(aQuestion.mIsin, aQuestion.mName) << " on "
         << formatCalendarDate(aQuestion.mEffectiveDate) << '\n';

    for (const auto& source : aQuestion.mSources)
    {
        aOut << "  Source: " << brokerName(source.mBroker);

        if (aQuestion.mAskSameEvent)
        {
            aOut << ", row of " << formatCalendarDate(source.mDate);
        }

        if (source.mTransactionId)
        {
            aOut << ", transaction " << *source.mTransactionId;
        }

        if (source.mSourceValue)
        {
            aOut << ", shares " << *source.mSourceValue << " (the broker's own value, not a ratio)";
        }

        aOut << '\n';
    }

    if (aQuestion.mPositionBefore)
    {
        aOut << "  You held " << unitsText(*aQuestion.mPositionBefore)
             << " units just before it.\n";
    }
    else
    {
        aOut << "  The sale on " << formatCalendarDate(aQuestion.mEarlierSaleDate.value())
             << " needs more units than you held. If the action took effect on\n"
                "  or before that sale, enter it; if not, cancel: earlier purchases are then "
                "missing.\n";
    }

    aOut << kRatioHelp;
}

/// Two whole numbers from the user: a ratio, or two share counts in ticks of 10^-8 units.
struct EnteredRatio {
    std::int64_t mNewShares{};
    std::int64_t mOldShares{};
};

/// What a line of the ratio prompt says: a cancel, two numbers, or the text to show before the
/// prompt is asked again.
using RatioInput = std::variant<CancelAnswer, EnteredRatio, std::string>;

/// A whole number without a sign: `10`.
std::optional<std::int64_t> wholeNumber(std::string_view aText) {
    const auto text = trimmed(aText);
    std::int64_t value{};
    const auto* end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, value);

    if (text.empty() || text.front() == '-' || parsed.ec != std::errc{} || parsed.ptr != end)
    {
        return std::nullopt;
    }

    return value;
}

/// A share count above 0 with up to 8 decimals, in ticks of 10^-8 units: `0,52` and `0.52` are
/// 52000000. The decimal separator is a comma or a point, never both, and there is no digit
/// grouping: `1,234` is 1.234, and `1.000,5` is refused.
std::optional<std::int64_t> shareCount(std::string_view aText) {
    std::string text{aText};

    if (text.find(',') != std::string::npos && text.find('.') != std::string::npos)
    {
        return std::nullopt;
    }

    std::replace(text.begin(), text.end(), ',', '.');

    const auto imported = importFixedPoint(text, UNITS_SCALE, ValuePolicy::Positive, true, false);
    const auto* parsed = std::get_if<ParsedFixedPoint>(&imported);

    if (parsed == nullptr)
    {
        return std::nullopt;
    }

    return parsed->mValue;
}

/// Writes the quantity question: the sale, its units and the units the earlier purchases left.
void writeQuantityQuestion(std::ostream& aOut,
                           const DecisionRequest& aRequest,
                           const QuantityCorrectionQuestion& aQuestion) {
    aOut
        << "Question " << aRequest.mId << ": the sale of "
        << investmentText(aQuestion.mIsin, aQuestion.mName) << " on "
        << formatCalendarDate(aQuestion.mSaleDate) << " is " << unitsText(aQuestion.mOriginalUnits)
        << " units, but your earlier purchases left only " << unitsText(aQuestion.mOpenUnits)
        << " units.\n"
        << "  The difference is below 0.00000100 units, but the files do not show that it is\n"
           "  rounding. Enter the quantity of the sale to use (at most the units left, with up to\n"
           "  8 decimals), press Enter for the units left, or cancel: no answer, and the\n"
           "  investment is blocked.\n";
}

/// Reads a sale quantity with up to 8 decimals, Enter for the units left, or `cancel`; asks again
/// on anything else. Absent when the input ends.
std::optional<AnswerValue> readQuantityAnswer(const QuantityCorrectionQuestion& aQuestion,
                                              std::istream& aIn,
                                              std::ostream& aOut) {
    while (true)
    {
        const auto line = readLine(aIn, aOut, "Your answer (a quantity, Enter or cancel):");

        if (!line)
        {
            return std::nullopt;
        }

        if (lowerCase(*line) == "cancel")
        {
            return CancelAnswer{};
        }

        if (line->empty())
        {
            return QuantityCorrectionAnswer{.mCorrectedUnits = aQuestion.mOpenUnits};
        }

        const auto units = shareCount(*line);

        if (units)
        {
            return QuantityCorrectionAnswer{.mCorrectedUnits = *units};
        }

        aOut << "Enter a quantity above 0 with at most 8 decimals, press Enter or answer "
                "cancel.\n";
    }
}

/// `new : old`, `before -> after` or `cancel`. The share counts become the ratio after / before,
/// as splitRatioFromShareCounts does.
RatioInput parseRatioInput(const std::string& aLine) {
    if (lowerCase(aLine) == "cancel")
    {
        return CancelAnswer{};
    }

    const std::string_view line{aLine};

    if (const auto arrow = line.find("->"); arrow != std::string_view::npos)
    {
        const auto before = shareCount(line.substr(0, arrow));
        const auto after = shareCount(line.substr(arrow + 2));

        if (!before || !after)
        {
            return std::string{
                "Enter the shares as numbers above 0 with up to 8 decimals, for example 0,52 -> "
                "5,2."};
        }

        return EnteredRatio{.mNewShares = *after, .mOldShares = *before};
    }

    if (const auto colon = line.find(':'); colon != std::string_view::npos)
    {
        const auto newShares = wholeNumber(line.substr(0, colon));
        const auto oldShares = wholeNumber(line.substr(colon + 1));

        if (!newShares || !oldShares)
        {
            return std::string{"Enter whole numbers, for example 10 : 1."};
        }

        return EnteredRatio{.mNewShares = *newShares, .mOldShares = *oldShares};
    }

    return std::string{"Enter the ratio as new : old, the shares as before -> after, or cancel."};
}

/// The dates the action may have: `2022-01-11 to 2025-03-10`, or with several ranges each with
/// its units, `2024-01-11 to 2024-11-12 with 1.0000 units, or 2024-11-13 to 2024-12-31 with
/// 1.1000 units`.
std::string dateRangesText(const CorporateActionRatioQuestion& aQuestion) {
    std::string text;

    for (const auto& range : aQuestion.mDateRanges)
    {
        if (!text.empty())
        {
            text += ", or ";
        }

        text += formatCalendarDate(range.mEarliestDate) + " to " +
                formatCalendarDate(range.mLatestDate);

        if (aQuestion.mDateRanges.size() > 1)
        {
            text += " with " + unitsText(range.mPositionBefore) + " units";
        }
    }

    return text;
}

/// The effective date from the announcement; an empty line takes the date of the broker's row
/// when that date is in a range. Empty when the input has ended.
std::optional<Date> readEffectiveDate(const CorporateActionRatioQuestion& aQuestion,
                                      std::istream& aIn,
                                      std::ostream& aOut) {
    const bool rowDateAllowed = findEffectiveDateRange(aQuestion, aQuestion.mEffectiveDate);
    const auto prompt =
        "Effective date from the announcement (YYYY-MM-DD, " + dateRangesText(aQuestion) + ")" +
        (rowDateAllowed ? ", or empty for " + formatCalendarDate(aQuestion.mEffectiveDate) : "") +
        ":";

    while (true)
    {
        const auto line = readLine(aIn, aOut, prompt);

        if (!line)
        {
            return std::nullopt;
        }

        if (line->empty() && rowDateAllowed)
        {
            return aQuestion.mEffectiveDate;
        }

        if (const auto date = parseCalendarDate(*line))
        {
            return *date;
        }

        aOut << "Enter the date as YYYY-MM-DD.\n";
    }
}

/// yes or no; empty when the input has ended.
std::optional<bool> readConfirmation(std::istream& aIn, std::ostream& aOut) {
    while (true)
    {
        const auto line = readLine(aIn, aOut, "Is this right? (yes or no):");

        if (!line)
        {
            return std::nullopt;
        }

        const auto word = lowerCase(*line);

        if (word == "yes" || word == "no")
        {
            return word == "yes";
        }

        aOut << "Answer yes or no.\n";
    }
}

/// For rows of different brokers on different dates: yes, no or cancel. Empty when the input has
/// ended.
std::optional<std::variant<bool, CancelAnswer>> readSameEvent(std::istream& aIn,
                                                              std::ostream& aOut) {
    aOut << "  The rows are on different dates. If both brokers booked the same split, answer yes\n"
            "  and enter it once. If they are two splits, answer no: two splits so close together\n"
            "  are not supported yet, and the sales cannot be reported.\n";

    while (true)
    {
        const auto line = readLine(aIn, aOut, "Are these rows one split? (yes, no or cancel):");

        if (!line)
        {
            return std::nullopt;
        }

        const auto word = lowerCase(*line);

        if (word == "yes" || word == "no")
        {
            return word == "yes";
        }

        if (word == "cancel")
        {
            return CancelAnswer{};
        }

        aOut << "Answer yes, no or cancel.\n";
    }
}

/// Reads the ratio, the effective date and the confirmation of the preview, after asking whether
/// rows on different dates are one split. A ratio without a preview (a number 0, equal numbers, a
/// position that does not fit, a date in no range) is passed on as it is, so the session's
/// rejection says what is wrong.
std::optional<AnswerValue> readRatioAnswer(const CorporateActionRatioQuestion& aQuestion,
                                           std::istream& aIn,
                                           std::ostream& aOut) {
    if (aQuestion.mAskSameEvent)
    {
        const auto sameEvent = readSameEvent(aIn, aOut);

        if (!sameEvent)
        {
            return std::nullopt;
        }

        if (std::holds_alternative<CancelAnswer>(*sameEvent))
        {
            return CancelAnswer{};
        }

        if (!std::get<bool>(*sameEvent))
        {
            return SeparateEventsAnswer{};
        }
    }

    while (true)
    {
        const auto line = readLine(aIn, aOut, "Ratio:");

        if (!line)
        {
            return std::nullopt;
        }

        const auto input = parseRatioInput(*line);

        if (const auto* message = std::get_if<std::string>(&input))
        {
            aOut << *message << '\n';
            continue;
        }

        if (std::holds_alternative<CancelAnswer>(input))
        {
            return CancelAnswer{};
        }

        const auto entered = std::get<EnteredRatio>(input);
        const auto ratio = makeSplitRatio(entered.mNewShares, entered.mOldShares);

        if (!ratio)
        {
            return SplitRatioAnswer{
                .mNewShares = entered.mNewShares,
                .mOldShares = entered.mOldShares,
                .mEffectiveDate = aQuestion.mEffectiveDate,
            };
        }

        const auto date = readEffectiveDate(aQuestion, aIn, aOut);

        if (!date)
        {
            return std::nullopt;
        }

        // A date in no range has no preview; the session then rejects the date.
        const auto* range = findEffectiveDateRange(aQuestion, *date);
        const auto preview =
            range != nullptr ? previewSplitUnits(range->mPositionBefore, *ratio) : std::nullopt;

        if (!preview)
        {
            return SplitRatioAnswer{
                .mNewShares = entered.mNewShares,
                .mOldShares = entered.mOldShares,
                .mEffectiveDate = *date,
            };
        }

        if (isUnusualSplitRatio(*ratio))
        {
            aOut << "This ratio looks unusual: check the numbers.\n";
        }

        aOut << "After it you hold " << unitsText(*preview) << " units (" << ratio->mNewShares
             << " : " << ratio->mOldShares << " from " << formatCalendarDate(*date) << ").\n";

        const auto confirmed = readConfirmation(aIn, aOut);

        if (!confirmed)
        {
            return std::nullopt;
        }

        if (*confirmed)
        {
            return SplitRatioAnswer{
                .mNewShares = entered.mNewShares,
                .mOldShares = entered.mOldShares,
                .mEffectiveDate = *date,
                .mConfirmedUnits = *preview,
            };
        }
    }
}

/// The question about the type of a bond that was sold or matured in the year.
void writeBondTypeQuestion(std::ostream& aOut,
                           const DecisionRequest& aRequest,
                           const BondTypeQuestion& aQuestion) {
    aOut << "Question " << aRequest.mId << ": the type of the bond "
         << investmentText(aQuestion.mIsin, aQuestion.mName) << ": sold "
         << unitsText(aQuestion.mSoldUnits) << " and matured " << unitsText(aQuestion.mMaturedUnits)
         << " units in the year";

    if (aQuestion.mMaturityDate)
    {
        aOut << " (maturity " << formatCalendarDate(*aQuestion.mMaturityDate) << ")";
    }

    aOut << ".\n"
            "  ordinary: pays coupons; they are Doh-Obr rows.\n"
            "  discount: issued below its nominal value (discount or zero-coupon); its interest\n"
            "  on the sale or maturity is not calculated yet, so Doh-Obr is blocked.\n"
            "  cancel: no answer, and no Doh-Obr.\n";
}

std::optional<AnswerValue> readBondTypeAnswer(std::istream& aIn, std::ostream& aOut) {
    while (true)
    {
        const auto line = readLine(aIn, aOut, "Your answer (ordinary, discount or cancel):");

        if (!line)
        {
            return std::nullopt;
        }

        const auto word = lowerCase(*line);

        if (word == "ordinary")
        {
            return BondTypeAnswer{BondType::Ordinary};
        }

        if (word == "discount")
        {
            return BondTypeAnswer{BondType::DiscountOrZeroCoupon};
        }

        if (word == "cancel")
        {
            return CancelAnswer{};
        }

        aOut << "Answer ordinary, discount or cancel.\n";
    }
}

/// The question whether the year's deposit interest, with the other banks, is above the allowance.
void writeDepositQuestion(std::ostream& aOut,
                          const DecisionRequest& aRequest,
                          const DepositInterestTotalQuestion& aQuestion) {
    aOut
        << "Question " << aRequest.mId << ": the bank-deposit interest imported for "
        << aQuestion.mYear << " is EUR " << formatFixedPoint(aQuestion.mTotal, MONEY_SCALE, 2)
        << ", not above the EUR 1,000 allowance.\n"
        << "  Is your bank-deposit interest from all Slovenian and EU banks, with the interest of\n"
           "  your other banks, above it? yes: make Doh-DHO from the imported interest.\n"
           "  no: no Doh-DHO.\n";
}

std::optional<AnswerValue> readDepositAnswer(std::istream& aIn, std::ostream& aOut) {
    while (true)
    {
        const auto line = readLine(aIn, aOut, "Your answer (yes or no):");

        if (!line)
        {
            return std::nullopt;
        }

        const auto word = lowerCase(*line);

        if (word == "yes")
        {
            return DepositInterestTotalAnswer{.mAboveLimit = true};
        }

        if (word == "no")
        {
            return DepositInterestTotalAnswer{.mAboveLimit = false};
        }

        aOut << "Answer yes or no.\n";
    }
}

/// The question for the treaty text of the dividends of one payer country.
void writeTreatyQuestion(std::ostream& aOut,
                         const DecisionRequest& aRequest,
                         const TreatyClaimQuestion& aQuestion) {
    aOut << "Question " << aRequest.mId << ": treaty relief on the dividends from "
         << aQuestion.mCountry << " in " << aQuestion.mYear << '\n'
         << "  Foreign tax was withheld on "
         << countText(aQuestion.mIsins.size(), "investment", "investments") << ":";

    for (const auto& isin : aQuestion.mIsins)
    {
        aOut << ' ' << isin;
    }

    aOut << "\n  The reference files have no treaty text for this country.\n"
            "  A claim writes your text as ReliefStatement in Doh-Div. You check in eDavki that\n"
            "  the treaty applies to you and attach the evidence of the tax.\n"
            "  Without a claim the dividends keep false, and FURS may credit less of the foreign "
            "tax.\n"
            "  other: claim with a text you type. no: no claim.\n";
}

std::optional<AnswerValue> readTreatyAnswer(std::istream& aIn, std::ostream& aOut) {
    while (true)
    {
        const auto line = readLine(aIn, aOut, "Your answer (other or no):");

        if (!line)
        {
            return std::nullopt;
        }

        const auto word = lowerCase(*line);

        if (word == "no")
        {
            return TreatyClaimAnswer{.mStatement = std::nullopt};
        }

        if (word != "other")
        {
            aOut << "Answer other or no.\n";
            continue;
        }

        const auto text = readLine(aIn, aOut, "Treaty text (1 to 100 characters):");

        if (!text)
        {
            return std::nullopt;
        }

        if (isValidTreatyStatement(*text))
        {
            return TreatyClaimAnswer{.mStatement = *text};
        }

        aOut << "The text must have 1 to " << kMaxTreatyStatementLength
             << " characters, no control characters, and not be true or false.\n";
    }
}

/// The question which of several reference rows is the payer of a dividend.
void writePayerQuestion(std::ostream& aOut,
                        const DecisionRequest& aRequest,
                        const PayerChoiceQuestion& aQuestion) {
    aOut << "Question " << aRequest.mId << ": the payer of "
         << investmentText(aQuestion.mIsin, aQuestion.mName)
         << " has several reference rows that differ:\n";

    for (std::size_t index = 0; index < aQuestion.mOptions.size(); ++index)
    {
        const auto& option = aQuestion.mOptions[index];

        aOut << "  " << index + 1 << ": " << option.mName << ", "
             << (option.mAddress.empty() ? "(no address)" : option.mAddress) << ", "
             << option.mCountry << '\n';
    }

    aOut << "  Enter the number of the row, or skip: the payer facts stay empty and you complete\n"
            "  them in eDavki. The report is made either way.\n";
}

std::optional<AnswerValue>
readPayerAnswer(const PayerChoiceQuestion& aQuestion, std::istream& aIn, std::ostream& aOut) {
    while (true)
    {
        const auto line = readLine(aIn, aOut, "Your answer (a row number or skip):");

        if (!line)
        {
            return std::nullopt;
        }

        const auto word = lowerCase(*line);

        if (word == "skip")
        {
            return PayerChoiceAnswer{.mPickedOption = std::nullopt};
        }

        const auto number = wholeNumber(word);

        if (number && *number >= 1 &&
            *number <= static_cast<std::int64_t>(aQuestion.mOptions.size()))
        {
            return PayerChoiceAnswer{.mPickedOption = static_cast<std::size_t>(*number - 1)};
        }

        aOut << "Answer a row number from 1 to " << aQuestion.mOptions.size() << " or skip.\n";
    }
}

/// The question what an income payment of unknown type is.
void writeIncomeClassificationQuestion(std::ostream& aOut,
                                       const DecisionRequest& aRequest,
                                       const IncomeClassificationQuestion& aQuestion) {
    std::string labels;

    for (const auto& label : aQuestion.mClassLabels)
    {
        labels += (labels.empty() ? "" : ", ") + (label.empty() ? std::string{"(blank)"} : label);
    }

    aOut << "Question " << aRequest.mId << ": the "
         << (aQuestion.mEventKind == IncomeEventKind::Dividend ? "dividend" : "interest")
         << " payment of " << investmentText(aQuestion.mIsin.value_or(""), aQuestion.mName)
         << " on " << formatCalendarDate(aQuestion.mDate) << ", "
         << formatFixedPoint(aQuestion.mGrossAmount, MONEY_SCALE, 2) << ' '
         << currencyCode(aQuestion.mCurrency) << ", has a type that is missing, not recognized or\n"
         << "  differs between rows (class in the files: " << (labels.empty() ? "none" : labels)
         << "). What is it?\n";

    if (aQuestion.offers(IncomeTreatment::Dividends))
    {
        aOut << "  dividends: a dividend (Doh-Div).\n";
    }

    aOut << "  bank_deposit_interest: interest on a bank deposit (Doh-DHO).\n"
            "  broker_cash_interest: interest on cash held by a broker (Doh-Obr).\n";

    if (aQuestion.offers(IncomeTreatment::BondCoupon))
    {
        aOut << "  bond_coupon: a bond coupon (Doh-Obr).\n";
    }

    aOut << "  exclude: leave it out of the reports; the summary lists it, and you report it\n"
            "  yourself if it is taxable.\n"
            "  cancel: no answer, and the forms it may belong to are blocked.\n";
}

std::optional<AnswerValue> readIncomeClassificationAnswer(
    const IncomeClassificationQuestion& aQuestion, std::istream& aIn, std::ostream& aOut) {
    while (true)
    {
        const auto line = readLine(aIn, aOut, "Your answer (one of the words above):");

        if (!line)
        {
            return std::nullopt;
        }

        const auto word = lowerCase(*line);

        if (word == "cancel")
        {
            return CancelAnswer{};
        }

        if (word == "exclude")
        {
            return ExcludeFromReportsAnswer{};
        }

        std::optional<IncomeTreatment> treatment;

        if (word == "dividends")
        {
            treatment = IncomeTreatment::Dividends;
        }
        else if (word == "bank_deposit_interest")
        {
            treatment = IncomeTreatment::BankDepositInterest;
        }
        else if (word == "broker_cash_interest")
        {
            treatment = IncomeTreatment::BrokerCashInterest;
        }
        else if (word == "bond_coupon")
        {
            treatment = IncomeTreatment::BondCoupon;
        }

        if (treatment && aQuestion.offers(*treatment))
        {
            return IncomeTreatmentAnswer{.mTreatment = *treatment};
        }

        aOut << "Answer one of the words above.\n";
    }
}

/// Shows one question and reads its answer. Empty when the input has ended. Throws
/// `std::logic_error` for a question that canAsk() keeps out.
std::optional<AnswerValue>
askQuestion(const DecisionRequest& aRequest, std::istream& aIn, std::ostream& aOut) {
    const auto ask = [&aRequest, &aIn, &aOut](const auto& aQuestion) -> std::optional<AnswerValue> {
        using Question = std::decay_t<decltype(aQuestion)>;

        if constexpr (std::is_same_v<Question, std::monostate>)
        {
            // canAsk() keeps it out: a question without data cannot be answered.
            throw std::logic_error("this question cannot be asked");
        }
        else if constexpr (std::is_same_v<Question, PayerChoiceQuestion>)
        {
            writePayerQuestion(aOut, aRequest, aQuestion);
            return readPayerAnswer(aQuestion, aIn, aOut);
        }
        else if constexpr (std::is_same_v<Question, TreatyClaimQuestion>)
        {
            writeTreatyQuestion(aOut, aRequest, aQuestion);
            return readTreatyAnswer(aIn, aOut);
        }
        else if constexpr (std::is_same_v<Question, DepositInterestTotalQuestion>)
        {
            writeDepositQuestion(aOut, aRequest, aQuestion);
            return readDepositAnswer(aIn, aOut);
        }
        else if constexpr (std::is_same_v<Question, IncomeClassificationQuestion>)
        {
            writeIncomeClassificationQuestion(aOut, aRequest, aQuestion);
            return readIncomeClassificationAnswer(aQuestion, aIn, aOut);
        }
        else if constexpr (std::is_same_v<Question, BondTypeQuestion>)
        {
            writeBondTypeQuestion(aOut, aRequest, aQuestion);
            return readBondTypeAnswer(aIn, aOut);
        }
        else if constexpr (std::is_same_v<Question, FailureResolutionQuestion>)
        {
            writeFailureQuestion(aOut, aRequest);
            return readFailureAnswer(aIn, aOut);
        }
        else if constexpr (std::is_same_v<Question, LossEligibilityQuestion>)
        {
            writeLossQuestion(aOut, aRequest, aQuestion);
            return readLossAnswer(aIn, aOut);
        }
        else if constexpr (std::is_same_v<Question, TaxClassificationQuestion>)
        {
            writeClassificationQuestion(aOut, aRequest, aQuestion);
            return readClassificationAnswer(aIn, aOut);
        }
        else if constexpr (std::is_same_v<Question, QuantityCorrectionQuestion>)
        {
            writeQuantityQuestion(aOut, aRequest, aQuestion);
            return readQuantityAnswer(aQuestion, aIn, aOut);
        }
        else
        {
            static_assert(std::is_same_v<Question, CorporateActionRatioQuestion>);
            writeRatioQuestion(aOut, aRequest, aQuestion);
            return readRatioAnswer(aQuestion, aIn, aOut);
        }
    };

    return std::visit(ask, aRequest.mData);
}

std::string_view failureResolutionText(FailureResolution aChoice) {
    switch (aChoice)
    {
    case FailureResolution::ExcludeFailedInstruments:
        return "leave the failed investments out";
    case FailureResolution::FailReport:
        break;
    }

    return "block the report";
}

/// The answer as the summary shows it.
std::string answerText(const AnswerValue& aAnswer) {
    const auto text = [](const auto& aValue) -> std::string {
        using Value = std::decay_t<decltype(aValue)>;

        if constexpr (std::is_same_v<Value, FailureResolutionAnswer>)
        {
            return std::string{failureResolutionText(aValue.mChoice)};
        }
        else if constexpr (std::is_same_v<Value, CancelAnswer>)
        {
            return "cancelled";
        }
        else if constexpr (std::is_same_v<Value, NoOtherActivityAnswer>)
        {
            return "no, not bought or sold anywhere else (the open loss is deductible)";
        }
        else if constexpr (std::is_same_v<Value, OtherActivityAnswer>)
        {
            return "yes, bought or sold elsewhere (the open loss is not deductible, F10 false)";
        }
        else if constexpr (std::is_same_v<Value, SeparateEventsAnswer>)
        {
            return "no, the rows are two splits";
        }
        else if constexpr (std::is_same_v<Value, CapitalGainsAnswer>)
        {
            return "report it as shares or fund units (Doh-KDVP)";
        }
        else if constexpr (std::is_same_v<Value, ExcludeFromReportsAnswer>)
        {
            return "leave it out of the reports";
        }
        else if constexpr (std::is_same_v<Value, QuantityCorrectionAnswer>)
        {
            return "sale quantity " + unitsText(aValue.mCorrectedUnits) + " units";
        }
        else if constexpr (std::is_same_v<Value, DepositInterestTotalAnswer>)
        {
            return aValue.mAboveLimit ? "above EUR 1,000 with the other banks"
                                      : "not above EUR 1,000 with the other banks";
        }
        else if constexpr (std::is_same_v<Value, IncomeTreatmentAnswer>)
        {
            switch (aValue.mTreatment)
            {
            case IncomeTreatment::Dividends:
                return "a dividend (Doh-Div)";
            case IncomeTreatment::BankDepositInterest:
                return "bank-deposit interest (Doh-DHO)";
            case IncomeTreatment::BrokerCashInterest:
                return "interest on broker cash (Doh-Obr)";
            case IncomeTreatment::BondCoupon:
                break;
            }

            return "a bond coupon (Doh-Obr)";
        }
        else if constexpr (std::is_same_v<Value, BondTypeAnswer>)
        {
            return aValue.mType == BondType::Ordinary
                       ? "an ordinary bond"
                       : "a discount or zero-coupon bond (issued below its nominal value)";
        }
        else if constexpr (std::is_same_v<Value, TreatyClaimAnswer>)
        {
            return aValue.mStatement ? "treaty relief claimed: " + *aValue.mStatement
                                     : std::string{"no treaty claim"};
        }
        else if constexpr (std::is_same_v<Value, PayerChoiceAnswer>)
        {
            return aValue.mPickedOption ? "payer row " + std::to_string(*aValue.mPickedOption + 1)
                                        : std::string{"skipped, the payer is completed in eDavki"};
        }
        else
        {
            static_assert(std::is_same_v<Value, SplitRatioAnswer>);
            return std::to_string(aValue.mNewShares) + " : " + std::to_string(aValue.mOldShares) +
                   " from " + formatCalendarDate(aValue.mEffectiveDate) + ", " +
                   unitsText(aValue.mConfirmedUnits) + " units after it";
        }
    };

    return std::visit(text, aAnswer);
}

} // namespace

bool canAsk(const QuestionData& aData) {
    const auto askable = [](const auto& aQuestion) {
        using Question = std::decay_t<decltype(aQuestion)>;

        if constexpr (std::is_same_v<Question, std::monostate>)
        {
            return false;
        }
        else if constexpr (std::is_same_v<Question, FailureResolutionQuestion>)
        {
            return true;
        }
        else if constexpr (std::is_same_v<Question, LossEligibilityQuestion>)
        {
            return true;
        }
        else if constexpr (std::is_same_v<Question, TaxClassificationQuestion>)
        {
            return true;
        }
        else if constexpr (std::is_same_v<Question, QuantityCorrectionQuestion>)
        {
            return true;
        }
        else if constexpr (std::is_same_v<Question, PayerChoiceQuestion> ||
                           std::is_same_v<Question, TreatyClaimQuestion> ||
                           std::is_same_v<Question, DepositInterestTotalQuestion> ||
                           std::is_same_v<Question, IncomeClassificationQuestion> ||
                           std::is_same_v<Question, BondTypeQuestion>)
        {
            return true;
        }
        else
        {
            static_assert(std::is_same_v<Question, CorporateActionRatioQuestion>);
            return true;
        }
    };

    return std::visit(askable, aData);
}

namespace {

/// The ISINs that the failure question lists, empty when there is none.
std::set<Isin> failedIsins(const std::vector<DecisionRequest>& aRequests) {
    std::set<Isin> isins;

    for (const auto& request : aRequests)
    {
        for (const auto& investment : request.mInvestments)
        {
            isins.insert(investment.mIsin);
        }
    }

    return isins;
}

/// The next request to ask, or the end. First a classification question. Then the questions of
/// investments that the failure question does not list, so an answer that adds an investment to its
/// list comes before it. Then the failure question. A question of a listed investment is asked
/// after it, and only if the investment is not excluded, as exclusion drops the question.
std::vector<DecisionRequest>::const_iterator
nextQuestion(const std::vector<DecisionRequest>& aRequests) {
    const auto isFailure = [](const DecisionRequest& aRequest) {
        return std::holds_alternative<FailureResolutionQuestion>(aRequest.mData);
    };
    const auto isClassification = [](const DecisionRequest& aRequest) {
        return std::holds_alternative<TaxClassificationQuestion>(aRequest.mData);
    };
    const auto classification = std::find_if(aRequests.begin(), aRequests.end(), isClassification);

    // An exclusion drops the loss and ratio questions of its investment, so it is asked first.
    if (classification != aRequests.end())
    {
        return classification;
    }

    const auto listed = failedIsins(aRequests);
    const auto isOtherQuestion = [&isFailure, &listed](const DecisionRequest& aRequest) {
        return canAsk(aRequest.mData) && !isFailure(aRequest) &&
               !(aRequest.mIsin && listed.contains(*aRequest.mIsin));
    };
    const auto other = std::find_if(aRequests.begin(), aRequests.end(), isOtherQuestion);

    if (other != aRequests.end())
    {
        return other;
    }

    return std::find_if(aRequests.begin(), aRequests.end(), isFailure);
}

} // namespace

void askOpenQuestions(DecisionSession& aSession, std::istream& aIn, std::ostream& aOut) {
    bool asked = false;

    while (true)
    {
        const auto& requests = aSession.result().mRequests;
        const auto request = nextQuestion(requests);

        if (request == requests.end())
        {
            break;
        }

        aOut << (asked ? "\n" : "");
        asked = true;

        auto answer = askQuestion(*request, aIn, aOut);

        if (!answer)
        {
            aOut << "Input ended: the open questions are listed in the summary.\n";
            break;
        }

        // The answer replaces the result and its requests, so the ID is copied first.
        std::vector<DecisionAnswer> answers{DecisionAnswer{
            .mRequestId = request->mId,
            .mRunRevision = aSession.result().mRunRevision,
            .mValue = std::move(*answer),
        }};
        const auto rejections = aSession.answer(std::move(answers));

        for (const auto& rejection : rejections)
        {
            aOut << rejection.mMessage << '\n';
        }
    }

    aOut << (asked ? "\n" : "");
}

std::string answerLine(const RecordedDecision& aDecision) {
    const auto questionText = [&aDecision](const auto& aQuestion) -> std::string {
        using Question = std::decay_t<decltype(aQuestion)>;

        if constexpr (std::is_same_v<Question, std::monostate>)
        {
            return std::string{api::requestTypeName(aDecision.mType)};
        }
        else if constexpr (std::is_same_v<Question, FailureResolutionQuestion>)
        {
            return countText(aQuestion.mInvestments.size(),
                             "failed investment",
                             "failed investments");
        }
        else if constexpr (std::is_same_v<Question, LossEligibilityQuestion>)
        {
            return investmentText(aQuestion.mIsin, aQuestion.mName) + ", 30-day loss rule";
        }
        else if constexpr (std::is_same_v<Question, TaxClassificationQuestion>)
        {
            return investmentText(aQuestion.mIsin, aQuestion.mName) + ", asset class";
        }
        else if constexpr (std::is_same_v<Question, QuantityCorrectionQuestion>)
        {
            return investmentText(aQuestion.mIsin, aQuestion.mName) + ", sale on " +
                   formatCalendarDate(aQuestion.mSaleDate);
        }
        else if constexpr (std::is_same_v<Question, PayerChoiceQuestion>)
        {
            return investmentText(aQuestion.mIsin, aQuestion.mName) + ", payer";
        }
        else if constexpr (std::is_same_v<Question, TreatyClaimQuestion>)
        {
            return aQuestion.mCountry + ", treaty claim";
        }
        else if constexpr (std::is_same_v<Question, DepositInterestTotalQuestion>)
        {
            return "bank-deposit interest " + std::to_string(aQuestion.mYear);
        }
        else if constexpr (std::is_same_v<Question, IncomeClassificationQuestion>)
        {
            return investmentText(aQuestion.mIsin.value_or(""), aQuestion.mName) + ", payment on " +
                   formatCalendarDate(aQuestion.mDate);
        }
        else if constexpr (std::is_same_v<Question, BondTypeQuestion>)
        {
            return investmentText(aQuestion.mIsin, aQuestion.mName) + ", bond type";
        }
        else
        {
            static_assert(std::is_same_v<Question, CorporateActionRatioQuestion>);
            auto text = investmentText(aQuestion.mIsin, aQuestion.mName) +
                        ", corporate action on " + formatCalendarDate(aQuestion.mEffectiveDate);

            if (!aQuestion.mAskSameEvent)
            {
                return text;
            }

            // The sources are in date order, so a new date is a new row date.
            std::vector<Date> dates;

            for (const auto& source : aQuestion.mSources)
            {
                if (dates.empty() || dates.back() != source.mDate)
                {
                    dates.push_back(source.mDate);
                }
            }

            text += " (rows of ";

            for (std::size_t index = 0; index < dates.size(); ++index)
            {
                text += (index == 0 ? "" : " and ") + formatCalendarDate(dates[index]);
            }

            return text + ")";
        }
    };
    const auto* ratio = std::get_if<CorporateActionRatioQuestion>(&aDecision.mData);

    // A ratio for rows on two dates also says that they are one split.
    const bool oneSplit = ratio != nullptr && ratio->mAskSameEvent &&
                          std::holds_alternative<SplitRatioAnswer>(aDecision.mAnswer);

    return std::visit(questionText, aDecision.mData) + ": " + (oneSplit ? "one split, " : "") +
           answerText(aDecision.mAnswer);
}

} // namespace taxbroker::cli
