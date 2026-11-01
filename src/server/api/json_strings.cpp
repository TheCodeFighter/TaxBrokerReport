#include "json_strings.hpp"

#include <variant>

namespace taxbroker::api::detail {

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
    case DiagnosticCode::TradeAmountMismatch:
        return "trade_amount_mismatch";
    case DiagnosticCode::FieldCountMismatch:
        return "field_count_mismatch";
    case DiagnosticCode::LocalizedNumberFormat:
        return "localized_number_format";
    case DiagnosticCode::InvalidHeader:
        return "invalid_header";
    case DiagnosticCode::ParseError:
        return "parse_error";
    case DiagnosticCode::DuplicateRate:
        return "duplicate_rate";
    case DiagnosticCode::MixedRateYears:
        return "mixed_rate_years";
    case DiagnosticCode::DuplicateRateYear:
        return "duplicate_rate_year";
    case DiagnosticCode::NoRates:
        return "no_rates";
    }

    return "parse_error";
}

nlohmann::json messageParamsToJson(const std::vector<MessageParam>& aParams) {
    nlohmann::json params = nlohmann::json::object();

    for (const auto& param : aParams)
    {
        const auto toJson = [](const auto& aValue) { return nlohmann::json(aValue); };

        params[param.mName] = std::visit(toJson, param.mValue);
    }

    return params;
}

} // namespace taxbroker::api::detail
