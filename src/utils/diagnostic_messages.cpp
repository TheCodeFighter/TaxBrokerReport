#include "taxbroker/diagnostic_messages.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <csv.hpp>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

using taxbroker::MessageDefinition;
using taxbroker::MessageKey;
using taxbroker::MessageParamDefinition;
using taxbroker::MessageParamKind;

constexpr std::array kCountAndColumns{
    MessageParamDefinition{"count", MessageParamKind::Number},
    MessageParamDefinition{"columns", MessageParamKind::Text},
};
constexpr std::array kExpectedAndFound{
    MessageParamDefinition{"expected", MessageParamKind::Number},
    MessageParamDefinition{"found", MessageParamKind::Number},
};
constexpr std::array kRowCount{
    MessageParamDefinition{"rowCount", MessageParamKind::Number},
};
constexpr std::array kFieldAndRowKind{
    MessageParamDefinition{"field", MessageParamKind::Text},
    MessageParamDefinition{"rowKind", MessageParamKind::Term},
};
constexpr std::array kRowKind{
    MessageParamDefinition{"rowKind", MessageParamKind::Term},
};
constexpr std::array kField{
    MessageParamDefinition{"field", MessageParamKind::Text},
};
constexpr std::array kDefaultHour{
    MessageParamDefinition{"defaultHour", MessageParamKind::Number},
};
constexpr std::array kAssetClass{
    MessageParamDefinition{"assetClass", MessageParamKind::Text},
};
constexpr std::array kTypeAndCategory{
    MessageParamDefinition{"type", MessageParamKind::Text},
    MessageParamDefinition{"category", MessageParamKind::Text},
};
constexpr std::array kCurrency{
    MessageParamDefinition{"currency", MessageParamKind::Text},
};
constexpr std::array kRequestId{
    MessageParamDefinition{"requestId", MessageParamKind::Text},
};
constexpr std::array kDecisionId{
    MessageParamDefinition{"decisionId", MessageParamKind::Text},
};
constexpr std::array kRequestIdAndField{
    MessageParamDefinition{"requestId", MessageParamKind::Text},
    MessageParamDefinition{"field", MessageParamKind::Text},
};
constexpr std::array kNewAndOldSharesAndDate{
    MessageParamDefinition{"newShares", MessageParamKind::Number},
    MessageParamDefinition{"oldShares", MessageParamKind::Number},
    MessageParamDefinition{"effectiveDate", MessageParamKind::Text},
};
constexpr std::array kDays{
    MessageParamDefinition{"days", MessageParamKind::Number},
};
constexpr std::array kSaleDateAndDays{
    MessageParamDefinition{"saleDate", MessageParamKind::Text},
    MessageParamDefinition{"days", MessageParamKind::Number},
};
constexpr std::array kRequestIdAndDateRange{
    MessageParamDefinition{"requestId", MessageParamKind::Text},
    MessageParamDefinition{"earliestDate", MessageParamKind::Text},
    MessageParamDefinition{"latestDate", MessageParamKind::Text},
};
constexpr std::array kEarlierDate{
    MessageParamDefinition{"earlierDate", MessageParamKind::Text},
};
constexpr std::array kYearRange{
    MessageParamDefinition{"minYear", MessageParamKind::Number},
    MessageParamDefinition{"maxYear", MessageParamKind::Number},
};

