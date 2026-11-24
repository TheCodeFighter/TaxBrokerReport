#include "taxbroker/api/answer_json.hpp"

#include "taxbroker/diagnostic_messages.hpp"
#include "taxbroker/text_format.hpp"

#include "json_scan.hpp"
#include "json_strings.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace taxbroker::api {

namespace {

using Json = nlohmann::json;
using Parsed = std::optional<AnswerValue>;

/// One word of the JSON and the answer it stands for.
struct Choice {
    std::string_view mWord;
    AnswerValue mAnswer;
};

/// The answer for a word of aChoices, or nothing when the JSON is not one of the words.
Parsed fromWords(const Json& aJson, std::initializer_list<Choice> aChoices) {
    if (!aJson.is_string())
    {
        return std::nullopt;
    }

    const std::string& word = aJson.get_ref<const std::string&>();
    const auto found =
        std::find_if(aChoices.begin(), aChoices.end(), [&word](const Choice& aChoice) {
            return aChoice.mWord == word;
        });

    if (found == aChoices.end())
    {
        return std::nullopt;
    }

    return found->mAnswer;
}

/// True when the object has exactly the keys of aKeys.
bool hasExactKeys(const Json& aObject, std::initializer_list<std::string_view> aKeys) {
    if (!aObject.is_object() || aObject.size() != aKeys.size())
    {
        return false;
    }

    return std::all_of(aKeys.begin(), aKeys.end(), [&aObject](std::string_view aKey) {
        return aObject.contains(aKey);
    });
}

/// Units from a text with at most 8 decimals; a JSON number would lose digits, so it is refused.
std::optional<Units> unitsFrom(const Json& aJson) {
    if (!aJson.is_string())
    {
        return std::nullopt;
    }

    const auto imported = importFixedPoint(aJson.get_ref<const std::string&>(),
                                           UNITS_SCALE,
                                           ValuePolicy::Signed,
                                           true,
                                           false);

    if (!std::holds_alternative<ParsedFixedPoint>(imported))
    {
        return std::nullopt;
    }

    return std::get<ParsedFixedPoint>(imported).mValue;
}

/// A whole number that fits int64.
std::optional<std::int64_t> wholeNumberFrom(const Json& aJson) {
    if (!aJson.is_number_integer())
    {
        return std::nullopt;
    }

    if (aJson.is_number_unsigned() &&
        aJson.get<std::uint64_t>() >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    {
        return std::nullopt;
    }

    return aJson.get<std::int64_t>();
}

Parsed ratioFrom(const Json& aJson) {
    if (!hasExactKeys(aJson, {"newShares", "oldShares", "effectiveDate", "confirmedUnits"}))
    {
        return std::nullopt;
    }

    const auto newShares = wholeNumberFrom(aJson["newShares"]);
    const auto oldShares = wholeNumberFrom(aJson["oldShares"]);
    const auto units = unitsFrom(aJson["confirmedUnits"]);

    if (!newShares || !oldShares || !units || !aJson["effectiveDate"].is_string())
    {
        return std::nullopt;
    }

    const auto date = parseCalendarDate(aJson["effectiveDate"].get_ref<const std::string&>());

    if (!date)
    {
        return std::nullopt;
    }

    return SplitRatioAnswer{
        .mNewShares = *newShares,
        .mOldShares = *oldShares,
        .mEffectiveDate = *date,
        .mConfirmedUnits = *units,
    };
}

/// The answer to an identity question: `{"type": "grouping", "actionOfRow": [1, 1, 2]}`, one
/// whole number from 1 for each row. The session checks the count and the numbers.
Parsed groupingFrom(const Json& aJson) {
    if (!hasExactKeys(aJson, {"type", "actionOfRow"}) || aJson["type"] != "grouping" ||
        !aJson["actionOfRow"].is_array())
    {
        return std::nullopt;
    }

    CorporateActionGroupingAnswer answer;

    for (const auto& number : aJson["actionOfRow"])
    {
        const auto whole = wholeNumberFrom(number);

        if (!whole || *whole < 1)
        {
            return std::nullopt;
        }

        answer.mActionOfRow.push_back(static_cast<std::size_t>(*whole));
    }

    return answer;
}

Parsed incomeTreatmentFrom(const Json& aJson) {
    if (aJson.is_object())
    {
        if (!hasExactKeys(aJson, {"choice", "subtype"}) || aJson["choice"] != "other_interest")
        {
            return std::nullopt;
        }

        if (aJson["subtype"] == "broker_cash")
        {
            return IncomeTreatmentAnswer{IncomeTreatment::BrokerCashInterest};
        }

        if (aJson["subtype"] == "bond_coupon")
        {
            return IncomeTreatmentAnswer{IncomeTreatment::BondCoupon};
        }

        return std::nullopt;
    }

    return fromWords(
        aJson,
        {{"dividends", IncomeTreatmentAnswer{IncomeTreatment::Dividends}},
         {"bank_deposit_interest", IncomeTreatmentAnswer{IncomeTreatment::BankDepositInterest}},
         {"exclude_from_supported_reports", ExcludeFromReportsAnswer{}}});
}

Parsed failureResolutionFrom(const Json& aJson, bool aOffersDeveloperExport) {
    Parsed answer =
        fromWords(aJson,
                  {{"exclude_failed_instruments",
                    FailureResolutionAnswer{FailureResolution::ExcludeFailedInstruments}},
                   {"fail_report", FailureResolutionAnswer{FailureResolution::FailReport}}});

    if (!answer && aOffersDeveloperExport)
    {
        answer = fromWords(
            aJson,
            {{"developer_export", FailureResolutionAnswer{FailureResolution::DeveloperExport}}});
    }

    return answer;
}

/// The answer for the question data of aRequest, or nothing when the JSON does not fit.
Parsed answerFor(const Json& aJson, const DecisionRequest& aRequest) {
    // Closing a question is possible for every type that has question data.
    if (!std::holds_alternative<std::monostate>(aRequest.mData) && aJson == "cancel")
    {
        return CancelAnswer{};
    }

    const auto fromData = [&aJson, &aRequest](const auto& aData) -> Parsed {
        using Data = std::decay_t<decltype(aData)>;

        if constexpr (std::is_same_v<Data, std::monostate>)
        {
            return std::nullopt;
        }
        else if constexpr (std::is_same_v<Data, FailureResolutionQuestion>)
        {
            return failureResolutionFrom(aJson,
                                         aRequest.mType ==
                                             RequestType::CapitalGainsFailureResolutionRequired);
        }
        else if constexpr (std::is_same_v<Data, LossEligibilityQuestion>)
        {
            return fromWords(aJson,
                             {{"no_other_activity", NoOtherActivityAnswer{}},
                              {"other_activity", OtherActivityAnswer{}}});
        }
        else if constexpr (std::is_same_v<Data, CorporateActionRatioQuestion>)
        {
            if (aJson.is_object())
            {
                return ratioFrom(aJson);
            }

            return fromWords(aJson, {{"separate_events", SeparateEventsAnswer{}}});
        }
        else if constexpr (std::is_same_v<Data, CorporateActionIdentityQuestion>)
        {
            return groupingFrom(aJson);
        }
        else if constexpr (std::is_same_v<Data, TaxClassificationQuestion>)
        {
            return fromWords(aJson,
                             {{"capital_gains", CapitalGainsAnswer{}},
                              {"exclude_from_supported_reports", ExcludeFromReportsAnswer{}}});
        }
        else if constexpr (std::is_same_v<Data, IncomeClassificationQuestion>)
        {
            return incomeTreatmentFrom(aJson);
        }
        else if constexpr (std::is_same_v<Data, QuantityCorrectionQuestion>)
        {
            if (!hasExactKeys(aJson, {"correctedUnits"}))
            {
                return std::nullopt;
            }

            const auto units = unitsFrom(aJson["correctedUnits"]);

            if (!units)
            {
                return std::nullopt;
            }

            return QuantityCorrectionAnswer{*units};
        }
        else if constexpr (std::is_same_v<Data, PayerChoiceQuestion>)
        {
            if (aJson == "skip")
            {
                return PayerChoiceAnswer{std::nullopt};
            }

            if (!hasExactKeys(aJson, {"option"}) || !aJson["option"].is_number_unsigned())
            {
                return std::nullopt;
            }

            return PayerChoiceAnswer{aJson["option"].get<std::size_t>()};
        }
        else if constexpr (std::is_same_v<Data, PayerCountryQuestion>)
        {
            if (aJson == "skip")
            {
                return PayerCountryAnswer{std::nullopt};
            }

            // The session checks that the string is a country code.
            if (!hasExactKeys(aJson, {"country"}) || !aJson["country"].is_string())
            {
                return std::nullopt;
            }

            return PayerCountryAnswer{aJson["country"].get<std::string>()};
        }
        else if constexpr (std::is_same_v<Data, DepositInterestTotalQuestion>)
        {
            return fromWords(aJson,
                             {{"above_limit", DepositInterestTotalAnswer{true}},
                              {"not_above_limit", DepositInterestTotalAnswer{false}}});
        }
        else if constexpr (std::is_same_v<Data, BondTypeQuestion>)
        {
            return fromWords(
                aJson,
                {{"ordinary", BondTypeAnswer{BondType::Ordinary}},
                 {"discount_or_zero_coupon", BondTypeAnswer{BondType::DiscountOrZeroCoupon}}});
        }
        else if constexpr (std::is_same_v<Data, TreatyClaimQuestion>)
        {
            if (aJson == "no_claim")
            {
                return TreatyClaimAnswer{std::nullopt};
            }

            if (!hasExactKeys(aJson, {"statement"}) || !aJson["statement"].is_string())
            {
                return std::nullopt;
            }

            return TreatyClaimAnswer{aJson["statement"].get<std::string>()};
        }
        else
        {
            static_assert(std::is_same_v<Data, DeveloperExportQuestion>);

            return fromWords(aJson, {{"confirm", DeveloperExportConfirmAnswer{}}});
        }
    };

    return std::visit(fromData, aRequest.mData);
}

/// The body as a JSON object, or an error when it is not an object, has a `path` key or is too
/// deep. The caller checks the keys.
std::variant<Json, CallBodyError> readBodyObject(std::string_view aBody) {
    Json json = Json::parse(aBody, nullptr, false);

    if (!json.is_object())
    {
        return CallBodyError::BadRequest;
    }

    const detail::JsonScan scan = detail::scanRequestJson(json, 1);

    if (scan == detail::JsonScan::HasPathKey)
    {
        return CallBodyError::PathNotAllowed;
    }

    if (scan == detail::JsonScan::TooDeep)
    {
        return CallBodyError::BadRequest;
    }

    return json;
}

/// The run revision of a call body: a whole number of 0 or more.
std::optional<std::uint64_t> revisionOf(const Json& aBody) {
    const auto revision = aBody.find("runRevision");

    if (revision == aBody.end() || !revision->is_number_unsigned())
    {
        return std::nullopt;
    }

    return revision->get<std::uint64_t>();
}

} // namespace

std::string_view callBodyErrorCode(CallBodyError aError) {
    if (aError == CallBodyError::PathNotAllowed)
    {
        return "path_not_allowed";
    }

    return "bad_request";
}

std::variant<AnswerValue, AnswerShapeError> parseAnswerJson(std::string_view aAnswerJson,
                                                            const DecisionRequest& aRequest) {
    const Json json = Json::parse(aAnswerJson, nullptr, false);
    Parsed answer;

    if (!json.is_discarded())
    {
        answer = answerFor(json, aRequest);
    }

    if (!answer)
    {
        return AnswerShapeError{};
    }

    return std::move(*answer);
}

std::variant<AnswerCall, CallBodyError> parseAnswerCallJson(std::string_view aBody,
                                                            const ApplicationResult& aResult) {
    auto body = readBodyObject(aBody);

    if (const auto* error = std::get_if<CallBodyError>(&body))
    {
        return *error;
    }

    const Json& json = std::get<Json>(body);
    const auto revision = revisionOf(json);
    const auto answers = json.find("answers");

    if (!hasExactKeys(json, {"runRevision", "answers"}) || !revision || answers == json.end() ||
        !answers->is_array())
    {
        return CallBodyError::BadRequest;
    }

    AnswerCall call;

    for (const Json& entry : *answers)
    {
        if (!hasExactKeys(entry, {"requestId", "answer"}) || !entry["requestId"].is_string())
        {
            return CallBodyError::BadRequest;
        }

        const std::string& requestId = entry["requestId"].get_ref<const std::string&>();
        const std::size_t index = call.mAnswers.size();

        // A request that is not open in the current revision gets a placeholder: the session
        // rejects the call for the revision or the ID before it looks at the value.
        const auto request = *revision == aResult.mRunRevision
                                 ? std::find_if(aResult.mRequests.begin(),
                                                aResult.mRequests.end(),
                                                [&requestId](const DecisionRequest& aRequest) {
                                                    return aRequest.mId == requestId;
                                                })
                                 : aResult.mRequests.end();

        DecisionAnswer answer{.mRequestId = requestId,
                              .mRunRevision = *revision,
                              .mValue = CancelAnswer{}};

        if (request != aResult.mRequests.end())
        {
            Parsed value = answerFor(entry["answer"], *request);

            if (!value)
            {
                auto message = makeDiagnosticMessage(MessageKey::AnswerRejectedAnswerDoesNotFit,
                                                     {textParam("requestId", requestId)});

                call.mShapeRejections.push_back(
                    RejectedAnswer{.mIndex = index,
                                   .mReason = AnswerRejection::AnswerDoesNotFit,
                                   .mMessage = std::move(message.mEnglish),
                                   .mMessageKey = message.mKey,
                                   .mMessageParams = std::move(message.mParams)});
            }
            else
            {
                answer.mValue = std::move(*value);
            }
        }

        call.mAnswers.push_back(std::move(answer));
    }

    return call;
}

std::variant<DeleteCall, CallBodyError> parseDeleteCallJson(std::string_view aBody) {
    auto body = readBodyObject(aBody);

    if (const auto* error = std::get_if<CallBodyError>(&body))
    {
        return *error;
    }

    const Json& json = std::get<Json>(body);
    const auto revision = revisionOf(json);

    if (!revision || json.size() != 2)
    {
        return CallBodyError::BadRequest;
    }

    DeleteCall call{.mRunRevision = *revision};

    if (const auto all = json.find("all"); all != json.end())
    {
        if (!all->is_boolean() || !all->get<bool>())
        {
            return CallBodyError::BadRequest;
        }

        call.mAll = true;

        return call;
    }

    const auto ids = json.find("decisionIds");

    if (ids == json.end() || !ids->is_array())
    {
        return CallBodyError::BadRequest;
    }

    const bool allText =
        std::all_of(ids->begin(), ids->end(), [](const Json& aId) { return aId.is_string(); });

    if (!allText)
    {
        return CallBodyError::BadRequest;
    }

    call.mDecisionIds.reserve(ids->size());

    std::transform(ids->begin(),
                   ids->end(),
                   std::back_inserter(call.mDecisionIds),
                   [](const Json& aId) { return aId.get<std::string>(); });

    return call;
}

namespace {

/// The `source` object of an input fault: the plain file name and the row, when there are any.
nlohmann::ordered_json faultSourceJson(std::string_view aFile, std::optional<std::size_t> aRow) {
    nlohmann::ordered_json source = nlohmann::ordered_json::object();
    const auto filename = SourceFilename::tryFromPath(aFile);

    if (filename)
    {
        source["file"] = filename->value();
    }

    if (aRow)
    {
        source["row"] = *aRow;
    }

    return source;
}

} // namespace

std::string serializeRejectionsJson(const std::vector<RejectedAnswer>& aRejections) {
    using OrderedJson = nlohmann::ordered_json;

    OrderedJson rejections = OrderedJson::array();

    std::transform(
        aRejections.begin(),
        aRejections.end(),
        std::back_inserter(rejections),
        [](const RejectedAnswer& aRejection) {
            return OrderedJson{
                {"index", aRejection.mIndex},
                {"messageKey", messageDefinition(aRejection.mMessageKey).mId},
                {"messageParams", detail::messageParamsToJson(aRejection.mMessageParams)},
            };
        });

    return OrderedJson{{"rejections", std::move(rejections)}}
        .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string inputFaultLogReason(const ApplicationInputFault& aFault) {
    const auto* rateFault = std::get_if<RateFileFault>(&aFault);

    if (rateFault != nullptr)
    {
        const auto& key = rateFault->mDiagnostic.mMessageKey;

        return "rate_file " + (key.has_value() ? std::string{messageDefinition(*key).mId} : "none");
    }

    return "reference_file " +
           std::string{messageDefinition(std::get<ReferenceFileFault>(aFault).mMessage.mKey).mId};
}

std::string serializeInputFaultJson(const ApplicationInputFault& aFault) {
    using OrderedJson = nlohmann::ordered_json;

    OrderedJson fault = OrderedJson::object();

    const auto fill = [&fault](const auto& aSpecific) {
        using Fault = std::decay_t<decltype(aSpecific)>;

        if constexpr (std::is_same_v<Fault, RateFileFault>)
        {
            const ParseDiagnostic& diagnostic = aSpecific.mDiagnostic;

            fault["kind"] = "ecb_rate_file";

            if (diagnostic.mMessageKey)
            {
                fault["messageKey"] = messageDefinition(*diagnostic.mMessageKey).mId;
                fault["messageParams"] = detail::messageParamsToJson(diagnostic.mMessageParams);
            }

            fault["source"] = faultSourceJson(diagnostic.mSourceFile, diagnostic.mRowIndex);
        }
        else
        {
            static_assert(std::is_same_v<Fault, ReferenceFileFault>);

            fault["kind"] = "reference_file";
            fault["messageKey"] = messageDefinition(aSpecific.mMessage.mKey).mId;
            fault["messageParams"] = detail::messageParamsToJson(aSpecific.mMessage.mParams);
            fault["source"] = faultSourceJson(aSpecific.mFile, aSpecific.mRow);
        }
    };

    std::visit(fill, aFault);

    return OrderedJson{{"inputFault", std::move(fault)}}
        .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

} // namespace taxbroker::api
