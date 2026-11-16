#include "taxbroker/api/taxpayer_json.hpp"

#include "taxbroker/text_format.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace taxbroker::api {
namespace {

using Json = nlohmann::json;

constexpr std::array<std::string_view, 10> kKnownKeys{
    "taxNumber",
    "isResident",
    "name",
    "address",
    "city",
    "postCode",
    "postName",
    "birthDate",
    "email",
    "phoneNumber",
};

/// The optional keys in the order their problems are reported.
constexpr std::array<std::string_view, 8> kOptionalKeys{
    "name",
    "address",
    "city",
    "postCode",
    "postName",
    "email",
    "phoneNumber",
    "birthDate",
};

constexpr std::size_t kTaxNumberLength = 8;
constexpr std::size_t kPostCodeLength = 4;

TaxpayerJsonError keyError(std::string_view aKey, std::string_view aProblem) {
    return TaxpayerJsonError{"\"" + std::string{aKey} + "\" " + std::string{aProblem},
                             std::string{aKey}};
}

bool isValidTaxNumber(const std::string& aValue) {
    const bool hasEightDigits = aValue.size() == kTaxNumberLength &&
                                std::all_of(aValue.begin(), aValue.end(), [](char aCharacter) {
                                    return aCharacter >= '0' && aCharacter <= '9';
                                });

    return hasEightDigits && aValue != "00000000";
}

/// A Slovenian post number: four digits, from 1000 to 9999.
bool isValidPostCode(const std::string& aValue) {
    return aValue.size() == kPostCodeLength && aValue.front() >= '1' && aValue.front() <= '9' &&
           std::all_of(aValue.begin(), aValue.end(), [](char aCharacter) {
               return aCharacter >= '0' && aCharacter <= '9';
           });
}

bool isKnownKey(std::string_view aKey) {
    return std::find(kKnownKeys.begin(), kKnownKeys.end(), aKey) != kKnownKeys.end();
}

/// The problem with the value of a known key, as the end of an error sentence, or nothing when the
/// value is fine. The taxpayer file and the saved profile both use it, so a loaded field is
/// checked like a typed one.
std::optional<std::string_view> valueProblem(std::string_view aKey, const Json& aValue) {
    if (aKey == "taxNumber")
    {
        if (aValue.is_string() && isValidTaxNumber(aValue.get<std::string>()))
        {
            return std::nullopt;
        }

        return "must be a text of eight digits, not 00000000.";
    }

    if (aKey == "isResident")
    {
        if (!aValue.is_boolean())
        {
            return "must be true or false.";
        }

        if (!aValue.get<bool>())
        {
            return "is false, but only Slovenian tax residents are supported for now.";
        }

        return std::nullopt;
    }

    if (!aValue.is_string() || aValue.get<std::string>().empty())
    {
        return "must be a text that is not empty.";
    }

    if (aKey == "postCode" && !isValidPostCode(aValue.get<std::string>()))
    {
        return "must be a text of four digits, from 1000 to 9999.";
    }

    if (aKey == "birthDate" && !parseCalendarDate(aValue.get<std::string>()))
    {
        return "must be a date in the form YYYY-MM-DD.";
    }

    return std::nullopt;
}

/// The text of an optional key, or nothing when the key is not there. The value was checked.
std::optional<std::string> optionalText(const Json& aObject, std::string_view aKey) {
    const auto value = aObject.find(aKey);

    if (value == aObject.end())
    {
        return std::nullopt;
    }

    return value->get<std::string>();
}

} // namespace

std::variant<TaxpayerMetadata, TaxpayerJsonError> parseTaxpayerJson(std::string_view aText) {
    const auto json = Json::parse(aText, nullptr, false);

    if (json.is_discarded())
    {
        return TaxpayerJsonError{"The taxpayer file is not valid JSON."};
    }

    if (!json.is_object())
    {
        return TaxpayerJsonError{"The taxpayer file must contain one JSON object."};
    }

    for (const auto& [key, value] : json.items())
    {
        if (key == "filingType")
        {
            return TaxpayerJsonError{"\"filingType\" is not read from the taxpayer file: the "
                                     "report is an original return, or a self-report with "
                                     "--self-report.",
                                     "filingType"};
        }

        if (!isKnownKey(key))
        {
            auto error = keyError(key, "is not a known key of the taxpayer file.");

            error.mKey.clear();

            return error;
        }
    }

    // The two required keys come first; the first problem found is the one reported.
    for (const std::string_view key : {"taxNumber", "isResident"})
    {
        const auto value = json.find(key);

        if (value == json.end())
        {
            return keyError(key, "is missing.");
        }

        if (const auto problem = valueProblem(key, *value))
        {
            return keyError(key, *problem);
        }
    }

    for (const std::string_view key : kOptionalKeys)
    {
        const auto value = json.find(key);

        if (value == json.end())
        {
            continue;
        }

        if (const auto problem = valueProblem(key, *value))
        {
            return keyError(key, *problem);
        }
    }

    TaxpayerMetadata taxpayer{.mTaxNumber = json.at("taxNumber").get<std::string>(),
                              .mIsResident = true};

    taxpayer.mName = optionalText(json, "name");
    taxpayer.mAddress = optionalText(json, "address");
    taxpayer.mCity = optionalText(json, "city");
    taxpayer.mPostCode = optionalText(json, "postCode");
    taxpayer.mPostName = optionalText(json, "postName");
    taxpayer.mEmail = optionalText(json, "email");
    taxpayer.mPhoneNumber = optionalText(json, "phoneNumber");

    if (const auto birthDate = optionalText(json, "birthDate"))
    {
        taxpayer.mBirthDate = parseCalendarDate(*birthDate);
    }

    return taxpayer;
}

TaxpayerProfile readTaxpayerProfile(std::string_view aText) {
    TaxpayerProfile profile;

    if (aText.size() > kMaxProfileBytes)
    {
        profile.mUnreadable = true;

        return profile;
    }

    const auto json = Json::parse(aText, nullptr, false);

    if (json.is_discarded() || !json.is_object())
    {
        profile.mUnreadable = true;

        return profile;
    }

    for (const auto& [key, value] : json.items())
    {
        if (!isKnownKey(key))
        {
            profile.mIgnoredKeys = true;
        }
        else if (valueProblem(key, value))
        {
            profile.mInvalidKeys.push_back(key);
        }
        else if (key == "isResident")
        {
            profile.mIsResident = true;
        }
        else
        {
            profile.mFields.emplace(key, value.get<std::string>());
        }
    }

    return profile;
}

} // namespace taxbroker::api
