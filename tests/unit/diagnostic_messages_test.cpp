#include "taxbroker/diagnostic_messages.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace taxbroker;

struct ExpectedParam {
    std::string_view mName;
    MessageParamKind mKind{};
};

struct ExpectedMessage {
    MessageKey mKey{};
    std::string_view mId;
    std::vector<ExpectedParam> mParams;
};

/// Every key with its id and parameters, written out in full so that a change is deliberate.
const std::vector<ExpectedMessage>& expectedMessages() {
    static const std::vector<ExpectedMessage> messages{
        {MessageKey::ParseErrorUnclosedQuote, "parse_error.unclosed_quote", {}},
        {MessageKey::ParseErrorUnreadableFile, "parse_error.unreadable_file", {}},
        {MessageKey::ParseErrorLineBreakInField, "parse_error.line_break_in_field", {}},
        {MessageKey::InvalidHeaderOtherSeparator, "invalid_header.other_separator", {}},
        {MessageKey::InvalidHeaderMissingColumns,
         "invalid_header.missing_columns",
         {{"count", MessageParamKind::Number}, {"columns", MessageParamKind::Text}}},
        {MessageKey::FieldCountMismatch,
         "field_count_mismatch",
         {{"expected", MessageParamKind::Number}, {"found", MessageParamKind::Number}}},
        {MessageKey::LocalizedNumberFormat,
         "localized_number_format",
         {{"rowCount", MessageParamKind::Number}}},
        {MessageKey::MissingFieldDatetime, "missing_field.datetime", {}},
        {MessageKey::MissingFieldRequiredField,
         "missing_field.required_field",
         {{"field", MessageParamKind::Text}, {"rowKind", MessageParamKind::Term}}},
        {MessageKey::MissingFieldSymbolAndName,
         "missing_field.symbol_and_name",
         {{"rowKind", MessageParamKind::Term}}},
        {MessageKey::MissingFieldName, "missing_field.name", {}},
        {MessageKey::MissingFieldAssetClass, "missing_field.asset_class", {}},
        {MessageKey::InvalidValueDatetime, "invalid_value.datetime", {}},
        {MessageKey::InvalidValueField,
         "invalid_value.field",
         {{"field", MessageParamKind::Text}, {"rowKind", MessageParamKind::Term}}},
        {MessageKey::InconsistentValueDatetimeDate,
         "inconsistent_value.datetime_date",
         {{"defaultHour", MessageParamKind::Number}}},
        {MessageKey::InconsistentValueTradeSide,
         "inconsistent_value.trade_side",
         {{"field", MessageParamKind::Text}}},
        {MessageKey::InconsistentValueAssetClass, "inconsistent_value.asset_class", {}},
        {MessageKey::UnsupportedAssetClassUnrecognized, "unsupported_asset_class.unrecognized", {}},
        {MessageKey::UnsupportedAssetClassTaxUnsupported,
         "unsupported_asset_class.tax_unsupported",
         {{"assetClass", MessageParamKind::Text}}},
        {MessageKey::UnsupportedAssetClassInterestMapping,
         "unsupported_asset_class.interest_mapping",
         {}},
        {MessageKey::UnsupportedRowTypeRecognized,
         "unsupported_row_type.recognized",
         {{"type", MessageParamKind::Text}, {"category", MessageParamKind::Text}}},
        {MessageKey::UnsupportedRowTypeInterestType, "unsupported_row_type.interest_type", {}},
        {MessageKey::UnknownRowType,
         "unknown_row_type",
         {{"type", MessageParamKind::Text}, {"category", MessageParamKind::Text}}},
        {MessageKey::DuplicateSourceIndex, "duplicate_source_index", {}},
        {MessageKey::InconsistentSourceIndexNoSource, "inconsistent_source_index.no_source", {}},
        {MessageKey::InconsistentSourceIndexMismatch, "inconsistent_source_index.mismatch", {}},
        {MessageKey::ConflictingDuplicate, "conflicting_duplicate", {}},
        {MessageKey::DuplicateTransactionInFile, "duplicate_transaction_in_file", {}},
        {MessageKey::InstrumentNameConflict, "instrument_name_conflict", {}},
        {MessageKey::InstrumentAssetClassConflict, "instrument_asset_class_conflict", {}},
        {MessageKey::UnrepresentableValue, "unrepresentable_value", {}},
        {MessageKey::ArithmeticOverflow, "arithmetic_overflow", {}},
        {MessageKey::InvalidExchangeRate, "invalid_exchange_rate", {}},
        {MessageKey::CorporateActionUnrepresentable, "corporate_action_unrepresentable", {}},
        {MessageKey::InvalidReportingYear,
         "invalid_reporting_year",
         {{"minYear", MessageParamKind::Number}, {"maxYear", MessageParamKind::Number}}},
        {MessageKey::IncompleteHistory, "incomplete_history", {}},
        {MessageKey::XmlValueOutOfRange,
         "xml_value_out_of_range",
         {{"field", MessageParamKind::Text}}},
        {MessageKey::UnsupportedTaxTreatmentCapitalClass,
         "unsupported_tax_treatment.capital_class",
         {{"assetClass", MessageParamKind::Text}}},
        {MessageKey::UnsupportedTaxTreatmentPrivateMarketSkipped,
         "unsupported_tax_treatment.private_market_skipped",
         {}},
        {MessageKey::TaxClassificationUnknownCapital, "tax_classification_unknown.capital", {}},
        {MessageKey::MissingExchangeRateTrade,
         "missing_exchange_rate.trade",
         {{"currency", MessageParamKind::Text}}},
        {MessageKey::LossEligibilityUnresolvedConfirmationRequired,
         "loss_eligibility_unresolved.confirmation_required",
         {}},
        {MessageKey::LossEligibilityUnresolvedCancelled,
         "loss_eligibility_unresolved.cancelled",
         {}},
        {MessageKey::LossEligibilityUnresolvedAssumedBlocked,
         "loss_eligibility_unresolved.assumed_blocked",
         {}},
        {MessageKey::LossEligibilityUnresolvedUnitsMismatch,
         "loss_eligibility_unresolved.units_mismatch",
         {}},
        {MessageKey::CorporateActionNotAppliedLossWindow,
         "corporate_action_not_applied.loss_window",
         {}},
        {MessageKey::AnswerRejectedStaleRevision,
         "answer_rejected.stale_revision",
         {{"requestId", MessageParamKind::Text}}},
        {MessageKey::AnswerRejectedUnknownRequest,
         "answer_rejected.unknown_request",
         {{"requestId", MessageParamKind::Text}}},
        {MessageKey::AnswerRejectedDuplicateAnswer,
         "answer_rejected.duplicate_answer",
         {{"requestId", MessageParamKind::Text}}},
        {MessageKey::AnswerRejectedAnswerDoesNotFit,
         "answer_rejected.answer_does_not_fit",
         {{"requestId", MessageParamKind::Text}}},
        {MessageKey::AnswerRejectedUnknownDecision,
         "answer_rejected.unknown_decision",
         {{"decisionId", MessageParamKind::Text}}},
        {MessageKey::AnswerRejectedStaleRevisionDelete,
         "answer_rejected.stale_revision_delete",
         {{"decisionId", MessageParamKind::Text}}},
        {MessageKey::AnswerRejectedDuplicateDelete,
         "answer_rejected.duplicate_delete",
         {{"decisionId", MessageParamKind::Text}}},
        {MessageKey::CorporateActionAmbiguousRatioRequired,
         "corporate_action_ambiguous.ratio_required",
         {}},
        {MessageKey::CorporateActionAmbiguousCancelled, "corporate_action_ambiguous.cancelled", {}},
        {MessageKey::CorporateActionAmbiguousSameDay, "corporate_action_ambiguous.same_day", {}},
        {MessageKey::CorporateActionNotAppliedWaitingForEarlierAction,
         "corporate_action_not_applied.waiting_for_earlier_action",
         {{"earlierDate", MessageParamKind::Text}}},
        {MessageKey::AnswerRejectedRatioNotPositive,
         "answer_rejected.ratio_not_positive",
         {{"requestId", MessageParamKind::Text}, {"field", MessageParamKind::Text}}},
        {MessageKey::AnswerRejectedRatioUnchanged,
         "answer_rejected.ratio_unchanged",
         {{"requestId", MessageParamKind::Text}}},
        {MessageKey::AnswerRejectedRatioOutOfRange,
         "answer_rejected.ratio_out_of_range",
         {{"requestId", MessageParamKind::Text}}},
        {MessageKey::AnswerRejectedPreviewMismatch,
         "answer_rejected.preview_mismatch",
         {{"requestId", MessageParamKind::Text}}},
        {MessageKey::AnswerRejectedEffectiveDateOutOfRange,
         "answer_rejected.effective_date_out_of_range",
         {{"requestId", MessageParamKind::Text},
          {"earliestDate", MessageParamKind::Text},
          {"latestDate", MessageParamKind::Text}}},
        {MessageKey::AnswerRejectedEffectiveDateWithoutUnits,
         "answer_rejected.effective_date_without_units",
         {{"requestId", MessageParamKind::Text}}},
        {MessageKey::CorporateActionAppliedUserRatio,
         "corporate_action_applied.user_ratio",
         {{"newShares", MessageParamKind::Number},
          {"oldShares", MessageParamKind::Number},
          {"effectiveDate", MessageParamKind::Text}}},
        {MessageKey::CorporateActionQuantityRounded, "corporate_action_quantity_rounded", {}},
        {MessageKey::CorporateActionUnrepresentableRoundingBudget,
         "corporate_action_unrepresentable.rounding_budget",
         {}},
        {MessageKey::CorporateActionUnrepresentableUnitValue,
         "corporate_action_unrepresentable.unit_value",
         {}},
        {MessageKey::CorporateActionMissingPosition, "corporate_action_missing_position", {}},
        {MessageKey::CorporateActionAmbiguousDifferentDates,
         "corporate_action_ambiguous.different_dates",
         {{"days", MessageParamKind::Number}}},
        {MessageKey::UnsupportedCorporateActionMerger, "unsupported_corporate_action.merger", {}},
        {MessageKey::CorporateActionAmbiguousRatioRequiredBeforeSale,
         "corporate_action_ambiguous.ratio_required_before_sale",
         {{"saleDate", MessageParamKind::Text}, {"days", MessageParamKind::Number}}},
        {MessageKey::AnswerRejectedLaterSaleTooLarge,
         "answer_rejected.later_sale_too_large",
         {{"requestId", MessageParamKind::Text}}},
    };

    return messages;
}