/// In MessageKey order; definitionsFollowKeyOrder() checks that at compile time.
constexpr std::array kDefinitions{
    MessageDefinition{MessageKey::ParseErrorUnclosedQuote, "parse_error.unclosed_quote", {}},
    MessageDefinition{MessageKey::ParseErrorUnreadableFile, "parse_error.unreadable_file", {}},
    MessageDefinition{
        MessageKey::ParseErrorLineBreakInField, "parse_error.line_break_in_field", {}},
    MessageDefinition{
        MessageKey::InvalidHeaderOtherSeparator, "invalid_header.other_separator", {}},
    MessageDefinition{MessageKey::InvalidHeaderMissingColumns,
                      "invalid_header.missing_columns",
                      kCountAndColumns},
    MessageDefinition{MessageKey::FieldCountMismatch, "field_count_mismatch", kExpectedAndFound},
    MessageDefinition{MessageKey::LocalizedNumberFormat, "localized_number_format", kRowCount},
    MessageDefinition{MessageKey::MissingFieldDatetime, "missing_field.datetime", {}},
    MessageDefinition{
        MessageKey::MissingFieldRequiredField, "missing_field.required_field", kFieldAndRowKind},
    MessageDefinition{
        MessageKey::MissingFieldSymbolAndName, "missing_field.symbol_and_name", kRowKind},
    MessageDefinition{MessageKey::MissingFieldName, "missing_field.name", {}},
    MessageDefinition{MessageKey::MissingFieldAssetClass, "missing_field.asset_class", {}},
    MessageDefinition{MessageKey::InvalidValueDatetime, "invalid_value.datetime", {}},
    MessageDefinition{MessageKey::InvalidValueField, "invalid_value.field", kFieldAndRowKind},
    MessageDefinition{MessageKey::InconsistentValueDatetimeDate,
                      "inconsistent_value.datetime_date",
                      kDefaultHour},
    MessageDefinition{
        MessageKey::InconsistentValueTradeSide, "inconsistent_value.trade_side", kField},
    MessageDefinition{
        MessageKey::InconsistentValueAssetClass, "inconsistent_value.asset_class", {}},
    MessageDefinition{
        MessageKey::UnsupportedAssetClassUnrecognized, "unsupported_asset_class.unrecognized", {}},
    MessageDefinition{MessageKey::UnsupportedAssetClassTaxUnsupported,
                      "unsupported_asset_class.tax_unsupported",
                      kAssetClass},
    MessageDefinition{MessageKey::UnsupportedAssetClassInterestMapping,
                      "unsupported_asset_class.interest_mapping",
                      {}},
    MessageDefinition{MessageKey::UnsupportedRowTypeRecognized,
                      "unsupported_row_type.recognized",
                      kTypeAndCategory},
    MessageDefinition{
        MessageKey::UnsupportedRowTypeInterestType, "unsupported_row_type.interest_type", {}},
    MessageDefinition{MessageKey::UnknownRowType, "unknown_row_type", kTypeAndCategory},
    MessageDefinition{MessageKey::DuplicateSourceIndex, "duplicate_source_index", {}},
    MessageDefinition{
        MessageKey::InconsistentSourceIndexNoSource, "inconsistent_source_index.no_source", {}},
    MessageDefinition{
        MessageKey::InconsistentSourceIndexMismatch, "inconsistent_source_index.mismatch", {}},
    MessageDefinition{MessageKey::ConflictingDuplicate, "conflicting_duplicate", {}},
    MessageDefinition{MessageKey::DuplicateTransactionInFile, "duplicate_transaction_in_file", {}},
    MessageDefinition{MessageKey::InstrumentNameConflict, "instrument_name_conflict", {}},
    MessageDefinition{
        MessageKey::InstrumentAssetClassConflict, "instrument_asset_class_conflict", {}},
    MessageDefinition{MessageKey::UnrepresentableValue, "unrepresentable_value", {}},
    MessageDefinition{MessageKey::ArithmeticOverflow, "arithmetic_overflow", {}},
    MessageDefinition{MessageKey::InvalidExchangeRate, "invalid_exchange_rate", {}},
    MessageDefinition{
        MessageKey::CorporateActionUnrepresentable, "corporate_action_unrepresentable", {}},
    MessageDefinition{MessageKey::InvalidReportingYear, "invalid_reporting_year", kYearRange},
    MessageDefinition{MessageKey::IncompleteHistory, "incomplete_history", {}},
    MessageDefinition{MessageKey::XmlValueOutOfRange, "xml_value_out_of_range", kField},
    MessageDefinition{MessageKey::UnsupportedTaxTreatmentCapitalClass,
                      "unsupported_tax_treatment.capital_class",
                      kAssetClass},
    MessageDefinition{MessageKey::UnsupportedTaxTreatmentPrivateMarketSkipped,
                      "unsupported_tax_treatment.private_market_skipped",
                      {}},
    MessageDefinition{
        MessageKey::TaxClassificationUnknownCapital, "tax_classification_unknown.capital", {}},
    MessageDefinition{MessageKey::CorporateActionNotApplied, "corporate_action_not_applied", {}},
    MessageDefinition{
        MessageKey::MissingExchangeRateTrade, "missing_exchange_rate.trade", kCurrency},
    MessageDefinition{MessageKey::LossEligibilityUnresolvedConfirmationRequired,
                      "loss_eligibility_unresolved.confirmation_required",
                      {}},
    MessageDefinition{MessageKey::LossEligibilityUnresolvedCancelled,
                      "loss_eligibility_unresolved.cancelled",
                      {}},
    MessageDefinition{MessageKey::LossEligibilityUnresolvedAssumedBlocked,
                      "loss_eligibility_unresolved.assumed_blocked",
                      {}},
    MessageDefinition{MessageKey::UnsupportedTaxTreatmentPartialLossEligibility,
                      "unsupported_tax_treatment.partial_loss_eligibility",
                      {}},
    MessageDefinition{MessageKey::CorporateActionNotAppliedLossWindow,
                      "corporate_action_not_applied.loss_window",
                      {}},
    MessageDefinition{
        MessageKey::AnswerRejectedStaleRevision, "answer_rejected.stale_revision", kRequestId},
    MessageDefinition{
        MessageKey::AnswerRejectedUnknownRequest, "answer_rejected.unknown_request", kRequestId},
    MessageDefinition{
        MessageKey::AnswerRejectedDuplicateAnswer, "answer_rejected.duplicate_answer", kRequestId},
    MessageDefinition{MessageKey::AnswerRejectedAnswerDoesNotFit,
                      "answer_rejected.answer_does_not_fit",
                      kRequestId},
    MessageDefinition{
        MessageKey::AnswerRejectedUnknownDecision, "answer_rejected.unknown_decision", kDecisionId},
    MessageDefinition{MessageKey::AnswerRejectedStaleRevisionDelete,
                      "answer_rejected.stale_revision_delete",
                      kDecisionId},
    MessageDefinition{
        MessageKey::AnswerRejectedDuplicateDelete, "answer_rejected.duplicate_delete", kDecisionId},
    MessageDefinition{MessageKey::CorporateActionAmbiguousRatioRequired,
                      "corporate_action_ambiguous.ratio_required",
                      {}},
    MessageDefinition{
        MessageKey::CorporateActionAmbiguousCancelled, "corporate_action_ambiguous.cancelled", {}},
    MessageDefinition{
        MessageKey::CorporateActionAmbiguousSameDay, "corporate_action_ambiguous.same_day", {}},
    MessageDefinition{MessageKey::CorporateActionNotAppliedWaitingForEarlierAction,
                      "corporate_action_not_applied.waiting_for_earlier_action",
                      kEarlierDate},
    MessageDefinition{MessageKey::AnswerRejectedRatioNotPositive,
                      "answer_rejected.ratio_not_positive",
                      kRequestIdAndField},
    MessageDefinition{
        MessageKey::AnswerRejectedRatioUnchanged, "answer_rejected.ratio_unchanged", kRequestId},
    MessageDefinition{MessageKey::AnswerRejectedRatioOutOfRange,
                      "answer_rejected.ratio_out_of_range",
                      kRequestId},
    MessageDefinition{
        MessageKey::AnswerRejectedPreviewMismatch, "answer_rejected.preview_mismatch", kRequestId},
    MessageDefinition{MessageKey::AnswerRejectedEffectiveDateOutOfRange,
                      "answer_rejected.effective_date_out_of_range",
                      kRequestIdAndDateRange},
    MessageDefinition{MessageKey::AnswerRejectedEffectiveDateWithoutUnits,
                      "answer_rejected.effective_date_without_units",
                      kRequestId},
    MessageDefinition{MessageKey::CorporateActionAppliedUserRatio,
                      "corporate_action_applied.user_ratio",
                      kNewAndOldSharesAndDate},
    MessageDefinition{
        MessageKey::CorporateActionQuantityRounded, "corporate_action_quantity_rounded", {}},
    MessageDefinition{MessageKey::CorporateActionUnrepresentableRoundingBudget,
                      "corporate_action_unrepresentable.rounding_budget",
                      {}},
    MessageDefinition{
        MessageKey::CorporateActionMissingPosition, "corporate_action_missing_position", {}},
    MessageDefinition{MessageKey::CorporateActionAmbiguousDifferentDates,
                      "corporate_action_ambiguous.different_dates",
                      kDays},
    MessageDefinition{
        MessageKey::UnsupportedCorporateActionMerger, "unsupported_corporate_action.merger", {}},
    MessageDefinition{MessageKey::CorporateActionNotAppliedLossRuleSplit,
                      "corporate_action_not_applied.loss_rule_split",
                      {}},
    MessageDefinition{MessageKey::CorporateActionNotAppliedFilingRows,
                      "corporate_action_not_applied.filing_rows",
                      {}},
    MessageDefinition{MessageKey::CorporateActionAmbiguousRatioRequiredBeforeSale,
                      "corporate_action_ambiguous.ratio_required_before_sale",
                      kSaleDateAndDays},
    MessageDefinition{MessageKey::AnswerRejectedLaterSaleTooLarge,
                      "answer_rejected.later_sale_too_large",
                      kRequestId},
};

