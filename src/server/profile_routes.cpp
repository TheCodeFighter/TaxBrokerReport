#include "profile_routes.hpp"

#include "exception_kind.hpp"

#include "taxbroker/api/taxpayer_json.hpp"
#include "taxbroker/logging.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace taxbroker {

namespace {

using Json = nlohmann::json;

/// The longest body of a save. The file is made from the checked fields only, so it is never
/// longer than the body, and it stays far below the longest profile that is read.
constexpr std::size_t kMaxSaveBytes = 32 * 1024;

HttpResponse refuse(unsigned aStatus, std::string_view aCode) {
    LOG_WARNING("Profile request refused: {} {}", aStatus, aCode);

    return makeErrorCodeResponse(aStatus, aCode);
}

HttpResponse refuseMethod(std::string_view aAllow) {
    LOG_WARNING("Profile request refused: 405 method_not_allowed");

    return makeNotAllowedResponse(aAllow);
}

/// A refused taxpayer object: the fixed taxpayer key is the reason word in the log and the
/// `field` of the body. The key is one of the application's own words, never a value or a
/// sender's name; an unknown key has no field.
HttpResponse refuseTaxpayer(const std::string& aField) {
    if (aField.empty())
    {
        return refuse(400, "bad_taxpayer");
    }

    LOG_WARNING("Profile request refused: 400 bad_taxpayer ({})", aField);

    return makeJsonResponse(400, R"({"error":"bad_taxpayer","field":")" + aField + R"("})");
}

} // namespace

ProfileRoutes::ProfileRoutes(ProfileStore aStore) : mStore(std::move(aStore)) {}

HttpResponse ProfileRoutes::handle(const HttpRequest& aRequest, std::string_view aPath) const {
    if (aPath == "/api/profile")
    {
        if (aRequest.mMethod == "GET")
        {
            return load();
        }

        if (aRequest.mMethod == "POST")
        {
            return save(aRequest);
        }

        return refuseMethod("GET, POST");
    }

    if (aPath == "/api/profile/delete")
    {
        if (aRequest.mMethod != "POST")
        {
            return refuseMethod("POST");
        }

        return remove();
    }

    return refuse(404, "not_found");
}

HttpResponse ProfileRoutes::notSavedResponse() const {
    Json body{{"saved", false}};

    if (mStore.location().has_value())
    {
        body["location"] = *mStore.location();
    }

    return makeJsonResponse(200, body.dump());
}

HttpResponse ProfileRoutes::unreadableResponse() const {
    LOG_WARNING("Profile unreadable");

    Json body{{"saved", true},
              {"unreadable", true},
              {"ignoredKeys", false},
              {"invalidFields", Json::array()}};

    if (mStore.location().has_value())
    {
        body["location"] = *mStore.location();
    }

    return makeJsonResponse(200, body.dump());
}

HttpResponse ProfileRoutes::load() const {
    std::optional<std::string> content;

    try
    { content = mStore.read(api::kMaxProfileBytes + 1); } catch (const std::exception&)
    { return unreadableResponse(); }

    if (!content.has_value())
    {
        return notSavedResponse();
    }

    const api::TaxpayerProfile profile = api::readTaxpayerProfile(*content);

    if (profile.mUnreadable)
    {
        return unreadableResponse();
    }

    Json taxpayer = Json::object();

    for (const auto& [key, value] : profile.mFields)
    {
        taxpayer[key] = value;
    }

    if (profile.mIsResident)
    {
        taxpayer["isResident"] = true;
    }

    Json body{{"saved", true},
              {"unreadable", false},
              {"ignoredKeys", profile.mIgnoredKeys},
              {"invalidFields", profile.mInvalidKeys},
              {"taxpayer", taxpayer}};

    if (mStore.location().has_value())
    {
        body["location"] = *mStore.location();
    }

    LOG_INFO("Profile loaded (fields {}, invalid {})",
             taxpayer.size(),
             profile.mInvalidKeys.size());

    return makeJsonResponse(200, body.dump());
}

HttpResponse ProfileRoutes::save(const HttpRequest& aRequest) const {
    if (aRequest.mBody.size() > kMaxSaveBytes)
    {
        LOG_WARNING("Profile request refused: 413 request_too_large (limit {})", kMaxSaveBytes);

        return makeErrorCodeResponse(413, "request_too_large");
    }

    const auto parsed = api::parseTaxpayerJson(aRequest.mBody);

    if (const auto* error = std::get_if<api::TaxpayerJsonError>(&parsed))
    {
        return refuseTaxpayer(error->mKey);
    }

    // The body holds only known, checked keys, so the file is the body in the fixed key order.
    const std::string content = Json::parse(aRequest.mBody).dump() + "\n";

    try
    { mStore.write(content); } catch (const std::exception&)
    {
        LOG_ERROR("Profile not saved ({})", currentExceptionKind());

        return makeErrorCodeResponse(500, "profile_not_saved");
    }

    LOG_INFO("Profile saved");

    return makeResponse(204);
}

HttpResponse ProfileRoutes::remove() const {
    try
    { mStore.remove(); } catch (const std::exception&)
    {
        LOG_ERROR("Profile not deleted ({})", currentExceptionKind());

        return makeErrorCodeResponse(500, "profile_not_deleted");
    }

    LOG_INFO("Profile deleted");

    return notSavedResponse();
}

} // namespace taxbroker