const std::vector<std::string_view>& expectedTerms() {
    static const std::vector<std::string_view> terms{
        "row_kind.trade",
        "row_kind.dividend",
        "row_kind.bond_interest",
        "row_kind.broker_interest",
        "row_kind.corporate_action",
        "row_kind.benefit",
        "row_kind.private_market",
    };

    return terms;
}

/// The names between braces, for example `{expected}` gives "expected".
std::set<std::string> placeholdersOf(const std::string& aText) {
    static const std::regex pattern{R"(\{([A-Za-z]+)\})"};
    std::set<std::string> names;

    for (std::sregex_iterator match{aText.begin(), aText.end(), pattern};
         match != std::sregex_iterator{};
         ++match)
    {
        names.insert((*match)[1].str());
    }

    return names;
}

/// Parameters of the right kind for a definition, to render a message without a scenario.
std::vector<MessageParam> sampleParams(const MessageDefinition& aDefinition) {
    std::vector<MessageParam> params;

    for (const auto& definition : aDefinition.mParams)
    {
        switch (definition.mKind)
        {
        case MessageParamKind::Number:
            params.push_back(numberParam(std::string{definition.mName}, 7U));
            break;
        case MessageParamKind::Text:
            params.push_back(textParam(std::string{definition.mName}, "sample"));
            break;
        case MessageParamKind::Term:
            params.push_back(termParam(std::string{definition.mName}, term::kRowKindTrade));
            break;
        }
    }

    return params;
}

