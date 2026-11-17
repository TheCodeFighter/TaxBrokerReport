#include "run_routes.hpp"

#include "multipart.hpp"

#include "taxbroker/api/answer_json.hpp"
#include "taxbroker/api/application_result_json.hpp"
#include "taxbroker/api/report_files.hpp"
#include "taxbroker/api/request_json.hpp"
#include "taxbroker/api/support_report_json.hpp"
#include "taxbroker/exception_kind.hpp"
#include "taxbroker/diagnostic_messages.hpp"
#include "taxbroker/logging.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace taxbroker {

namespace {

/// The report in the URL: the key of the result JSON.
struct ReportName {
    std::string_view mKey; ///< The key in the URL and in the result JSON: `capitalGains`, ...
    ReportType mType{};    ///< The report of that key; always set.
};

constexpr std::array<ReportName, 4> kReportNames{{
    {"capitalGains", ReportType::CapitalGains},
    {"dividends", ReportType::Dividends},
    {"bankInterest", ReportType::BankDepositInterest},
    {"otherInterest", ReportType::OtherInterest},
}};

/// The value of `revision=<digits>` in the query, or nothing when there is none or it is not a
/// whole number.
std::optional<std::uint64_t> revisionFromQuery(std::string_view aQuery) {
    constexpr std::string_view kName = "revision=";

    if (!aQuery.starts_with(kName))
    {
        return std::nullopt;
    }

    const std::string_view digits = aQuery.substr(kName.size());
    std::uint64_t revision = 0;
    const auto [end, error] =
        std::from_chars(digits.data(), digits.data() + digits.size(), revision);

    if (digits.empty() || error != std::errc{} || end != digits.data() + digits.size())
    {
        return std::nullopt;
    }

    return revision;
}

/// A refused request: one warning with the status and the fixed code, then the response. The line
/// never holds a file name, a path, a query or a value (R13); the limit is a fixed number of the
/// server. It is the line a user sends with a bug report.
HttpResponse refuse(unsigned aStatus,
                    std::string_view aCode,
                    std::optional<std::uint64_t> aLimit = std::nullopt) {
    if (aLimit.has_value())
    {
        LOG_WARNING("Run request refused: {} {} (limit {})", aStatus, aCode, *aLimit);
    }
    else
    {
        LOG_WARNING("Run request refused: {} {}", aStatus, aCode);
    }

    return makeErrorCodeResponse(aStatus, aCode);
}

/// A refused request with a fixed reason word after the code, for example
/// `Run request refused: 400 bad_multipart (part_header)`. The reason is never a request value.
HttpResponse refuseWithReason(unsigned aStatus, std::string_view aCode, std::string_view aReason) {
    LOG_WARNING("Run request refused: {} {} ({})", aStatus, aCode, aReason);

    return makeErrorCodeResponse(aStatus, aCode);
}

/// A refused taxpayer object: the fixed taxpayer key is the reason word in the log and the
/// `field` of the body, so the form can mark its input. The key is one of the application's own
/// words (never a value or a sender's name); an unknown key has no field.
HttpResponse refuseTaxpayer(std::string_view aField) {
    if (aField.empty())
    {
        return refuse(400, "bad_taxpayer");
    }

    LOG_WARNING("Run request refused: 400 bad_taxpayer ({})", aField);

    return makeJsonResponse(400,
                            R"({"error":"bad_taxpayer","field":")" + std::string(aField) + R"("})");
}

HttpResponse inputFaultResponse(const ApplicationInputFault& aFault) {
    LOG_WARNING("Run request refused: 422 input_fault ({})", api::inputFaultLogReason(aFault));

    return makeJsonResponse(422, api::serializeInputFaultJson(aFault));
}

HttpResponse refuseMethod(std::string_view aAllow) {
    LOG_WARNING("Run request refused: 405 method_not_allowed");

    return makeNotAllowedResponse(aAllow);
}

HttpResponse resultResponse(const ApplicationResult& aResult) {
    return makeJsonResponse(200, api::serializeApplicationResultJson(aResult));
}

HttpResponse noRunResponse() {
    return refuse(404, "no_run");
}

/// The question types of the answers with their counts. An ID that is not in the result is left
/// out.
std::string answeredTypesLogText(const ApplicationResult& aResult,
                                 const std::vector<DecisionAnswer>& aAnswers) {
    std::map<std::string_view, std::size_t> types;

    for (const DecisionAnswer& answer : aAnswers)
    {
        const auto request = std::find_if(aResult.mRequests.begin(),
                                          aResult.mRequests.end(),
                                          [&answer](const DecisionRequest& aRequest) {
                                              return aRequest.mId == answer.mRequestId;
                                          });

        if (request != aResult.mRequests.end())
        {
            ++types[api::requestTypeName(request->mType)];
        }
    }

    std::string text = "[";

    for (const auto& [name, count] : types)
    {
        text += text.size() > 1 ? ", " : "";
        text += name;
        text += " x" + std::to_string(count);
    }

    return text + "]";
}

/// The log line holds the number of rejections and each stable message key once with its count,
/// sorted by key (a call can hold thousands of answers); never an answer value or a decision ID.
HttpResponse rejectedResponse(const std::vector<RejectedAnswer>& aRejections) {
    std::map<std::string_view, std::size_t> counts;

    for (const RejectedAnswer& rejection : aRejections)
    {
        ++counts[messageDefinition(rejection.mMessageKey).mId];
    }

    std::string keys;

    for (const auto& [key, count] : counts)
    {
        keys += keys.empty() ? "" : ", ";
        keys += key;
        keys += " x" + std::to_string(count);
    }

    LOG_WARNING("Run request refused: 422 answer_rejected ({}: {})", aRejections.size(), keys);

    return makeJsonResponse(422, api::serializeRejectionsJson(aRejections));
}

} // namespace

