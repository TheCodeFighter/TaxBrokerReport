#include "taxbroker/api/messages_json.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>

namespace {

using namespace taxbroker;

TEST(MessagesJsonTest, MessageWithoutParametersHasBothLanguages) {
    const nlohmann::json body = nlohmann::json::parse(api::messagesJson());

    const nlohmann::json& message = body.at("messages").at("parse_error.unclosed_quote");

    // resources/i18n/diagnostic_messages.csv, line 2.
    EXPECT_EQ(message.at("en"),
              "The CSV file has an unclosed quote; it may be cut off or edited. No rows were "
              "imported.");
    EXPECT_EQ(message.at("sl"),
              "Datoteka CSV ima nezaprt narekovaj; morda je odrezana ali urejena. Nobena vrstica "
              "ni bila uvožena.");
    EXPECT_TRUE(message.at("params").empty());
}

TEST(MessagesJsonTest, MessageParametersHaveNameAndKind) {
    const nlohmann::json body = nlohmann::json::parse(api::messagesJson());

    const nlohmann::json& params =
        body.at("messages").at("unsupported_asset_class.tax_unsupported").at("params");

    // resources/i18n/diagnostic_messages.csv, line 23: "{assetClass}".
    ASSERT_EQ(params.size(), 1U);
    EXPECT_EQ(params[0], nlohmann::json::parse(R"({"name":"assetClass","kind":"text"})"));
}

TEST(MessagesJsonTest, NumberParametersHaveTheNumberKind) {
    const nlohmann::json body = nlohmann::json::parse(api::messagesJson());

    const nlohmann::json& params = body.at("messages").at("field_count_mismatch").at("params");

    // resources/i18n/diagnostic_messages.csv, line 7: "(expected {expected}, found {found})".
    ASSERT_EQ(params.size(), 2U);
    EXPECT_EQ(params[0], nlohmann::json::parse(R"({"name":"expected","kind":"number"})"));
    EXPECT_EQ(params[1], nlohmann::json::parse(R"({"name":"found","kind":"number"})"));
}

TEST(MessagesJsonTest, TermParameterHasTheTermKind) {
    const nlohmann::json body = nlohmann::json::parse(api::messagesJson());

    const nlohmann::json& params =
        body.at("messages").at("missing_field.required_field").at("params");

    // The text has "{field}" and "{rowKind}"; the row kind is a term the frontend translates.
    ASSERT_EQ(params.size(), 2U);
    EXPECT_EQ(params[0], nlohmann::json::parse(R"({"name":"field","kind":"text"})"));
    EXPECT_EQ(params[1], nlohmann::json::parse(R"({"name":"rowKind","kind":"term"})"));
}

TEST(MessagesJsonTest, ElevenTermsHaveBothLanguages) {
    const nlohmann::json body = nlohmann::json::parse(api::messagesJson());

    const nlohmann::json& terms = body.at("terms");

    // kTermKeys has eleven ids.
    EXPECT_EQ(terms.size(), 11U);
    EXPECT_FALSE(terms.at("row_kind.trade").at("en").get<std::string>().empty());
    EXPECT_FALSE(terms.at("row_kind.trade").at("sl").get<std::string>().empty());

    for (const auto& term : terms)
    {
        EXPECT_FALSE(term.at("en").get<std::string>().empty());
        EXPECT_FALSE(term.at("sl").get<std::string>().empty());
    }
}

TEST(MessagesJsonTest, TheTextIsBuiltOnce) {
    EXPECT_EQ(&api::messagesJson(), &api::messagesJson());
}

} // namespace