constexpr bool definitionsFollowKeyOrder() {
    for (std::size_t index = 0; index < kDefinitions.size(); ++index)
    {
        if (static_cast<std::size_t>(kDefinitions[index].mKey) != index)
        {
            return false;
        }
    }

    return true;
}

static_assert(definitionsFollowKeyOrder(), "Definitions must follow the MessageKey order");
static_assert(kDefinitions.size() ==
                  static_cast<std::size_t>(MessageKey::AnswerRejectedLaterSaleTooLarge) + 1,
              "Every MessageKey needs one definition");

constexpr std::array<std::string_view, 7> kTermKeys{
    taxbroker::term::kRowKindTrade,
    taxbroker::term::kRowKindDividend,
    taxbroker::term::kRowKindBondInterest,
    taxbroker::term::kRowKindBrokerInterest,
    taxbroker::term::kRowKindCorporateAction,
    taxbroker::term::kRowKindBenefit,
    taxbroker::term::kRowKindPrivateMarket,
};

/// Throws when the parameters differ from the definition. Wrong parameters are a programming
/// error, so the message names only the key id.
void validateParams(const MessageDefinition& aDefinition,
                    const std::vector<taxbroker::MessageParam>& aParams) {
    const std::string id{aDefinition.mId};

    if (aParams.size() != aDefinition.mParams.size())
    {
        throw std::logic_error{"Message " + id + " has the wrong number of parameters"};
    }

    for (std::size_t index = 0; index < aParams.size(); ++index)
    {
        const auto& definition = aDefinition.mParams[index];
        const auto& param = aParams[index];

        if (param.mName != definition.mName)
        {
            throw std::logic_error{"Message " + id + " expects parameter " +
                                   std::string{definition.mName} + " at position " +
                                   std::to_string(index)};
        }

        const bool isNumber = std::holds_alternative<std::uint64_t>(param.mValue);

        if (definition.mKind == MessageParamKind::Number && !isNumber)
        {
            throw std::logic_error{"Message " + id + " expects a number for " +
                                   std::string{definition.mName}};
        }

        if (definition.mKind != MessageParamKind::Number && isNumber)
        {
            throw std::logic_error{"Message " + id + " expects text for " +
                                   std::string{definition.mName}};
        }

        if (definition.mKind == MessageParamKind::Term &&
            std::find(kTermKeys.begin(), kTermKeys.end(), std::get<std::string>(param.mValue)) ==
                kTermKeys.end())
        {
            throw std::logic_error{"Message " + id + " got an unknown term for " +
                                   std::string{definition.mName}};
        }
    }
}