void logRouteFailure(std::string_view aRoute) {
    LOG_ERROR("Run request failed: {} ({})", aRoute, currentExceptionKind());
}

RunRoutes::RunRoutes(std::size_t aMaxFileBytes) : mMaxFileBytes(aMaxFileBytes) {}

std::uint64_t RunRoutes::uploadBodyLimit() const {
    return static_cast<std::uint64_t>(api::kMaxUploadFiles + 1) * mMaxFileBytes;
}

HttpResponse
RunRoutes::handle(const HttpRequest& aRequest, std::string_view aPath, std::string_view aQuery) {
    // The route word is set before the work starts, so a failure names the step that was running.
    std::string_view route = "unknown";

    try
    { return dispatch(aRequest, aPath, aQuery, route); } catch (...)
    {
        // The HTTP layer answers 500 and writes its own line; this one names the route.
        logRouteFailure(route);

        throw;
    }
}

HttpResponse RunRoutes::dispatch(const HttpRequest& aRequest,
                                 std::string_view aPath,
                                 std::string_view aQuery,
                                 std::string_view& aRoute) {
    constexpr std::string_view kReportsPrefix = "/api/run/reports/";
    constexpr std::string_view kXmlSuffix = "/xml";

    if (aPath == "/api/run")
    {
        if (aRequest.mMethod == "GET")
        {
            aRoute = "result";

            return result();
        }

        if (aRequest.mMethod == "POST")
        {
            aRoute = "upload";

            return upload(aRequest);
        }

        return refuseMethod("GET, POST");
    }

    if (aPath == "/api/run/answers" || aPath == "/api/run/decisions/delete")
    {
        if (aRequest.mMethod != "POST")
        {
            return refuseMethod("POST");
        }

        if (aPath == "/api/run/answers")
        {
            aRoute = "answers";

            return answers(aRequest);
        }

        aRoute = "delete_answers";

        return deleteAnswers(aRequest);
    }

    if (aPath == "/api/run/support-report")
    {
        if (aRequest.mMethod != "GET")
        {
            return refuseMethod("GET");
        }

        aRoute = "support_report";

        return supportReport();
    }

    // `/api/run/reports/xml` would share its slash between the prefix and the suffix.
    if (aPath.starts_with(kReportsPrefix) && aPath.ends_with(kXmlSuffix) &&
        aPath.size() > kReportsPrefix.size() + kXmlSuffix.size())
    {
        if (aRequest.mMethod != "GET")
        {
            return refuseMethod("GET");
        }

        const std::string_view report =
            aPath.substr(kReportsPrefix.size(),
                         aPath.size() - kReportsPrefix.size() - kXmlSuffix.size());

        aRoute = "download";

        return download(report, aQuery);
    }

    LOG_WARNING("Run request refused: 404 not_found");

    return makeErrorResponse(404);
}

