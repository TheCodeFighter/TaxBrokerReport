#include "xml/dho_xml_writer.hpp"
#include "xml_common.hpp"

#include <pugixml.hpp>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace taxbroker {

using namespace xml_common;

namespace {

constexpr std::string_view kDhoNamespace = "http://edavki.durs.si/Documents/Schemas/Doh_DHO_4.xsd";

constexpr ReportType kReport = ReportType::BankDepositInterest;
// `typeHeaderYear` of the schema starts at 2006.
constexpr MetadataFields kMetadataFields{
    .mYear = "Year", .mMinimumYear = 2006, .mPhone = "PhoneNumber", .mEmail = "Email"};

// Interest type 1: interest on deposits at banks in Slovenia and other EU states. Type 2 (bonds
// of the Republic of Slovenia) is not written.
constexpr std::string_view kBankDepositType = "1";

// The payer country of a Slovenian bank: the form leaves the foreign tax empty for it.
constexpr std::string_view kSlovenia = "SI";

// Limits of the schema: texts, row count, and amounts of at most 12 digits with 2 decimals.
constexpr std::size_t kMaximumTitleLength = 50;
constexpr std::size_t kMaximumAddressLength = 100;
constexpr std::size_t kMaximumRows = 999;
constexpr std::int64_t kMaximumCents = 999'999'999'999; // 9999999999.99

std::string_view interestType(DepositInterestKind aKind) {
    switch (aKind)
    {
    case DepositInterestKind::BankDeposit:
        return kBankDepositType;
    }

    throw std::logic_error{"Unknown deposit interest kind"};
}

bool isSlovenianPayer(const DhoInterestRow& aRow) {
    return aRow.mPayer.mCountry && *aRow.mPayer.mCountry == kSlovenia;
}

/// The payer name of a row for the context of its errors, only when it is valid XML text.
ErrorIdentity rowIdentity(const DhoInterestRow& aRow) {
    return payerIdentity(aRow.mPayer.mName);
}

/// Optional text of 1 to `aMaximumLength` characters that XML accepts.
void checkLimitedText(std::vector<ProcessingDiagnostic>& aDiagnostics,
                      std::string_view aField,
                      const std::optional<std::string>& aText,
                      std::size_t aMaximumLength,
                      const ErrorIdentity& aIdentity) {
    if (!aText)
    {
        return;
    }

    const auto length = xmlTextLength(*aText);

    if (!length || *length == 0 || *length > aMaximumLength)
    {
        addOutOfRange(aDiagnostics, kReport, aField, aIdentity);
    }
}

void checkRow(std::vector<ProcessingDiagnostic>& aDiagnostics, const DhoInterestRow& aRow) {
    const auto identity = rowIdentity(aRow);
    const auto& payer = aRow.mPayer;

    if (payer.mTaxNumber && !isTaxNumber(*payer.mTaxNumber))
    {
        addOutOfRange(aDiagnostics, kReport, "TaxID", identity);
    }

    if (payer.mIdentificationNumber && !xmlTextLength(*payer.mIdentificationNumber))
    {
        addOutOfRange(aDiagnostics, kReport, "IDeu", identity);
    }

    checkLimitedText(aDiagnostics, "Title", payer.mName, kMaximumTitleLength, identity);
    checkLimitedText(aDiagnostics, "Address", payer.mAddress, kMaximumAddressLength, identity);
    checkCountry(aDiagnostics, kReport, "PayerCountryCode", payer.mCountry, identity);
    checkCountry(aDiagnostics, kReport, "SourceCountryCode", aRow.mSourceCountry, identity);

    // The form reports interest above zero: a value that rounds to 0.00 is no payment to file.
    const auto interestCents = roundedCents(aRow.mInterest);

    if (interestCents <= 0 || interestCents > kMaximumCents)
    {
        addOutOfRange(aDiagnostics, kReport, "InterestEarned", identity);
    }

    const auto taxCents = roundedCents(aRow.mForeignTax);

    if (aRow.mForeignTax < 0 || taxCents > kMaximumCents ||
        (isSlovenianPayer(aRow) && taxCents > 0))
    {
        addOutOfRange(aDiagnostics, kReport, "ForeignTaxPaid", identity);
    }
}

/// The sums of the rounded rows, which the totals element must equal.
struct Totals {
    std::int64_t mInterestCents{};
    std::int64_t mForeignTaxCents{};
};

/// Sums the rows that passed `checkRow`. The caller has checked the row count, so the sum of at
/// most 999 amounts of 12 digits cannot overflow.
Totals sumRows(const DepositInterestReportDHO& aReport) {
    Totals totals;

    for (const auto& row : aReport.mRows)
    {
        const auto interestCents = roundedCents(row.mInterest);
        const auto taxCents = roundedCents(row.mForeignTax);

        if (interestCents > 0 && interestCents <= kMaximumCents)
        {
            totals.mInterestCents += interestCents;
        }

        if (row.mForeignTax >= 0 && taxCents <= kMaximumCents)
        {
            totals.mForeignTaxCents += taxCents;
        }
    }

    return totals;
}

void appendTaxpayerData(pugi::xml_node aDho, const ReportMetadata& aMetadata) {
    const auto& taxpayer = aMetadata.mTaxpayer;
    auto data = aDho.append_child("Doh_DHO_TaxPayerData");

    appendOptionalText(data, "Email", taxpayer.mEmail);
    appendOptionalText(data, "PhoneNumber", taxpayer.mPhoneNumber);
    appendBoolean(data, "SelfReport", aMetadata.mFilingType == FilingType::SelfReport);
    appendBoolean(data, "IsResident", taxpayer.mIsResident);
}

void appendInterest(pugi::xml_node aDho, const DhoInterestRow& aRow) {
    const auto& payer = aRow.mPayer;
    auto interest = aDho.append_child("Doh_DHO_InterestEarned");

    appendOptionalText(interest, "TaxID", payer.mTaxNumber);
    appendOptionalText(interest, "IDeu", payer.mIdentificationNumber);
    appendOptionalText(interest, "Title", payer.mName);
    appendOptionalText(interest, "Address", payer.mAddress);
    appendOptionalText(interest, "PayerCountryCode", payer.mCountry);
    appendText(interest, "InterestType", interestType(aRow.mKind));
    appendText(interest, "InterestEarned", amountText(roundedCents(aRow.mInterest)));

    // The form wants the foreign tax empty for a Slovenian payer.
    if (!isSlovenianPayer(aRow))
    {
        appendText(interest, "ForeignTaxPaid", amountText(roundedCents(aRow.mForeignTax)));
    }

    appendOptionalText(interest, "SourceCountryCode", aRow.mSourceCountry);
}

void appendTotals(pugi::xml_node aDho, const ReportMetadata& aMetadata, const Totals& aTotals) {
    auto totals = aDho.append_child("Doh_DHO_Totals");

    appendText(totals, "TotalInterestEarned", amountText(aTotals.mInterestCents));
    appendText(totals, "TotalForeignTaxPaid", amountText(aTotals.mForeignTaxCents));
    appendText(totals, "Year", std::to_string(aMetadata.mReportingYear));
}

} // namespace