TEST(DiagnosticMessagesTest, KeepsEveryKeyIdAndParameterListStable) {
    const auto definitions = messageDefinitions();

    ASSERT_EQ(definitions.size(), 72U);
    ASSERT_EQ(expectedMessages().size(), 72U);

    for (std::size_t index = 0; index < definitions.size(); ++index)
    {
        const auto& expected = expectedMessages()[index];
        const auto& definition = definitions[index];

        SCOPED_TRACE(expected.mId);
        EXPECT_EQ(definition.mKey, expected.mKey);
        EXPECT_EQ(definition.mId, expected.mId);
        EXPECT_EQ(&messageDefinition(expected.mKey), &definition);
        ASSERT_EQ(definition.mParams.size(), expected.mParams.size());

        for (std::size_t param = 0; param < expected.mParams.size(); ++param)
        {
            EXPECT_EQ(definition.mParams[param].mName, expected.mParams[param].mName);
            EXPECT_EQ(definition.mParams[param].mKind, expected.mParams[param].mKind);
        }
    }
}

TEST(DiagnosticMessagesTest, EveryKeyHasOneUniqueIdOfTheDocumentedShape) {
    const std::regex shape{R"(^[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)?$)"};
    std::set<std::string> ids;
    std::set<MessageKey> keys;

    for (const auto& definition : messageDefinitions())
    {
        SCOPED_TRACE(definition.mId);
        EXPECT_TRUE(std::regex_match(std::string{definition.mId}, shape));
        EXPECT_TRUE(ids.insert(std::string{definition.mId}).second);
        EXPECT_TRUE(keys.insert(definition.mKey).second);
    }

    EXPECT_EQ(ids.size(), 72U);
    EXPECT_EQ(keys.size(), 72U);
}

