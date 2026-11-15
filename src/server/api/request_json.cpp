#include "taxbroker/api/request_json.hpp"

#include "taxbroker/api/taxpayer_json.hpp"

#include "json_scan.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <set>
#include <utility>

namespace taxbroker::api {

namespace {

using Json = nlohmann::json;

/// The broker names of the request. A new broker adds one row; no route branches on a broker.
constexpr std::array<std::pair<std::string_view, Broker>, 1> kBrokers{{
    {"trade_republic", Broker::TradeRepublic},
}};

/// The kinds of a file entry.
enum class FileKind {
    Broker,
    EcbRates,
    Reference,
};

constexpr std::array<std::pair<std::string_view, FileKind>, 3> kFileKinds{{
    {"broker", FileKind::Broker},
    {"ecb_rates", FileKind::EcbRates},
    {"reference", FileKind::Reference},
}};

constexpr std::array<std::pair<std::string_view, FilingType>, 2> kFilingTypes{{
    {"original", FilingType::Original},
    {"self_report", FilingType::SelfReport},
}};

/// True when the object has no key outside aAllowed.
template <std::size_t N>
bool hasOnlyKeys(const Json& aObject, const std::array<std::string_view, N>& aAllowed) {
    for (const auto& [key, value] : aObject.items())
    {
        if (std::find(aAllowed.begin(), aAllowed.end(), key) == aAllowed.end())
        {
            return false;
        }
    }

    return true;
}

template <typename Value, std::size_t N>
std::optional<Value> lookup(const std::array<std::pair<std::string_view, Value>, N>& aTable,
                            const std::string& aName) {
    const auto found = std::find_if(aTable.begin(), aTable.end(), [&aName](const auto& aRow) {
        return aRow.first == aName;
    });

    if (found == aTable.end())
    {
        return std::nullopt;
    }

    return found->second;
}

/// The display name of an uploaded file: the last part of the name the browser sent, or an empty
/// name when there is none (the run reports it as a diagnostic).
std::string displayName(const UploadedPart& aPart) {
    if (!aPart.mFileName.has_value())
    {
        return {};
    }

    const auto filename = SourceFilename::tryFromPath(*aPart.mFileName);

    return filename.has_value() ? filename->value() : std::string{};
}

} // namespace

std::string_view runRequestErrorCode(RunRequestError aError) {
    switch (aError)
    {
    case RunRequestError::BadRequest:
        return "bad_request";
    case RunRequestError::PathNotAllowed:
        return "path_not_allowed";
    case RunRequestError::NoBrokerFile:
        return "no_broker_file";
    case RunRequestError::MissingPart:
        return "missing_part";
    case RunRequestError::UnreferencedPart:
        return "unreferenced_part";
    case RunRequestError::DuplicatePart:
        return "duplicate_part";
    case RunRequestError::TooManyFiles:
        return "too_many_files";
    case RunRequestError::UnsupportedBroker:
        break;
    }

    return "unsupported_broker";
}

std::variant<ApplicationRequest, RunRequestError>
buildApplicationRequest(std::span<const UploadedPart> aParts) {
    // The part names must be unique, and exactly one is the request.
    std::set<std::string_view> names;
    const UploadedPart* requestPart = nullptr;

    for (const UploadedPart& part : aParts)
    {
        if (!names.insert(part.mName).second)
        {
            return RunRequestError::DuplicatePart;
        }

        if (part.mName == "request")
        {
            requestPart = &part;
        }
    }

    if (requestPart == nullptr)
    {
        return RunRequestError::BadRequest;
    }

    const Json json = Json::parse(requestPart->mContent, nullptr, false);

    if (!json.is_object())
    {
        return RunRequestError::BadRequest;
    }

    const detail::JsonScan scan = detail::scanRequestJson(json, 1);

    if (scan == detail::JsonScan::HasPathKey)
    {
        return RunRequestError::PathNotAllowed;
    }

    if (scan == detail::JsonScan::TooDeep)
    {
        return RunRequestError::BadRequest;
    }

    constexpr std::array<std::string_view, 5> topKeys{"reportingYear",
                                                      "filingType",
                                                      "developerMode",
                                                      "taxpayer",
                                                      "files"};

    if (!hasOnlyKeys(json, topKeys))
    {
        return RunRequestError::BadRequest;
    }

    ApplicationRequest request;

    const auto year = json.find("reportingYear");

    if (year == json.end() || !year->is_number_integer() ||
        year->get<std::int64_t>() < kMinimumReportingYear ||
        year->get<std::int64_t>() > kMaximumReportingYear)
    {
        return RunRequestError::BadRequest;
    }

    request.mReportingYear = year->get<int>();

    const auto filingType = json.find("filingType");
    const auto filing = filingType != json.end() && filingType->is_string()
                            ? lookup(kFilingTypes, filingType->get<std::string>())
                            : std::nullopt;

    if (!filing.has_value())
    {
        return RunRequestError::BadRequest;
    }

    request.mFilingType = *filing;

    const auto developerMode = json.find("developerMode");

    if (developerMode != json.end())
    {
        if (!developerMode->is_boolean())
        {
            return RunRequestError::BadRequest;
        }

        request.mDeveloperMode = developerMode->get<bool>();
    }

    const auto taxpayer = json.find("taxpayer");

    if (taxpayer == json.end() || !taxpayer->is_object())
    {
        return RunRequestError::BadRequest;
    }

    auto parsedTaxpayer = parseTaxpayerJson(taxpayer->dump());

    if (!std::holds_alternative<TaxpayerMetadata>(parsedTaxpayer))
    {
        return RunRequestError::BadRequest;
    }

    request.mTaxpayer = std::move(std::get<TaxpayerMetadata>(parsedTaxpayer));

    const auto files = json.find("files");

    if (files == json.end() || !files->is_array())
    {
        return RunRequestError::BadRequest;
    }

    if (files->size() > kMaxUploadFiles)
    {
        return RunRequestError::TooManyFiles;
    }

    constexpr std::array<std::string_view, 3> fileKeys{"part", "kind", "broker"};
    std::set<std::string> named;

    for (const Json& entry : *files)
    {
        if (!entry.is_object() || !hasOnlyKeys(entry, fileKeys))
        {
            return RunRequestError::BadRequest;
        }

        const auto partName = entry.find("part");
        const auto kindName = entry.find("kind");

        if (partName == entry.end() || !partName->is_string() || kindName == entry.end() ||
            !kindName->is_string())
        {
            return RunRequestError::BadRequest;
        }

        const auto kind = lookup(kFileKinds, kindName->get<std::string>());
        const auto brokerField = entry.find("broker");

        if (!kind.has_value() || (*kind == FileKind::Broker) != (brokerField != entry.end()))
        {
            return RunRequestError::BadRequest;
        }

        const std::string& partText = partName->get_ref<const std::string&>();

        if (!named.insert(partText).second)
        {
            return RunRequestError::DuplicatePart;
        }

        const auto part =
            std::find_if(aParts.begin(), aParts.end(), [&partText](const auto& aPart) {
                return aPart.mName == partText;
            });

        if (part == aParts.end() || &*part == requestPart)
        {
            return RunRequestError::MissingPart;
        }

        InputFile file{.mName = displayName(*part), .mContent = std::string{part->mContent}};

        if (*kind == FileKind::EcbRates)
        {
            request.mEcbRateFiles.push_back(std::move(file));
        }
        else if (*kind == FileKind::Reference)
        {
            request.mReferenceFiles.push_back(std::move(file));
        }
        else
        {
            if (!brokerField->is_string())
            {
                return RunRequestError::BadRequest;
            }

            const auto broker = lookup(kBrokers, brokerField->get<std::string>());

            if (!broker.has_value())
            {
                return RunRequestError::UnsupportedBroker;
            }

            request.mBrokerFiles.push_back(
                BrokerInputFile{.mBroker = *broker, .mFile = std::move(file)});
        }
    }

    // Every part except the request must be named.
    if (named.size() + 1 != aParts.size())
    {
        return RunRequestError::UnreferencedPart;
    }

    if (request.mBrokerFiles.empty())
    {
        return RunRequestError::NoBrokerFile;
    }

    return request;
}

} // namespace taxbroker::api
