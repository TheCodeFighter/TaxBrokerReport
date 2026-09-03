#include "taxbroker/api/diagnostics_json.hpp"
#include "taxbroker/types.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <string_view>

namespace {

using namespace taxbroker;

std::string_view toString(Broker aBroker) {
    switch (aBroker)
    {
    case Broker::TradeRepublic:
        return "trade_republic";
    case Broker::InteractiveBrokers:
        return "interactive_brokers";
    case Broker::Unknown:
        return "unknown";
    }

    return "unknown";
}

std::string_view toString(DiagnosticSeverity aSeverity) {
    switch (aSeverity)
    {
    case DiagnosticSeverity::Warning:
        return "warning";
    case DiagnosticSeverity::Error:
        return "error";
    }

    return "error";
}

std::string_view toString(DiagnosticCode aCode) {
    switch (aCode)
    {
    case DiagnosticCode::UnknownRowType:
        return "unknown_row_type";
    case DiagnosticCode::UnsupportedRowType:
        return "unsupported_row_type";
    case DiagnosticCode::UnsupportedAssetClass:
        return "unsupported_asset_class";
    case DiagnosticCode::MissingField:
        return "missing_field";
    case DiagnosticCode::InvalidValue:
        return "invalid_value";
    case DiagnosticCode::InconsistentValue:
        return "inconsistent_value";
    case DiagnosticCode::ParseError:
        return "parse_error";
    }

    return "parse_error";
}

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
        const auto sourceFile = std::filesystem::path{diagnostic.mSourceFile}.filename().string();
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