TEST(DiagnosticMessagesTest, KeepsEveryTermStable) {
    const auto terms = termKeys();

    ASSERT_EQ(terms.size(), expectedTerms().size());
    EXPECT_TRUE(std::equal(terms.begin(), terms.end(), expectedTerms().begin()));

    EXPECT_EQ(term::kRowKindTrade, "row_kind.trade");
    EXPECT_EQ(term::kRowKindDividend, "row_kind.dividend");
    EXPECT_EQ(term::kRowKindBondInterest, "row_kind.bond_interest");
    EXPECT_EQ(term::kRowKindBrokerInterest, "row_kind.broker_interest");
    EXPECT_EQ(term::kRowKindCorporateAction, "row_kind.corporate_action");
    EXPECT_EQ(term::kRowKindBenefit, "row_kind.benefit");
    EXPECT_EQ(term::kRowKindPrivateMarket, "row_kind.private_market");
}

TEST(DiagnosticMessagesTest, TableHasEveryKeyAndTermExactlyOnce) {
    const auto& table = messageTable();

    ASSERT_EQ(table.size(), 79U); // 72 message keys and 7 terms.

    std::set<std::string> tableKeys;

    for (const auto& row : table)
    {
        EXPECT_TRUE(tableKeys.insert(row.mKey).second) << "Duplicate row: " << row.mKey;
    }

    std::set<std::string> expectedKeys;

    for (const auto& message : expectedMessages())
    {
        expectedKeys.insert(std::string{message.mId});
    }
    for (const auto term : expectedTerms())
    {
        expectedKeys.insert(std::string{term});
    }

    EXPECT_EQ(tableKeys, expectedKeys);
}

TEST(DiagnosticMessagesTest, EveryKeyAndTermHasEnglishAndSlovenianText) {
    for (const auto& row : messageTable())
    {
        SCOPED_TRACE(row.mKey);
        EXPECT_FALSE(row.mEnglish.empty());
        EXPECT_FALSE(row.mSlovenian.empty());
    }
}