HttpResponse RunRoutes::upload(const HttpRequest& aRequest) {
    const std::vector<std::string_view> contentTypes = headerValues(aRequest, "Content-Type");

    if (contentTypes.size() != 1)
    {
        return refuseWithReason(400, "bad_multipart", "content_type");
    }

    auto multipart = readMultipart(aRequest.mBody, contentTypes.front(), mMaxFileBytes);

    if (const auto* error = std::get_if<MultipartError>(&multipart))
    {
        if (*error == MultipartError::TooLarge)
        {
            return refuse(413, "file_too_large", mMaxFileBytes);
        }

        if (*error == MultipartError::TooManyParts)
        {
            return refuse(400, "too_many_files", api::kMaxUploadFiles);
        }

        return refuseWithReason(400, "bad_multipart", multipartErrorReason(*error));
    }

    auto& readParts = std::get<std::vector<MultipartPart>>(multipart);

    // The request JSON is small; a longer one is refused before it is parsed.
    const bool requestTooLarge =
        std::any_of(readParts.begin(), readParts.end(), [](const MultipartPart& aPart) {
            return aPart.mName == "request" && aPart.mContent.size() > kDefaultBodyLimit;
        });

    if (requestTooLarge)
    {
        return refuse(413, "request_too_large", kDefaultBodyLimit);
    }

    // A file part keeps the file limit also when it is sent as a plain field, without a file name.
    const bool fileTooLarge =
        std::any_of(readParts.begin(), readParts.end(), [this](const MultipartPart& aPart) {
            return aPart.mName != "request" && aPart.mContent.size() > mMaxFileBytes;
        });

    if (fileTooLarge)
    {
        return refuse(413, "file_too_large", mMaxFileBytes);
    }

    std::vector<api::UploadedPart> parts;

    parts.reserve(readParts.size());

    std::transform(readParts.begin(),
                   readParts.end(),
                   std::back_inserter(parts),
                   [](MultipartPart& aPart) {
                       return api::UploadedPart{.mName = std::move(aPart.mName),
                                                .mFileName = std::move(aPart.mFileName),
                                                .mContent = aPart.mContent};
                   });

    auto request = api::buildApplicationRequest(parts);

    if (const auto* refusal = std::get_if<api::RunRequestRefusal>(&request))
    {
        const api::RunRequestError error = refusal->mError;

        if (error == api::RunRequestError::BadTaxpayer)
        {
            return refuseTaxpayer(refusal->mField);
        }

        return refuse(400,
                      api::runRequestErrorCode(error),
                      error == api::RunRequestError::TooManyFiles
                          ? std::optional<std::uint64_t>{api::kMaxUploadFiles}
                          : std::nullopt);
    }

    // A second upload keeps the answers and starts a new revision; a failed restart leaves the
    // run as it was.
    if (mRun.has_value())
    {
        const auto fault = mRun->restart(std::move(std::get<ApplicationRequest>(request)));

        if (fault.has_value())
        {
            return inputFaultResponse(*fault);
        }

        api::logResult(mRun->result());

        return resultResponse(mRun->result());
    }

    auto started = ApplicationRun::start(std::move(std::get<ApplicationRequest>(request)));

    if (const auto* fault = std::get_if<ApplicationInputFault>(&started))
    {
        return inputFaultResponse(*fault);
    }

    mRun.emplace(std::move(std::get<ApplicationRun>(started)));

    api::logResult(mRun->result());

    return resultResponse(mRun->result());
}

HttpResponse RunRoutes::result() const {
    if (!mRun.has_value())
    {
        return noRunResponse();
    }

    LOG_DEBUG("Run result requested: revision {}", mRun->result().mRunRevision);

    return resultResponse(mRun->result());
}