XmlWriteResult DhoXmlWriter::write(const ReportMetadata& aMetadata,
                                   const DepositInterestReportDHO& aReport) {
    XmlWriteResult result;

    checkMetadata(result.mDiagnostics, kReport, aMetadata, kMetadataFields);

    if (aReport.mRows.size() > kMaximumRows)
    {
        addOutOfRange(result.mDiagnostics, kReport, "Doh_DHO_InterestEarned");
    }

    for (const auto& row : aReport.mRows)
    {
        checkRow(result.mDiagnostics, row);
    }

    Totals totals;

    if (aReport.mRows.size() <= kMaximumRows)
    {
        totals = sumRows(aReport);

        if (totals.mInterestCents > kMaximumCents)
        {
            addOutOfRange(result.mDiagnostics, kReport, "TotalInterestEarned");
        }

        if (totals.mForeignTaxCents > kMaximumCents)
        {
            addOutOfRange(result.mDiagnostics, kReport, "TotalForeignTaxPaid");
        }
    }

    if (!result.mDiagnostics.empty())
    {
        return result;
    }

    pugi::xml_document document;
    auto envelope = beginDocument(document, kDhoNamespace);

    appendHeader(envelope, aMetadata);
    envelope.append_child("edp:Signatures");

    auto body = envelope.append_child("body");

    body.append_child("edp:bodyContent");

    auto dho = body.append_child("Doh_DHO");

    appendTaxpayerData(dho, aMetadata);

    for (const auto& row : aReport.mRows)
    {
        appendInterest(dho, row);
    }

    appendTotals(dho, aMetadata, totals);

    result.mXml = saveDocument(document);

    return result;
}

} // namespace taxbroker