TEST(DiagnosticMessagesTest, PlaceholdersMatchTheDeclaredParametersInBothLanguages) {
    for (const auto& definition : messageDefinitions())
    {
        SCOPED_TRACE(definition.mId);

        std::set<std::string> parameterNames;

        for (const auto& param : definition.mParams)
        {
            parameterNames.insert(std::string{param.mName});
        }

        for (const auto language : {MessageLanguage::English, MessageLanguage::Slovenian})
        {
            const auto& text = messageText(definition.mId, language);

            EXPECT_EQ(placeholdersOf(text), parameterNames);
        }
    }
}

TEST(DiagnosticMessagesTest, TermTextsAndMessagesHaveNoStrayBraces) {
    for (const auto& row : messageTable())
    {
        SCOPED_TRACE(row.mKey);

        // Remove the placeholders; no brace may be left.
        const std::regex placeholder{R"(\{[A-Za-z]+\})"};
        const auto english = std::regex_replace(row.mEnglish, placeholder, "");
        const auto slovenian = std::regex_replace(row.mSlovenian, placeholder, "");

        EXPECT_EQ(english.find_first_of("{}"), std::string::npos);
        EXPECT_EQ(slovenian.find_first_of("{}"), std::string::npos);
    }
}

TEST(DiagnosticMessagesTest, TermRowsHaveNoPlaceholders) {
    for (const auto term : expectedTerms())
    {
        SCOPED_TRACE(term);
        EXPECT_TRUE(placeholdersOf(messageText(term, MessageLanguage::English)).empty());
        EXPECT_TRUE(placeholdersOf(messageText(term, MessageLanguage::Slovenian)).empty());
    }
}

TEST(DiagnosticMessagesTest, EveryKeyRendersInBothLanguages) {
    for (const auto& definition : messageDefinitions())
    {
        SCOPED_TRACE(definition.mId);

        const auto params = sampleParams(definition);

        const auto english = renderMessage(definition.mKey, params, MessageLanguage::English);
        const auto slovenian = renderMessage(definition.mKey, params, MessageLanguage::Slovenian);

        EXPECT_FALSE(english.empty());
        EXPECT_FALSE(slovenian.empty());
        EXPECT_EQ(english.find_first_of("{}"), std::string::npos);
        EXPECT_EQ(slovenian.find_first_of("{}"), std::string::npos);
    }
}

TEST(DiagnosticMessagesTest, RendersNumbersAndTextInTheEnglishSentence) {
    const auto message =
        makeDiagnosticMessage(MessageKey::FieldCountMismatch,
                              {numberParam("expected", 23U), numberParam("found", 22U)});

    EXPECT_EQ(message.mKey, MessageKey::FieldCountMismatch);
    EXPECT_EQ(message.mParams,
              (std::vector<MessageParam>{numberParam("expected", 23U), numberParam("found", 22U)}));
    EXPECT_EQ(message.mEnglish,
              "The row has a different number of fields than the header (expected 23, found "
              "22); the row was skipped.");
}

TEST(DiagnosticMessagesTest, RendersATermAsItsEnglishText) {
    const auto message = makeDiagnosticMessage(
        MessageKey::MissingFieldRequiredField,
        {textParam("field", "price"), termParam("rowKind", term::kRowKindTrade)});

    EXPECT_EQ(message.mEnglish,
              "Required field 'price' is missing in trade row; the row was skipped.");
}

TEST(DiagnosticMessagesTest, RendersAMessageWithoutParameters) {
    const auto message = makeDiagnosticMessage(MessageKey::ParseErrorUnclosedQuote);

    EXPECT_TRUE(message.mParams.empty());
    EXPECT_EQ(message.mEnglish,
              "The CSV file has an unclosed quote; it may be cut off or edited. No rows were "
              "imported.");
}

