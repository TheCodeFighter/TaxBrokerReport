#include "taxbroker/api/diagnostics_json.hpp"
#include "taxbroker/diagnostics.hpp"
#include "taxbroker/types.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <string>
#include <string_view>

namespace {

using namespace taxbroker;

TEST(DiagnosticsJsonTest, SerializesAnEmptySuccessfulResult) {
    const ParseResult parseResult{
        .mBroker = Broker::TradeRepublic,
    };

    const auto response = nlohmann::json::parse(api::serializeDiagnosticsJson(parseResult));

    EXPECT_EQ(response.at("schemaVersion"), api::kDiagnosticsSchemaVersion);
    EXPECT_EQ(response.at("broker"), "trade_republic");
    EXPECT_EQ(response.at("status"), "success");
    EXPECT_EQ(response.at("summary").at("warningCount"), 0U);
    EXPECT_EQ(response.at("summary").at("errorCount"), 0U);
    EXPECT_EQ(response.at("summary").at("totalCount"), 0U);
    EXPECT_TRUE(response.at("diagnostics").empty());
}

TEST(DiagnosticsJsonTest, SerializesStableFrontendFieldsAndSummary) {
    ParseResult parseResult{
        .mBroker = Broker::TradeRepublic,
    };
    parseResult.mDiagnostics = {
        ParseDiagnostic{
            .mSeverity = DiagnosticSeverity::Warning,
            .mCode = DiagnosticCode::UnsupportedAssetClass,
            .mSourceFile = "/private/upload/Synthetic Export.csv",
            .mRowIndex = 7U,
            .mTransactionId = "synthetic-transaction-001",
            .mField = "asset_class",
            .mMessage = "The asset was preserved, but tax support is pending.",
        },
        ParseDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = DiagnosticCode::ParseError,
            .mSourceFile = "/private/upload/Synthetic Export.csv",
            .mMessage = "The CSV could not be parsed.",
        },
    };

    const auto response = nlohmann::json::parse(api::serializeDiagnosticsJson(parseResult, 2));

    EXPECT_EQ(response.at("status"), "completed_with_errors");
    EXPECT_EQ(response.at("summary").at("warningCount"), 1U);
    EXPECT_EQ(response.at("summary").at("errorCount"), 1U);
    EXPECT_EQ(response.at("summary").at("totalCount"), 2U);

    const auto& warning = response.at("diagnostics").at(0);
    EXPECT_EQ(warning.at("severity"), "warning");
    EXPECT_EQ(warning.at("code"), "unsupported_asset_class");
    EXPECT_EQ(warning.at("source").at("file"), "Synthetic Export.csv");
    EXPECT_EQ(warning.at("source").at("row"), 7U);
    EXPECT_EQ(warning.at("transactionId"), "synthetic-transaction-001");
    EXPECT_EQ(warning.at("field"), "asset_class");

    const auto& error = response.at("diagnostics").at(1);
    EXPECT_EQ(error.at("severity"), "error");
    EXPECT_EQ(error.at("code"), "parse_error");
    EXPECT_FALSE(error.at("source").contains("row"));
    EXPECT_FALSE(error.contains("transactionId"));
    EXPECT_FALSE(error.contains("field"));

    // Host paths are private implementation details and must never reach the browser contract.
    EXPECT_EQ(api::serializeDiagnosticsJson(parseResult).find("/private/upload"),
              std::string::npos);
}

TEST(DiagnosticsJsonTest, ReportsWarningOnlyResultsSeparatelyFromErrors) {
    ParseResult parseResult{
        .mBroker = Broker::InteractiveBrokers,
    };
    parseResult.mDiagnostics.emplace_back(ParseDiagnostic{
        .mSeverity = DiagnosticSeverity::Warning,
        .mCode = DiagnosticCode::UnsupportedAssetClass,
        .mSourceFile = "synthetic.csv",
        .mMessage = "Synthetic warning",
    });

    const auto response = nlohmann::json::parse(api::serializeDiagnosticsJson(parseResult));

    EXPECT_EQ(response.at("broker"), "interactive_brokers");
    EXPECT_EQ(response.at("status"), "completed_with_warnings");
}

TEST(DiagnosticsJsonTest, KeepsEveryDiagnosticCodeStable) {
    struct ExpectedCode {
        DiagnosticCode mCode;
        std::string_view mJsonValue;
    };
    constexpr std::array expectedCodes{
        ExpectedCode{DiagnosticCode::UnknownRowType, "unknown_row_type"},
        ExpectedCode{DiagnosticCode::UnsupportedRowType, "unsupported_row_type"},
        ExpectedCode{DiagnosticCode::UnsupportedAssetClass, "unsupported_asset_class"},
        ExpectedCode{DiagnosticCode::MissingField, "missing_field"},
        ExpectedCode{DiagnosticCode::InvalidValue, "invalid_value"},
        ExpectedCode{DiagnosticCode::InconsistentValue, "inconsistent_value"},
        ExpectedCode{DiagnosticCode::ParseError, "parse_error"},
    };

    for (const auto& expected : expectedCodes)
    {
        SCOPED_TRACE(expected.mJsonValue);
        ParseResult parseResult;
        parseResult.mDiagnostics.emplace_back(ParseDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = expected.mCode,
            .mSourceFile = "synthetic.csv",
            .mMessage = "Synthetic diagnostic",
        });

        const auto response = nlohmann::json::parse(api::serializeDiagnosticsJson(parseResult));
        EXPECT_EQ(response.at("diagnostics").at(0).at("code"), expected.mJsonValue);
    }
}

TEST(DiagnosticsJsonTest, FallsBackToSafeValuesForUnexpectedEnumValues) {
    ParseResult parseResult{
        .mBroker = static_cast<Broker>(999),
    };
    parseResult.mDiagnostics.emplace_back(ParseDiagnostic{
        .mSeverity = static_cast<DiagnosticSeverity>(999),
        .mCode = static_cast<DiagnosticCode>(999),
        .mSourceFile = "synthetic.csv",
        .mMessage = "Synthetic diagnostic",
    });

    const auto response = nlohmann::json::parse(api::serializeDiagnosticsJson(parseResult));

    EXPECT_EQ(response.at("broker"), "unknown");
    EXPECT_EQ(response.at("status"), "completed_with_errors");
    EXPECT_EQ(response.at("diagnostics").at(0).at("severity"), "error");
    EXPECT_EQ(response.at("diagnostics").at(0).at("code"), "parse_error");
}

} // namespace
