#include "xml/div_xml_writer.hpp"
#include "taxbroker/application_result.hpp"
#include "xml_common.hpp"

#include <pugixml.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>

namespace taxbroker {

using namespace xml_common;

namespace {

constexpr std::string_view kDivNamespace = "http://edavki.durs.si/Documents/Schemas/Doh_Div_3.xsd";

constexpr ReportType kReport = ReportType::Dividends;
constexpr MetadataFields kMetadataFields{.mYear = "Period",
                                         .mMinimumYear = kFourDigitYear,
                                         .mPhone = "PhoneNumber",
                                         .mEmail = "EmailAddress"};

// Form type 1: a dividend. The instructions list a fund distribution as type 4; whether that
// code is right for a foreign fund is unconfirmed, so every payment is written as type 1.
constexpr std::string_view kDividendType = "1";

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
    return payerIdentity(aRow.mPayer.mName, aRow.mIsin);
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

    checkCountry(aDiagnostics, kReport, "PayerCountry", payer.mCountry, identity);
    checkCountry(aDiagnostics, kReport, "SourceCountry", aRow.mSourceCountry, identity);

    // The form reports a gross dividend: a value that rounds to 0.00 is no payment to file.
    if (roundedCents(aRow.mGrossIncome) <= 0)
    {
        addOutOfRange(aDiagnostics, kReport, "Value", identity);
    }

    if (aRow.mWithholding.mForeignTax < 0)
    {
        addOutOfRange(aDiagnostics, kReport, "ForeignTax", identity);
    }

    // A claim needs a foreign tax and a trimmed text a claim may carry; true and false are not one.
    if (aRow.mTreatyClaim)
    {
        const auto& statement = aRow.mTreatyClaim->mStatement;

        if (aRow.mWithholding.mForeignTax <= 0 || !xmlTextLength(statement) ||
            !isValidTreatyStatement(statement) || trimmedTreatyStatement(statement) != statement)
        {
            addOutOfRange(aDiagnostics, kReport, "ReliefStatement", identity);
        }
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
    // A claim writes its treaty text, which eDavki needs to apply the relief; otherwise `false`.
    if (aRow.mTreatyClaim)
    {
        appendText(dividend, "ReliefStatement", aRow.mTreatyClaim->mStatement);
    }
    else
    {
        appendBoolean(dividend, "ReliefStatement", false);
    }
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