TEST(DiagnosticMessagesTest, RendersTheSlovenianSentenceWithTheSameParameters) {
    const std::vector<MessageParam> params{numberParam("expected", 23U), numberParam("found", 22U)};

    const auto slovenian =
        renderMessage(MessageKey::FieldCountMismatch, params, MessageLanguage::Slovenian);

    EXPECT_EQ(slovenian,
              "Vrstica ima drugačno število polj kot glava (pričakovano 23, najdeno 22); "
              "vrstica je bila preskočena.");
}

TEST(DiagnosticMessagesTest, RendersATermInTheLanguageOfTheSentence) {
    const std::vector<MessageParam> params{textParam("field", "price"),
                                           termParam("rowKind", term::kRowKindDividend)};

    const auto english =
        renderMessage(MessageKey::InvalidValueField, params, MessageLanguage::English);
    const auto slovenian =
        renderMessage(MessageKey::InvalidValueField, params, MessageLanguage::Slovenian);

    EXPECT_EQ(english, "Field 'price' has an invalid value in dividend row; the row was skipped.");
    EXPECT_EQ(slovenian,
              "Polje 'price' ima neveljavno vrednost (vrsta vrstice: dividenda); vrstica je bila "
              "preskočena.");
}

TEST(DiagnosticMessagesTest, FillsEveryPlaceholderAndRepeatsAParameter) {
    const std::vector<MessageParam> params{numberParam("expected", 23U), numberParam("found", 22U)};

    EXPECT_EQ(fillMessageTemplate("{expected}/{found} then {expected}.",
                                  MessageKey::FieldCountMismatch,
                                  params,
                                  MessageLanguage::English),
              "23/22 then 23.");
    EXPECT_EQ(fillMessageTemplate("No placeholders.",
                                  MessageKey::FieldCountMismatch,
                                  params,
                                  MessageLanguage::English),
              "No placeholders.");
}

TEST(DiagnosticMessagesTest, FillsATemplateThatStartsAndEndsWithAPlaceholder) {
    const std::vector<MessageParam> params{numberParam("expected", 23U), numberParam("found", 22U)};

    EXPECT_EQ(fillMessageTemplate("{expected} fields, found {found}",
                                  MessageKey::FieldCountMismatch,
                                  params,
                                  MessageLanguage::English),
              "23 fields, found 22");
}

TEST(DiagnosticMessagesTest, RejectsATemplateWithAnUnclosedPlaceholder) {
    const std::vector<MessageParam> params{numberParam("expected", 23U), numberParam("found", 22U)};

    EXPECT_THROW((void)fillMessageTemplate("Expected {expected",
                                           MessageKey::FieldCountMismatch,
                                           params,
                                           MessageLanguage::English),
                 std::logic_error);
}

TEST(DiagnosticMessagesTest, RejectsATemplateWithAPlaceholderThatHasNoParameter) {
    const std::vector<MessageParam> params{numberParam("expected", 23U), numberParam("found", 22U)};

    EXPECT_THROW((void)fillMessageTemplate("Count {count}",
                                           MessageKey::FieldCountMismatch,
                                           params,
                                           MessageLanguage::English),
                 std::logic_error);
}

TEST(DiagnosticMessagesTest, RejectsAMissingParameter) {
    EXPECT_THROW(
        (void)makeDiagnosticMessage(MessageKey::FieldCountMismatch, {numberParam("expected", 23U)}),
        std::logic_error);
}

TEST(DiagnosticMessagesTest, RejectsAnExtraParameter) {
    EXPECT_THROW((void)makeDiagnosticMessage(MessageKey::ParseErrorUnclosedQuote,
                                             {numberParam("count", 1U)}),
                 std::logic_error);
}

TEST(DiagnosticMessagesTest, RejectsParametersInTheWrongOrder) {
    EXPECT_THROW(
        (void)makeDiagnosticMessage(MessageKey::FieldCountMismatch,
                                    {numberParam("found", 22U), numberParam("expected", 23U)}),
        std::logic_error);
}

