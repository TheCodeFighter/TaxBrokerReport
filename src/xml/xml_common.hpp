#pragma once

#include "taxbroker/processing_diagnostics.hpp"
#include "taxbroker/report_models.hpp"

#include <pugixml.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace taxbroker::xml_common {

/// The ISIN and the name of a row or list for the context of its errors, each only when it is
/// valid XML text: a broken value must not travel on into logs or JSON.
struct ErrorIdentity {
    std::optional<Isin> mIsin;        ///< Empty when the ISIN is not valid XML text.
    std::optional<std::string> mName; ///< Instrument or payer name; empty when unusable.
};

/// The names of the form's own elements that the shared taxpayer check reports.
struct MetadataFields {
    std::string_view mYear;  ///< The element with the reporting year.
    int mMinimumYear{};      ///< The first year the form accepts; always set by the writer.
    std::string_view mPhone; ///< The element with the telephone number.
    std::string_view mEmail; ///< The element with the e-mail address.
};

/// The first year with four digits, the lowest year of a form that sets no later limit.
inline constexpr int kFourDigitYear = 1000;

/// The number of characters of a text that XML accepts: valid UTF-8 without control characters
/// (below U+0020) and without U+FFFE and U+FFFF. Returns nothing for any other text.
[[nodiscard]] std::optional<std::size_t> xmlTextLength(std::string_view aText);

/// Eight digits, not all zero (the schema type is a positive integer).
[[nodiscard]] bool isTaxNumber(std::string_view aText);

/// `YYYY-MM-DD`, or nothing for a year outside 1 to 9999, which `xs:date` cannot hold in four
/// digits (year 0000 does not exist in XML).
[[nodiscard]] std::optional<std::string> dateText(const Date& aDate);

/// Two capital letters A to Z.
[[nodiscard]] bool isCountryCode(std::string_view aText);

/// The amount in cents, rounded once, halves up. A negative amount is cut towards zero, not
/// rounded: the caller rejects a negative amount on its exact value first.
[[nodiscard]] std::int64_t roundedCents(Money aValue);

/// The cents with two decimals, such as `12.35`. Only for values that are not negative.
[[nodiscard]] std::string amountText(std::int64_t aCents);

/// The workflow code (`O` or `I`) and its name for the filing type.
[[nodiscard]] std::string_view workflowCode(FilingType aFilingType);
[[nodiscard]] std::string_view workflowName(FilingType aFilingType);

/// Adds one error for a field that the form does not accept. The value is never part of the
/// message.
void addOutOfRange(std::vector<ProcessingDiagnostic>& aDiagnostics,
                   ReportType aReport,
                   std::string_view aField,
                   const ErrorIdentity& aIdentity = {});

/// Optional text: nothing to check when it is absent.
void checkText(std::vector<ProcessingDiagnostic>& aDiagnostics,
               ReportType aReport,
               std::string_view aField,
               const std::optional<std::string>& aText,
               std::optional<std::size_t> aMaximumLength = std::nullopt);

/// Checks the taxpayer, the year and the contact data that every form's header and body write.
void checkMetadata(std::vector<ProcessingDiagnostic>& aDiagnostics,
                   ReportType aReport,
                   const ReportMetadata& aMetadata,
                   const MetadataFields& aFields);

void appendText(pugi::xml_node aParent, std::string_view aName, std::string_view aText);
void appendBoolean(pugi::xml_node aParent, std::string_view aName, bool aValue);
void appendOptionalText(pugi::xml_node aParent,
                        std::string_view aName,
                        const std::optional<std::string>& aText);

/// Writes the declaration and the `Envelope` with the form's namespace; returns the `Envelope`.
[[nodiscard]] pugi::xml_node beginDocument(pugi::xml_document& aDocument,
                                           std::string_view aNamespace);

/// Writes `edp:Header` (taxpayer and workflow) into the `Envelope`.
void appendHeader(pugi::xml_node aEnvelope, const ReportMetadata& aMetadata);

/// The whole document as UTF-8 text.
[[nodiscard]] std::string saveDocument(const pugi::xml_document& aDocument);

} // namespace taxbroker::xml_common
