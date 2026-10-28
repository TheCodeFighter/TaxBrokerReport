#include "cli/decision_prompts.hpp"

#include "cli_text.hpp"
#include "taxbroker/api/application_result_json.hpp"
#include "taxbroker/split_ratio.hpp"
#include "utils/date_utils.hpp"
#include "utils/numeric_util.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <optional>
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

std::string_view brokerText(Broker aBroker) {
    switch (aBroker)
    {
    case Broker::TradeRepublic:
        return "Trade Republic";
    case Broker::InteractiveBrokers:
        return "Interactive Brokers";
    case Broker::Unknown:
        break;
    }

    return "an unknown broker";
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

        aOut << ", purchases in your files that count against it:";

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

void writeRatioQuestion(std::ostream& aOut,
                        const DecisionRequest& aRequest,
                        const CorporateActionRatioQuestion& aQuestion) {
    aOut << "Question " << aRequest.mId << ": a corporate action of "
         << investmentText(aQuestion.mIsin, aQuestion.mName) << " on "
         << formatCalendarDate(aQuestion.mEffectiveDate) << '\n';

    for (const auto& source : aQuestion.mSources)
    {
        aOut << "  Source: " << brokerText(source.mBroker);

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

    aOut << "  You held " << unitsText(aQuestion.mPositionBefore) << " units just before it.\n"
         << kRatioHelp;
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

/// The effective date from the announcement; an empty line takes the date of the broker's row.
/// Empty when the input has ended.
std::optional<Date> readEffectiveDate(const CorporateActionRatioQuestion& aQuestion,
                                      std::istream& aIn,
                                      std::ostream& aOut) {
    const auto prompt = "Effective date from the announcement (YYYY-MM-DD, " +
                        formatCalendarDate(aQuestion.mEarliestDate) + " to " +
                        formatCalendarDate(aQuestion.mLatestDate) + "), or empty for " +
                        formatCalendarDate(aQuestion.mEffectiveDate) + ":";

    while (true)
    {
        const auto line = readLine(aIn, aOut, prompt);

        if (!line)
        {
            return std::nullopt;
        }

        if (line->empty())
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

/// Reads the ratio, the effective date and the confirmation of the preview. A ratio without a
/// preview (a number 0, equal numbers, a position that does not fit) is passed on as it is, so the
/// session's rejection says what is wrong.
std::optional<AnswerValue> readRatioAnswer(const CorporateActionRatioQuestion& aQuestion,
                                           std::istream& aIn,
                                           std::ostream& aOut) {
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

        const auto preview = previewSplitUnits(aQuestion.mPositionBefore, *ratio);

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

/// Shows one question and reads its answer. Empty when the input has ended. Throws
/// `std::logic_error` for a question that canAsk() keeps out.
std::optional<AnswerValue>
askQuestion(const DecisionRequest& aRequest, std::istream& aIn, std::ostream& aOut) {
    const auto ask = [&aRequest, &aIn, &aOut](const auto& aQuestion) -> std::optional<AnswerValue> {
        using Question = std::decay_t<decltype(aQuestion)>;

        if constexpr (std::is_same_v<Question, std::monostate>)
        {
            throw std::logic_error("a question without question data cannot be asked");
        }
        else if constexpr (std::is_same_v<Question, FailureResolutionQuestion>)
        {
            throw std::logic_error("the failure-resolution question is not asked yet");
        }
        else if constexpr (std::is_same_v<Question, LossEligibilityQuestion>)
        {
            writeLossQuestion(aOut, aRequest, aQuestion);
            return readLossAnswer(aIn, aOut);
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
            // No stage asks it yet; it comes with the ISIN exclusion.
            return false;
        }
        else if constexpr (std::is_same_v<Question, LossEligibilityQuestion>)
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

void askOpenQuestions(DecisionSession& aSession, std::istream& aIn, std::ostream& aOut) {
    bool asked = false;

    while (true)
    {
        const auto& requests = aSession.result().mRequests;
        const auto request =
            std::find_if(requests.begin(), requests.end(), [](const DecisionRequest& aRequest) {
                return canAsk(aRequest.mData);
            });

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
        else
        {
            static_assert(std::is_same_v<Question, CorporateActionRatioQuestion>);
            return investmentText(aQuestion.mIsin, aQuestion.mName) + ", corporate action on " +
                   formatCalendarDate(aQuestion.mEffectiveDate);
        }
    };

    return std::visit(questionText, aDecision.mData) + ": " + answerText(aDecision.mAnswer);
}

} // namespace taxbroker::cli