TEST(DiagnosticMessagesTest, RejectsAWrongParameterName) {
    EXPECT_THROW(
        (void)makeDiagnosticMessage(MessageKey::FieldCountMismatch,
                                    {numberParam("expected", 23U), numberParam("seen", 22U)}),
        std::logic_error);
}

TEST(DiagnosticMessagesTest, RejectsTextWhereANumberIsExpected) {
    EXPECT_THROW(
        (void)makeDiagnosticMessage(MessageKey::FieldCountMismatch,
                                    {textParam("expected", "23"), numberParam("found", 22U)}),
        std::logic_error);
}

TEST(DiagnosticMessagesTest, RejectsANumberWhereTextIsExpected) {
    EXPECT_THROW((void)makeDiagnosticMessage(MessageKey::InconsistentValueTradeSide,
                                             {numberParam("field", 1U)}),
                 std::logic_error);
}

TEST(DiagnosticMessagesTest, RejectsATermThatIsNotInTheTermList) {
    EXPECT_THROW((void)makeDiagnosticMessage(MessageKey::MissingFieldSymbolAndName,
                                             {termParam("rowKind", "row_kind.unknown")}),
                 std::logic_error);
}

TEST(DiagnosticMessagesTest, RejectsAKeyWithoutADefinition) {
    EXPECT_THROW((void)messageDefinition(static_cast<MessageKey>(999)), std::logic_error);
}

TEST(DiagnosticMessagesTest, RejectsAnIdWithoutATableRow) {
    EXPECT_THROW((void)messageText("not_a_message", MessageLanguage::English), std::logic_error);
}

TEST(DiagnosticMessagesTest, ReadsAQuotedTableWithCommasInsideTheTexts) {
    const auto rows = parseMessageTable("key,en,sl\n"
                                        "sample.key,\"One, two\",\"Ena, dva\"\n"
                                        "plain,Plain,Navadno\n");

    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(rows[0].mKey, "sample.key");
    EXPECT_EQ(rows[0].mEnglish, "One, two");
    EXPECT_EQ(rows[0].mSlovenian, "Ena, dva");
    EXPECT_EQ(rows[1].mKey, "plain");
    EXPECT_EQ(rows[1].mEnglish, "Plain");
    EXPECT_EQ(rows[1].mSlovenian, "Navadno");
}

TEST(DiagnosticMessagesTest, RejectsATableWithAnotherHeader) {
    EXPECT_THROW((void)parseMessageTable("key,english,slovenian\nsample,One,Ena\n"),
                 std::logic_error);
}

TEST(DiagnosticMessagesTest, RejectsATableRowWithTheWrongFieldCount) {
    // The second row has 2 fields: the Slovenian text is missing.
    EXPECT_THROW((void)parseMessageTable("key,en,sl\nsample,One\n"), std::logic_error);
}

TEST(DiagnosticMessagesTest, ReadsOnlyTheTextInsideTheView) {
    const std::string_view text{"key,en,sl\nsample,One,Ena\nnot part of the table"};

    // The first 25 characters (10 for the header line, 15 for the sample line) end after
    // "Ena\n"; the text after them is not in the view.
    const auto rows = parseMessageTable(text.substr(0, 25));

    ASSERT_EQ(rows.size(), 1U);

    EXPECT_EQ(rows[0].mKey, "sample");
    EXPECT_EQ(rows[0].mEnglish, "One");
    EXPECT_EQ(rows[0].mSlovenian, "Ena");
}

TEST(DiagnosticMessagesTest, EmbedsTheTableWithItsHeader) {
    EXPECT_TRUE(diagnosticMessagesCsv().starts_with("key,en,sl\n"));
}

TEST(DiagnosticMessagesTest, EmbedsTheTableUpToItsLastByte) {
    const auto table = diagnosticMessagesCsv();

    // The byte array has no terminating zero: the text ends with the newline of the last row.
    EXPECT_TRUE(table.ends_with("row_kind.private_market,private-market row,zasebni trg\n"));
    EXPECT_EQ(table.find('\0'), std::string_view::npos);
}

} // namespace
