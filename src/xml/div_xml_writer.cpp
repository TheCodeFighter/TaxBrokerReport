#include "xml/div_xml_writer.hpp"
#include "utils/numeric_util.hpp"
#include "xml_common.hpp"

#include <pugixml.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace taxbroker {

using namespace xml_common;

namespace {

constexpr std::string_view kDivNamespace = "http://edavki.durs.si/Documents/Schemas/Doh_Div_3.xsd";

constexpr ReportType kReport = ReportType::Dividends;
constexpr MetadataFields kMetadataFields{
    .mYear = "Period", .mPhone = "PhoneNumber", .mEmail = "EmailAddress"};

// Form type 1: a dividend. The instructions list a fund distribution as type 4; whether that
// code is right for a foreign fund is unconfirmed, so every payment is written as type 1.
constexpr std::string_view kDividendType = "1";

constexpr std::size_t kCountryLength = 2;

// `Money` has four decimals and the form two, so a cent is 100 stored steps.
constexpr Money kStepsPerCent = 100;
constexpr Money kHalfCent = 50;
constexpr std::int64_t kCentsPerUnit = 100;
constexpr std::size_t kAmountDecimals = 2;

/// The amount in cents, rounded once, halves up. Divides before it adds, so the largest `Money`
/// value cannot overflow. A negative amount is cut towards zero, not rounded: the caller rejects
/// every amount that is not above zero, and that is the same for the cut and the rounded value.
std::int64_t roundedCents(Money aValue) {
    std::int64_t cents = aValue / kStepsPerCent;

    if (aValue % kStepsPerCent >= kHalfCent)
    {
        ++cents;
    }

    return cents;
}

/// The cents with two decimals, such as `12.35`. Only for values that are not negative.
std::string amountText(std::int64_t aCents) {
    return formatFixedPoint(aCents, kCentsPerUnit, kAmountDecimals);
}

bool isCountryCode(std::string_view aText) {
    return aText.size() == kCountryLength &&
           std::all_of(aText.begin(), aText.end(), [](char aCharacter) {
               return aCharacter >= 'A' && aCharacter <= 'Z';
           });
}

std::string_view dividendType(DividendKind aKind) {
    switch (aKind)
    {
    case DividendKind::Ordinary:
        return kDividendType;
    }

    throw std::logic_error{"Unknown dividend kind"};
}

/// The ISIN and the payer name of a row for the context of its errors, each only when it is valid
/// XML text.
ErrorIdentity rowIdentity(const DivDividendRow& aRow) {
    ErrorIdentity identity;

    if (xmlTextLength(aRow.mIsin))
    {
        identity.mIsin = aRow.mIsin;
    }

    if (aRow.mPayer.mName && !aRow.mPayer.mName->empty() && xmlTextLength(*aRow.mPayer.mName))
    {
        identity.mName = aRow.mPayer.mName;
    }

    return identity;
}

void checkCountry(std::vector<ProcessingDiagnostic>& aDiagnostics,
                  std::string_view aField,
                  const std::optional<std::string>& aCountry,
                  const ErrorIdentity& aIdentity) {
    if (aCountry && !isCountryCode(*aCountry))
    {
        addOutOfRange(aDiagnostics, kReport, aField, aIdentity);
    }
}

void checkRow(std::vector<ProcessingDiagnostic>& aDiagnostics,
              int aReportingYear,
              const DivDividendRow& aRow) {
    const auto identity = rowIdentity(aRow);
    const auto& payer = aRow.mPayer;

    // The builder keeps only rows of the year; a row of another year must not be filed.
    const std::chrono::year_month_day date{std::chrono::sys_days{aRow.mDate}};

    if (!dateText(aRow.mDate) || static_cast<int>(date.year()) != aReportingYear)
    {
        addOutOfRange(aDiagnostics, kReport, "Date", identity);
    }

    if (payer.mTaxNumber && !isTaxNumber(*payer.mTaxNumber))
    {
        addOutOfRange(aDiagnostics, kReport, "PayerTaxNumber", identity);
    }

    const auto checkPayerText = [&aDiagnostics,
                                 &identity](std::string_view aField,
                                            const std::optional<std::string>& aText) {
        if (aText && !xmlTextLength(*aText))
        {
            addOutOfRange(aDiagnostics, kReport, aField, identity);
        }
    };

    checkPayerText("PayerIdentificationNumber", payer.mIdentificationNumber);
    checkPayerText("PayerName", payer.mName);
    checkPayerText("PayerAddress", payer.mAddress);

    checkCountry(aDiagnostics, "PayerCountry", payer.mCountry, identity);
    checkCountry(aDiagnostics, "SourceCountry", aRow.mSourceCountry, identity);

    // The form reports a gross dividend: a value that rounds to 0.00 is no payment to file.
    if (roundedCents(aRow.mGrossIncome) <= 0)
    {
        addOutOfRange(aDiagnostics, kReport, "Value", identity);
    }

    if (aRow.mWithholding.mForeignTax < 0)
    {
        addOutOfRange(aDiagnostics, kReport, "ForeignTax", identity);
    }
}

void appendDivHeader(pugi::xml_node aBody, const ReportMetadata& aMetadata) {
    const auto& taxpayer = aMetadata.mTaxpayer;
    auto div = aBody.append_child("Doh_Div");

    appendText(div, "Period", std::to_string(aMetadata.mReportingYear));
    appendOptionalText(div, "EmailAddress", taxpayer.mEmail);
    appendOptionalText(div, "PhoneNumber", taxpayer.mPhoneNumber);
    appendBoolean(div, "IsResident", taxpayer.mIsResident);
    appendBoolean(div, "SelfReport", aMetadata.mFilingType == FilingType::SelfReport);
}

void appendDividend(pugi::xml_node aBody, const DivDividendRow& aRow) {
    const auto& payer = aRow.mPayer;
    auto dividend = aBody.append_child("Dividend");

    appendText(dividend, "Date", dateText(aRow.mDate).value());
    appendOptionalText(dividend, "PayerTaxNumber", payer.mTaxNumber);
    appendOptionalText(dividend, "PayerIdentificationNumber", payer.mIdentificationNumber);
    appendOptionalText(dividend, "PayerName", payer.mName);
    appendOptionalText(dividend, "PayerAddress", payer.mAddress);
    appendOptionalText(dividend, "PayerCountry", payer.mCountry);
    appendText(dividend, "Type", dividendType(aRow.mKind));
    appendText(dividend, "Value", amountText(roundedCents(aRow.mGrossIncome)));
    appendText(dividend, "ForeignTax", amountText(roundedCents(aRow.mWithholding.mForeignTax)));
    appendOptionalText(dividend, "SourceCountry", aRow.mSourceCountry);
    // A confirmed treaty claim is `true`; the treaty text is not written.
    appendBoolean(dividend, "ReliefStatement", aRow.mTreatyClaim.has_value());
}

} // namespace

XmlWriteResult DivXmlWriter::write(const ReportMetadata& aMetadata,
                                   const DividendReportDiv& aReport) {
    XmlWriteResult result;

    checkMetadata(result.mDiagnostics, kReport, aMetadata, kMetadataFields);

    for (const auto& row : aReport.mRows)
    {
        checkRow(result.mDiagnostics, aMetadata.mReportingYear, row);
    }

    if (!result.mDiagnostics.empty())
    {
        return result;
    }

    pugi::xml_document document;
    auto envelope = beginDocument(document, kDivNamespace);

    appendHeader(envelope, aMetadata);
    envelope.append_child("edp:Signatures");

    // The schema has no `edp:bodyContent` here; the `Dividend` rows follow `Doh_Div` in `body`.
    auto body = envelope.append_child("body");

    appendDivHeader(body, aMetadata);

    for (const auto& row : aReport.mRows)
    {
        appendDividend(body, row);
    }

    result.mXml = saveDocument(document);

    return result;
}

} // namespace taxbroker
