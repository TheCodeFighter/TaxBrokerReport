#include "taxbroker/api/diagnostics_json.hpp"
#include "json_strings.hpp"
#include "taxbroker/diagnostic_messages.hpp"
#include "taxbroker/types.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string_view>
#include <variant>

namespace {

using namespace taxbroker;
using api::detail::messageParamsToJson;
using api::detail::toString;

std::string_view statusFor(std::size_t aWarningCount, std::size_t aErrorCount) {
    if (aErrorCount != 0)
    {
        return "completed_with_errors";
    }
    if (aWarningCount != 0)
    {
        return "completed_with_warnings";
    }

    return "success";
}

} // namespace

namespace taxbroker::api {

std::string serializeDiagnosticsJson(const ParseResult& aParseResult, int aIndent) {
    const auto warningCount = static_cast<std::size_t>(
        std::count_if(aParseResult.mDiagnostics.begin(),
                      aParseResult.mDiagnostics.end(),
                      [](const ParseDiagnostic& aDiagnostic) {
                          return aDiagnostic.mSeverity == DiagnosticSeverity::Warning;
                      }));
    const auto errorCount = aParseResult.mDiagnostics.size() - warningCount;

    nlohmann::json diagnostics = nlohmann::json::array();
    for (const auto& diagnostic : aParseResult.mDiagnostics)
    {
        const auto sourceFile = SourceFilename::fromPath(diagnostic.mSourceFile).value();
        nlohmann::json source{{"file", sourceFile}};
        if (diagnostic.mRowIndex)
        {
            source["row"] = *diagnostic.mRowIndex;
        }

        nlohmann::json serialized{
            {"severity", toString(diagnostic.mSeverity)},
            {"code", toString(diagnostic.mCode)},
            {"message", diagnostic.mMessage},
            {"source", std::move(source)},
        };

        if (diagnostic.mTransactionId)
        {
            serialized["transactionId"] = *diagnostic.mTransactionId;
        }
        if (diagnostic.mField)
        {
            serialized["field"] = *diagnostic.mField;
        }
        if (diagnostic.mMessageKey)
        {
            serialized["messageKey"] = messageDefinition(*diagnostic.mMessageKey).mId;
            serialized["messageParams"] = messageParamsToJson(diagnostic.mMessageParams);
        }

        diagnostics.emplace_back(std::move(serialized));
    }

    const nlohmann::json response{
        {"schemaVersion", kDiagnosticsSchemaVersion},
        {"broker", toString(aParseResult.mBroker)},
        {"status", statusFor(warningCount, errorCount)},
        {"summary",
         {
             {"warningCount", warningCount},
             {"errorCount", errorCount},
             {"totalCount", aParseResult.mDiagnostics.size()},
         }},
        {"diagnostics", std::move(diagnostics)},
    };

    return response.dump(aIndent, ' ', false, nlohmann::json::error_handler_t::replace);
}

} // namespace taxbroker::api
