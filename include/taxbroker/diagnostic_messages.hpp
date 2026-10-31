#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace taxbroker {

/// One value per user-facing diagnostic message. The value order is the order of the definitions
/// in `diagnostic_messages.cpp`. The text ids are API values: never rename or reuse them.
enum class MessageKey {
    ParseErrorUnclosedQuote,
    ParseErrorUnreadableFile,
    ParseErrorLineBreakInField,
    InvalidHeaderOtherSeparator,
    InvalidHeaderMissingColumns,
    FieldCountMismatch,
    LocalizedNumberFormat,
    MissingFieldDatetime,
    MissingFieldRequiredField,
    MissingFieldSymbolAndName,
    MissingFieldName,
    MissingFieldAssetClass,
    InvalidValueDatetime,
    InvalidValueField,
    InconsistentValueDatetimeDate,
    InconsistentValueTradeSide,
    InconsistentValueAssetClass,
    UnsupportedAssetClassUnrecognized,
    UnsupportedAssetClassTaxUnsupported,
    UnsupportedAssetClassInterestMapping,
    UnsupportedRowTypeRecognized,
    UnsupportedRowTypeInterestType,
    UnknownRowType,
    DuplicateSourceIndex,
    InconsistentSourceIndexNoSource,
    InconsistentSourceIndexMismatch,
    ConflictingDuplicate,
    DuplicateTransactionInFile,
    InstrumentNameConflict,
    InstrumentAssetClassConflict,
    UnrepresentableValue,
    ArithmeticOverflow,
    InvalidExchangeRate,
    CorporateActionUnrepresentable,
    InvalidReportingYear,
    IncompleteHistory,
    XmlValueOutOfRange,
    UnsupportedTaxTreatmentCapitalClass,
    UnsupportedTaxTreatmentPrivateMarketSkipped,
    TaxClassificationUnknownCapital,
    MissingExchangeRateTrade,
    LossEligibilityUnresolvedConfirmationRequired,
    LossEligibilityUnresolvedCancelled,
    LossEligibilityUnresolvedAssumedBlocked,
    UnsupportedTaxTreatmentPartialLossEligibility,
    CorporateActionNotAppliedLossWindow,
    AnswerRejectedStaleRevision,
    AnswerRejectedUnknownRequest,
    AnswerRejectedDuplicateAnswer,
    AnswerRejectedAnswerDoesNotFit,
    AnswerRejectedUnknownDecision,
    AnswerRejectedStaleRevisionDelete,
    AnswerRejectedDuplicateDelete,
    CorporateActionAmbiguousRatioRequired,
    CorporateActionAmbiguousCancelled,
    CorporateActionAmbiguousSameDay,
    CorporateActionNotAppliedWaitingForEarlierAction,
    AnswerRejectedRatioNotPositive,
    AnswerRejectedRatioUnchanged,
    AnswerRejectedRatioOutOfRange,
    AnswerRejectedPreviewMismatch,
    AnswerRejectedEffectiveDateOutOfRange,
    AnswerRejectedEffectiveDateWithoutUnits,
    CorporateActionAppliedUserRatio,
    CorporateActionQuantityRounded,
    CorporateActionUnrepresentableRoundingBudget,
    CorporateActionMissingPosition,
    CorporateActionAmbiguousDifferentDates,
    UnsupportedCorporateActionMerger,
    CorporateActionNotAppliedFilingRows,
    CorporateActionAmbiguousRatioRequiredBeforeSale,
    AnswerRejectedLaterSaleTooLarge,
};

enum class MessageParamKind {
    Number, ///< A whole number; the frontend formats it.
    Text,   ///< Shown as it is and never translated, such as one of our column names.
    Term,   ///< A term id from the `term` namespace; the frontend translates it too.
};

enum class MessageLanguage {
    English,
    Slovenian,
};

/// Term ids for values that the frontend translates inside a message.
namespace term {

inline constexpr std::string_view kRowKindTrade = "row_kind.trade";
inline constexpr std::string_view kRowKindDividend = "row_kind.dividend";
inline constexpr std::string_view kRowKindBondInterest = "row_kind.bond_interest";
inline constexpr std::string_view kRowKindBrokerInterest = "row_kind.broker_interest";
inline constexpr std::string_view kRowKindCorporateAction = "row_kind.corporate_action";
inline constexpr std::string_view kRowKindBenefit = "row_kind.benefit";
inline constexpr std::string_view kRowKindPrivateMarket = "row_kind.private_market";

} // namespace term

using MessageParamValue = std::variant<std::uint64_t, std::string>;

struct MessageParam {
    std::string mName;
    MessageParamValue mValue;

    bool operator==(const MessageParam&) const = default;
};

[[nodiscard]] MessageParam numberParam(std::string aName, std::uint64_t aValue);
[[nodiscard]] MessageParam textParam(std::string aName, std::string aValue);
[[nodiscard]] MessageParam termParam(std::string aName, std::string_view aTerm);

struct MessageParamDefinition {
    std::string_view mName;
    MessageParamKind mKind{};
};

struct MessageDefinition {
    MessageKey mKey{};
    std::string_view mId; ///< "field_count_mismatch" or "<code>.<sub_code>".
    std::span<const MessageParamDefinition> mParams;
};

/// A message key with its parameters and the English sentence for logs, the CLI and developer
/// tools. The frontend builds its text from the key and the parameters.
struct DiagnosticMessage {
    MessageKey mKey{};
    std::vector<MessageParam> mParams;
    std::string mEnglish;
};

struct MessageTableRow {
    std::string mKey;
    std::string mEnglish;
    std::string mSlovenian;
};

/// Every message key with its parameters, in MessageKey order.
[[nodiscard]] std::span<const MessageDefinition> messageDefinitions();

/// Throws std::logic_error when the key has no definition.
[[nodiscard]] const MessageDefinition& messageDefinition(MessageKey aKey);

/// Every term id, in the order of the `term` namespace.
[[nodiscard]] std::span<const std::string_view> termKeys();

/// The text of `resources/i18n/diagnostic_messages.csv`, embedded at build time.
[[nodiscard]] std::string_view diagnosticMessagesCsv();

/// Reads a message table with the header `key,en,sl`. Throws std::logic_error when the header or a
/// row does not have these three columns.
[[nodiscard]] std::vector<MessageTableRow> parseMessageTable(std::string_view aCsvText);

/// The embedded table, parsed once.
[[nodiscard]] const std::vector<MessageTableRow>& messageTable();

/// Throws std::logic_error when the table has no row for the id.
[[nodiscard]] const std::string& messageText(std::string_view aId, MessageLanguage aLanguage);

/// Replaces each `{name}` of the template with the text of the parameter of that name. Throws
/// std::logic_error when the parameters differ from the definition of the key in name, order or
/// value type, when a Term is not a known term, or when the template has a `{` without a closing
/// brace or a placeholder without a parameter.
[[nodiscard]] std::string fillMessageTemplate(const std::string& aTemplate,
                                              MessageKey aKey,
                                              const std::vector<MessageParam>& aParams,
                                              MessageLanguage aLanguage);

/// Builds the sentence of a message in one language from the table; the errors are those of
/// fillMessageTemplate.
[[nodiscard]] std::string
renderMessage(MessageKey aKey, const std::vector<MessageParam>& aParams, MessageLanguage aLanguage);

/// Checks the parameters and renders the English sentence. A mismatch is a programming error.
[[nodiscard]] DiagnosticMessage makeDiagnosticMessage(MessageKey aKey,
                                                      std::vector<MessageParam> aParams = {});

} // namespace taxbroker
