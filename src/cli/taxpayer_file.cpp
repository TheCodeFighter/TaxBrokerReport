#include "cli/taxpayer_file.hpp"

#include "utils/date_utils.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <optional>
#include <utility>

namespace taxbroker::cli {
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

constexpr std::size_t kTaxNumberLength = 8;

TaxpayerFileError keyError(std::string_view aKey, std::string_view aProblem) {
    return TaxpayerFileError{"\"" + std::string{aKey} + "\" " + std::string{aProblem}};
}

bool isValidTaxNumber(const std::string& aValue) {
    const bool hasEightDigits = aValue.size() == kTaxNumberLength &&
                                std::all_of(aValue.begin(), aValue.end(), [](char aCharacter) {
                                    return aCharacter >= '0' && aCharacter <= '9';
                                });

    return hasEightDigits && aValue != "00000000";
}

/// Reads an optional non-empty string. Returns an error when the key is there with another type
/// or an empty string.
std::variant<std::optional<std::string>, TaxpayerFileError> optionalText(const Json& aObject,
                                                                         std::string_view aKey) {
    const auto value = aObject.find(aKey);

    if (value == aObject.end())
    {
        return std::optional<std::string>{};
    }

    if (!value->is_string() || value->get<std::string>().empty())
    {
        return keyError(aKey, "must be a text that is not empty.");
    }

    return std::optional<std::string>{value->get<std::string>()};
}

} // namespace

std::variant<TaxpayerMetadata, TaxpayerFileError> parseTaxpayerJson(std::string_view aText) {
    const auto json = Json::parse(aText, nullptr, false);

    if (json.is_discarded())
    {
        return TaxpayerFileError{"The taxpayer file is not valid JSON."};
    }

    if (!json.is_object())
    {
        return TaxpayerFileError{"The taxpayer file must contain one JSON object."};
    }

    for (const auto& [key, value] : json.items())
    {
        if (key == "filingType")
        {
            return TaxpayerFileError{"\"filingType\" is not read from the taxpayer file: the "
                                     "report is an original return, or a self-report with "
                                     "--self-report."};
        }

        if (std::find(kKnownKeys.begin(), kKnownKeys.end(), key) == kKnownKeys.end())
        {
            return keyError(key, "is not a known key of the taxpayer file.");
        }
    }

    const auto taxNumber = json.find("taxNumber");

    if (taxNumber == json.end())
    {
        return keyError("taxNumber", "is missing.");
    }

    if (!taxNumber->is_string() || !isValidTaxNumber(taxNumber->get<std::string>()))
    {
        return keyError("taxNumber", "must be a text of eight digits, not 00000000.");
    }

    const auto isResident = json.find("isResident");

    if (isResident == json.end())
    {
        return keyError("isResident", "is missing.");
    }

    if (!isResident->is_boolean())
    {
        return keyError("isResident", "must be true or false.");
    }

    if (!isResident->get<bool>())
    {
        return keyError("isResident",
                        "is false, but only Slovenian tax residents are supported for now.");
    }

    TaxpayerMetadata taxpayer{.mTaxNumber = taxNumber->get<std::string>(), .mIsResident = true};

    const std::array<std::pair<std::string_view, std::optional<std::string>*>, 7> textFields{{
        {"name", &taxpayer.mName},
        {"address", &taxpayer.mAddress},
        {"city", &taxpayer.mCity},
        {"postCode", &taxpayer.mPostCode},
        {"postName", &taxpayer.mPostName},
        {"email", &taxpayer.mEmail},
        {"phoneNumber", &taxpayer.mPhoneNumber},
    }};

    for (const auto& [key, field] : textFields)
    {
        auto text = optionalText(json, key);

        if (auto* error = std::get_if<TaxpayerFileError>(&text))
        {
            return std::move(*error);
        }

        *field = std::get<std::optional<std::string>>(std::move(text));
    }

    auto birthDateText = optionalText(json, "birthDate");

    if (auto* error = std::get_if<TaxpayerFileError>(&birthDateText))
    {
        return std::move(*error);
    }

    if (const auto& text = std::get<std::optional<std::string>>(birthDateText))
    {
        const auto birthDate = parseCalendarDate(*text);

        if (!birthDate)
        {
            return keyError("birthDate", "must be a date in the form YYYY-MM-DD.");
        }

        taxpayer.mBirthDate = birthDate;
    }

    return taxpayer;
}

std::variant<TaxpayerMetadata, TaxpayerFileError>
readTaxpayerFile(const std::filesystem::path& aPath) {
    std::ifstream input{aPath, std::ios::binary};

    if (!input)
    {
        return TaxpayerFileError{"The taxpayer file cannot be read."};
    }

    const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};

    return parseTaxpayerJson(text);
}

} // namespace taxbroker::cli
