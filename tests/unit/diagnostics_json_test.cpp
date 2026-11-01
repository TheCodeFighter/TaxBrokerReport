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

    EXPECT_EQ(response.at("schemaVersion"), 1);
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
            .mSourceFile = "Synthetic Export.csv",
            .mRowIndex = 7U,
            .mTransactionId = "synthetic-transaction-001",
            .mField = "asset_class",
            .mMessage = "The asset was preserved, but tax support is pending.",
        },
        ParseDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = DiagnosticCode::ParseError,
            .mSourceFile = "Synthetic Export.csv",
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
    EXPECT_EQ(error.at("source").at("file"), "Synthetic Export.csv");
    EXPECT_FALSE(error.at("source").contains("row"));
    EXPECT_FALSE(error.contains("transactionId"));
    EXPECT_FALSE(error.contains("field"));
    // A diagnostic without a message key sends neither a key nor parameters.
    EXPECT_FALSE(error.contains("messageKey"));
    EXPECT_FALSE(error.contains("messageParams"));
}

TEST(DiagnosticsJsonTest, SerializesTheMessageKeyAndParameters) {
    ParseResult parseResult{
        .mBroker = Broker::TradeRepublic,
    };
    parseResult.mDiagnostics = {
        ParseDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = DiagnosticCode::FieldCountMismatch,
            .mSourceFile = "synthetic.csv",
            .mRowIndex = 3U,
            .mMessage = "The row has a different number of fields than the header (expected 23, "
                        "found 22); the row was skipped.",
            .mMessageKey = MessageKey::FieldCountMismatch,
            .mMessageParams = {numberParam("expected", 23U), numberParam("found", 22U)},
        },
        ParseDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = DiagnosticCode::ParseError,
            .mSourceFile = "synthetic.csv",
            .mMessage = "The CSV file has an unclosed quote; it may be cut off or edited. No "
                        "rows were imported.",
            .mMessageKey = MessageKey::ParseErrorUnclosedQuote,
        },
        ParseDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = DiagnosticCode::MissingField,
            .mSourceFile = "synthetic.csv",
            .mRowIndex = 4U,
            .mField = "price",
            .mMessage = "Required field 'price' is missing in trade row; the row was skipped.",
            .mMessageKey = MessageKey::MissingFieldRequiredField,
            .mMessageParams = {textParam("field", "price"),
                               termParam("rowKind", term::kRowKindTrade)},
        },
    };

    const auto response = nlohmann::json::parse(api::serializeDiagnosticsJson(parseResult));
    const auto& diagnostics = response.at("diagnostics");

    ASSERT_EQ(diagnostics.size(), 3U);

    EXPECT_EQ(diagnostics.at(0).at("messageKey"), "field_count_mismatch");
    EXPECT_EQ(diagnostics.at(0).at("messageParams"),
              (nlohmann::json{{"expected", 23}, {"found", 22}}));
    EXPECT_EQ(diagnostics.at(0).at("message"),
              "The row has a different number of fields than the header (expected 23, found "
              "22); the row was skipped.");

    EXPECT_EQ(diagnostics.at(1).at("messageKey"), "parse_error.unclosed_quote");
    EXPECT_EQ(diagnostics.at(1).at("messageParams"), nlohmann::json::object());

    // Numbers stay numbers; text and term ids are strings.
    EXPECT_EQ(diagnostics.at(2).at("messageKey"), "missing_field.required_field");
    EXPECT_EQ(diagnostics.at(2).at("messageParams"),
              (nlohmann::json{{"field", "price"}, {"rowKind", "row_kind.trade"}}));
    EXPECT_TRUE(diagnostics.at(0).at("messageParams").at("expected").is_number_unsigned());
    EXPECT_TRUE(diagnostics.at(2).at("messageParams").at("rowKind").is_string());
}

TEST(DiagnosticsJsonTest, KeepsOnlyFilenamesFromLinuxMacAndWindowsPaths) {
    ParseResult parseResult;
    parseResult.mDiagnostics = {
        ParseDiagnostic{.mSourceFile = "/home/user/imports/linux.csv"},
        ParseDiagnostic{.mSourceFile = "/Users/user/imports/macos.csv"},
        ParseDiagnostic{.mSourceFile = R"(C:\Users\user\imports\windows.csv)"},
    };

    const auto response = nlohmann::json::parse(api::serializeDiagnosticsJson(parseResult));
    const auto& diagnostics = response.at("diagnostics");

    EXPECT_EQ(diagnostics.at(0).at("source").at("file"), "linux.csv");
    EXPECT_EQ(diagnostics.at(1).at("source").at("file"), "macos.csv");
    EXPECT_EQ(diagnostics.at(2).at("source").at("file"), "windows.csv");
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
        DiagnosticCode mCode{};
        std::string_view mJsonValue;
    };
    constexpr std::array expectedCodes{
        ExpectedCode{DiagnosticCode::UnknownRowType, "unknown_row_type"},
        ExpectedCode{DiagnosticCode::UnsupportedRowType, "unsupported_row_type"},
        ExpectedCode{DiagnosticCode::UnsupportedAssetClass, "unsupported_asset_class"},
        ExpectedCode{DiagnosticCode::MissingField, "missing_field"},
        ExpectedCode{DiagnosticCode::InvalidValue, "invalid_value"},
        ExpectedCode{DiagnosticCode::InconsistentValue, "inconsistent_value"},
        ExpectedCode{DiagnosticCode::TradeAmountMismatch, "trade_amount_mismatch"},
        ExpectedCode{DiagnosticCode::FieldCountMismatch, "field_count_mismatch"},
        ExpectedCode{DiagnosticCode::LocalizedNumberFormat, "localized_number_format"},
        ExpectedCode{DiagnosticCode::InvalidHeader, "invalid_header"},
        ExpectedCode{DiagnosticCode::ParseError, "parse_error"},
        ExpectedCode{DiagnosticCode::DuplicateRate, "duplicate_rate"},
        ExpectedCode{DiagnosticCode::MixedRateYears, "mixed_rate_years"},
        ExpectedCode{DiagnosticCode::DuplicateRateYear, "duplicate_rate_year"},
        ExpectedCode{DiagnosticCode::NoRates, "no_rates"},
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