HttpResponse RunRoutes::supportReport() const {
    if (!mRun.has_value())
    {
        return noRunResponse();
    }

    const ApplicationResult& current = mRun->result();
    const std::string body = api::serializeSupportReport(api::currentSupportReportApp(),
                                                         api::supportReportRun(mRun->request()),
                                                         current);

    LOG_INFO("Support report downloaded: revision {}, {} bytes", current.mRunRevision, body.size());

    HttpResponse response = makeResponse(200, body);

    response.mHeaders.push_back({"Content-Type", "application/json; charset=utf-8"});
    response.mHeaders.push_back({"Content-Disposition",
                                 "attachment; filename=\"taxbroker-support-" +
                                     std::to_string(mRun->request().mReportingYear) + ".json\""});

    return response;
}

HttpResponse RunRoutes::answers(const HttpRequest& aRequest) {
    if (!mRun.has_value())
    {
        return noRunResponse();
    }

    auto call = api::parseAnswerCallJson(aRequest.mBody, mRun->result());

    if (const auto* error = std::get_if<api::CallBodyError>(&call))
    {
        return refuse(400, api::callBodyErrorCode(*error));
    }

    auto& answerCall = std::get<api::AnswerCall>(call);

    // A shape that does not fit never reaches the session.
    if (!answerCall.mShapeRejections.empty())
    {
        return rejectedResponse(answerCall.mShapeRejections);
    }

    // The question types are read before the answers change the result: the log says which
    // questions were answered, never the answers.
    const std::string answeredTypes = answeredTypesLogText(mRun->result(), answerCall.mAnswers);
    const std::size_t answerCount = answerCall.mAnswers.size();
    const std::vector<RejectedAnswer> rejections =
        mRun->session().answer(std::move(answerCall.mAnswers));

    if (!rejections.empty())
    {
        return rejectedResponse(rejections);
    }

    LOG_INFO("Answers accepted: {} {}", answerCount, answeredTypes);
    api::logResult(mRun->result());

    return resultResponse(mRun->result());
}

HttpResponse RunRoutes::deleteAnswers(const HttpRequest& aRequest) {
    if (!mRun.has_value())
    {
        return noRunResponse();
    }

    const auto call = api::parseDeleteCallJson(aRequest.mBody);

    if (const auto* error = std::get_if<api::CallBodyError>(&call))
    {
        return refuse(400, api::callBodyErrorCode(*error));
    }

    const auto& deleteCall = std::get<api::DeleteCall>(call);
    const std::vector<RejectedAnswer> rejections =
        deleteCall.mAll
            ? mRun->session().deleteAllAnswers(deleteCall.mRunRevision)
            : mRun->session().deleteAnswers(deleteCall.mDecisionIds, deleteCall.mRunRevision);

    if (!rejections.empty())
    {
        return rejectedResponse(rejections);
    }

    if (deleteCall.mAll)
    {
        LOG_INFO("Answers deleted: all");
    }
    else
    {
        LOG_INFO("Answers deleted: {}", deleteCall.mDecisionIds.size());
    }

    api::logResult(mRun->result());

    return resultResponse(mRun->result());
}

HttpResponse RunRoutes::download(std::string_view aReport, std::string_view aQuery) const {
    if (!mRun.has_value())
    {
        return noRunResponse();
    }

    const auto name =
        std::find_if(kReportNames.begin(), kReportNames.end(), [aReport](const ReportName& aName) {
            return aName.mKey == aReport;
        });

    if (name == kReportNames.end())
    {
        return refuse(404, "unknown_report");
    }

    const auto revision = revisionFromQuery(aQuery);

    if (!revision.has_value())
    {
        return refuse(400, "bad_request");
    }

    const ApplicationResult& current = mRun->result();

    if (*revision != current.mRunRevision)
    {
        return refuse(409, "stale_revision");
    }

    const ReportResult& report = current.report(name->mType);

    if (!report.mXml.has_value())
    {
        return refuse(404, "no_xml");
    }

    LOG_INFO("Report downloaded: {}, revision {}, {} bytes{}",
             name->mKey,
             current.mRunRevision,
             report.mXml->size(),
             report.mUnsafe ? ", unsafe developer export" : "");

    HttpResponse response = makeResponse(200, *report.mXml);

    response.mHeaders.push_back({"Content-Type", "application/xml; charset=utf-8"});
    response.mHeaders.push_back(
        {"Content-Disposition",
         "attachment; filename=\"" +
             api::reportXmlFileName(name->mType, mRun->request().mReportingYear, report.mUnsafe) +
             "\""});

    return response;
}

} // namespace taxbroker