std::string parameterText(const MessageParamDefinition& aDefinition,
                          const taxbroker::MessageParam& aParam,
                          taxbroker::MessageLanguage aLanguage) {
    switch (aDefinition.mKind)
    {
    case MessageParamKind::Number:
        return std::to_string(std::get<std::uint64_t>(aParam.mValue));
    case MessageParamKind::Text:
        return std::get<std::string>(aParam.mValue);
    case MessageParamKind::Term:
        return taxbroker::messageText(std::get<std::string>(aParam.mValue), aLanguage);
    }

    throw std::logic_error{"Unknown message parameter kind"};
}

} // namespace

namespace taxbroker {

MessageParam numberParam(std::string aName, std::uint64_t aValue) {
    return MessageParam{.mName = std::move(aName), .mValue = aValue};
}

MessageParam textParam(std::string aName, std::string aValue) {
    return MessageParam{.mName = std::move(aName), .mValue = std::move(aValue)};
}

MessageParam termParam(std::string aName, std::string_view aTerm) {
    return MessageParam{.mName = std::move(aName), .mValue = std::string{aTerm}};
}

std::string fillMessageTemplate(const std::string& aTemplate,
                                MessageKey aKey,
                                const std::vector<MessageParam>& aParams,
                                MessageLanguage aLanguage) {
    const auto& definition = messageDefinition(aKey);

    validateParams(definition, aParams);

    const std::string id{definition.mId};
    std::string result;
    std::size_t position = 0;

    while (position < aTemplate.size())
    {
        const auto open = aTemplate.find('{', position);

        if (open == std::string::npos)
        {
            result.append(aTemplate, position, std::string::npos);
            break;
        }

        const auto close = aTemplate.find('}', open);

        if (close == std::string::npos)
        {
            throw std::logic_error{"Message " + id + " has a placeholder without a closing brace"};
        }

        result.append(aTemplate, position, open - position);

        const auto name = aTemplate.substr(open + 1, close - open - 1);
        std::size_t index = 0;

        while (index < aParams.size() && aParams[index].mName != name)
        {
            ++index;
        }

        if (index == aParams.size())
        {
            throw std::logic_error{"Message " + id +
                                   " has a placeholder without a parameter: " + name};
        }

        result += parameterText(definition.mParams[index], aParams[index], aLanguage);
        position = close + 1;
    }

    return result;
}

std::span<const MessageDefinition> messageDefinitions() {
    return kDefinitions;
}

const MessageDefinition& messageDefinition(MessageKey aKey) {
    const auto index = static_cast<std::size_t>(aKey);

    if (index >= kDefinitions.size())
    {
        throw std::logic_error{"Unknown message key"};
    }

    return kDefinitions[index];
}

std::span<const std::string_view> termKeys() {
    return kTermKeys;
}

std::vector<MessageTableRow> parseMessageTable(std::string_view aCsvText) {
    csv::CSVFormat format;
    format.delimiter(',').variable_columns(csv::VariableColumnPolicy::KEEP);

    // Not csv::parse: it reads aCsvText.data() up to a zero byte and ignores the view's size.
    std::stringstream stream{std::string{aCsvText}};
    csv::CSVReader reader{stream, format};

    if (reader.get_col_names() != std::vector<std::string>{"key", "en", "sl"})
    {
        throw std::logic_error{"The message table header must be key,en,sl"};
    }

    std::vector<MessageTableRow> rows;

    for (const csv::CSVRow& row : reader)
    {
        if (row.size() != 3)
        {
            throw std::logic_error{"A message table row must have 3 fields"};
        }

        rows.push_back(MessageTableRow{
            .mKey = row[0].get<std::string>(),
            .mEnglish = row[1].get<std::string>(),
            .mSlovenian = row[2].get<std::string>(),
        });
    }

    return rows;
}

const std::vector<MessageTableRow>& messageTable() {
    static const std::vector<MessageTableRow> table = parseMessageTable(diagnosticMessagesCsv());

    return table;
}

const std::string& messageText(std::string_view aId, MessageLanguage aLanguage) {
    const auto& table = messageTable();
    const auto row = std::find_if(table.begin(), table.end(), [aId](const MessageTableRow& aRow) {
        return aRow.mKey == aId;
    });

    if (row == table.end())
    {
        throw std::logic_error{"The message table has no row for " + std::string{aId}};
    }

    return aLanguage == MessageLanguage::English ? row->mEnglish : row->mSlovenian;
}

std::string renderMessage(MessageKey aKey,
                          const std::vector<MessageParam>& aParams,
                          MessageLanguage aLanguage) {
    const auto& definition = messageDefinition(aKey);

    return fillMessageTemplate(messageText(definition.mId, aLanguage), aKey, aParams, aLanguage);
}

DiagnosticMessage makeDiagnosticMessage(MessageKey aKey, std::vector<MessageParam> aParams) {
    auto english = renderMessage(aKey, aParams, MessageLanguage::English);

    return DiagnosticMessage{
        .mKey = aKey,
        .mParams = std::move(aParams),
        .mEnglish = std::move(english),
    };
}

} // namespace taxbroker
