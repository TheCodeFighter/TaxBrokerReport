#include "xml/obr_xml_writer.hpp"
#include "taxbroker/application_result.hpp"
#include "xml_common.hpp"

#include <pugixml.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace taxbroker {

using namespace xml_common;

namespace {

constexpr std::string_view kObrNamespace = "http://edavki.durs.si/Documents/Schemas/Doh_Obr_2.xsd";

constexpr ReportType kReport = ReportType::OtherInterest;
constexpr MetadataFields kMetadataFields{.mYear = "Period",
                                         .mMinimumYear = kFourDigitYear,
                                         .mPhone = "TelephoneNumber",
                                         .mEmail = "Email"};

// Interest type 2: debt securities. Type 4: similar financial claims, used for the interest on the
// cash a broker holds; FURS names no code for it, so the code is unconfirmed.
constexpr std::string_view kCouponType = "2";
constexpr std::string_view kCashInterestType = "4";

// The country of a Slovenian payer: the form has no foreign tax for it and wants its tax number.
constexpr std::string_view kSlovenia = "SI";

// Limits of the schema: the identification number and amounts of at most 12 digits with 2
// decimals.
constexpr std::size_t kMaximumIdentificationLength = 30;
constexpr std::int64_t kMaximumCents = 99'999'999'999'999; // 999999999999.99

std::string_view interestType(const ObrInterestRow& aRow) {
    const auto type = [](const auto& aKind) -> std::string_view {
        using Kind = std::decay_t<decltype(aKind)>;

        if constexpr (std::is_same_v<Kind, BondCoupon>)
        {
            return kCouponType;
        }
        else
        {
            static_assert(std::is_same_v<Kind, BrokerCashInterest>);

            return kCashInterestType;
        }
    };

    return std::visit(type, aRow.mKind);
}

bool isSlovenianPayer(const ObrInterestRow& aRow) {
    return aRow.mPayer.mCountry && *aRow.mPayer.mCountry == kSlovenia;
}

/// The payer name of a row, and the ISIN of a coupon, for the context of its errors.
ErrorIdentity rowIdentity(const ObrInterestRow& aRow) {
    const auto* coupon = std::get_if<BondCoupon>(&aRow.mKind);

    return payerIdentity(aRow.mPayer.mName,
                         coupon != nullptr ? std::optional<Isin>{coupon->mIsin} : std::nullopt);
}

/// Optional text that XML accepts and that is not empty, at most `aMaximumLength` characters when
/// a limit is given.
void checkPayerText(std::vector<ProcessingDiagnostic>& aDiagnostics,
                    std::string_view aField,
                    const std::optional<std::string>& aText,
                    std::optional<std::size_t> aMaximumLength,
                    const ErrorIdentity& aIdentity) {
    if (!aText)
    {
        return;
    }

    const auto length = xmlTextLength(*aText);

    if (!length || *length == 0 || (aMaximumLength && *length > *aMaximumLength))
    {
        addOutOfRange(aDiagnostics, kReport, aField, aIdentity);
    }
}

void checkRow(std::vector<ProcessingDiagnostic>& aDiagnostics,
              int aReportingYear,
              const ObrInterestRow& aRow) {
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
        addOutOfRange(aDiagnostics, kReport, "TaxNumber", identity);
    }

    checkPayerText(aDiagnostics,
                   "IdentificationNumber",
                   payer.mIdentificationNumber,
                   kMaximumIdentificationLength,
                   identity);
    checkPayerText(aDiagnostics, "Name", payer.mName, std::nullopt, identity);
    checkPayerText(aDiagnostics, "Address", payer.mAddress, std::nullopt, identity);
    checkCountry(aDiagnostics, kReport, "Country", payer.mCountry, identity);
    checkCountry(aDiagnostics, kReport, "Country2", aRow.mSourceCountry, identity);

    // The form reports interest above zero: a value that rounds to 0.00 is no payment to file.
    const auto valueCents = roundedCents(aRow.mGrossInterest);

    if (valueCents <= 0 || valueCents > kMaximumCents)
    {
        addOutOfRange(aDiagnostics, kReport, "Value", identity);
    }

    // The foreign tax of a Slovenian payer is not written, so its value does not matter.
    if (!isSlovenianPayer(aRow))
    {
        const auto& tax = aRow.mWithholding.mForeignTax;

        if (tax < 0 || roundedCents(tax) > kMaximumCents)
        {
            addOutOfRange(aDiagnostics, kReport, "ForeignTax", identity);
        }
    }

    // A claim needs a foreign tax that is written, and, when it has a text, a trimmed text a claim
    // may carry; a claim without a text is written as `DA`.
    if (aRow.mTreatyClaim)
    {
        const auto& statement = aRow.mTreatyClaim->mStatement;
        const bool badText =
            statement && (!xmlTextLength(*statement) || !isValidTreatyStatement(*statement) ||
                          trimmedTreatyStatement(*statement) != *statement);

        if (aRow.mWithholding.mForeignTax <= 0 || isSlovenianPayer(aRow) || badText)
        {
            addOutOfRange(aDiagnostics, kReport, "ReliefStatement", identity);
        }
    }
}

/// Writes `Doh_Obr` with the taxpayer and period fields, and returns it for the rows.
pugi::xml_node appendObrHeader(pugi::xml_node aBody, const ReportMetadata& aMetadata) {
    const auto& taxpayer = aMetadata.mTaxpayer;
    auto obr = aBody.append_child("Doh_Obr");

    appendBoolean(obr, "SelfReport", false);
    appendText(obr, "Period", std::to_string(aMetadata.mReportingYear));
    appendText(obr, "DocumentWorkflowID", workflowCode(FilingType::Original));
    appendOptionalText(obr, "Email", taxpayer.mEmail);
    appendOptionalText(obr, "TelephoneNumber", taxpayer.mPhoneNumber);
    appendBoolean(obr, "ResidentOfRepublicOfSlovenia", taxpayer.mIsResident);

    if (taxpayer.mIsResident)
    {
        appendText(obr, "Country", kSlovenia);
    }

    return obr;
}

void appendInterest(pugi::xml_node aObr, const ObrInterestRow& aRow) {
    const auto& payer = aRow.mPayer;
    auto interest = aObr.append_child("Interest");

    appendText(interest, "Date", dateText(aRow.mDate).value());
    appendOptionalText(interest, "TaxNumber", payer.mTaxNumber);
    appendOptionalText(interest, "IdentificationNumber", payer.mIdentificationNumber);
    appendOptionalText(interest, "Name", payer.mName);
    appendOptionalText(interest, "Address", payer.mAddress);
    appendOptionalText(interest, "Country", payer.mCountry);
    appendText(interest, "Type", interestType(aRow));
    appendText(interest, "Value", amountText(roundedCents(aRow.mGrossInterest)));

    // The form wants the foreign tax empty for a Slovenian payer: a Slovenian withholding is no
    // foreign tax.
    if (!isSlovenianPayer(aRow))
    {
        appendText(interest, "ForeignTax", amountText(roundedCents(aRow.mWithholding.mForeignTax)));
    }

    appendOptionalText(interest, "Country2", aRow.mSourceCountry);

    // A claim writes its treaty text, or `DA` when the user fills in the paragraph and article in
    // eDavki; a row without a claim has no element.
    if (aRow.mTreatyClaim)
    {
        if (aRow.mTreatyClaim->mStatement)
        {
            appendText(interest, "ReliefStatement", *aRow.mTreatyClaim->mStatement);
        }
        else
        {
            appendText(interest, "ReliefStatement", "DA");
        }
    }
}

} // namespace

XmlWriteResult ObrXmlWriter::write(const ReportMetadata& aMetadata,
                                   const OtherInterestReportObr& aReport) {
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
    auto envelope = beginDocument(document, kObrNamespace);

    // The form has no self-report in eDavki: it is always filed as an original.
    appendHeader(envelope, aMetadata, FilingType::Original);
    envelope.append_child("edp:Signatures");

    auto body = envelope.append_child("body");

    body.append_child("edp:bodyContent");

    auto obr = appendObrHeader(body, aMetadata);

    for (const auto& row : aReport.mRows)
    {
        appendInterest(obr, row);
    }

    result.mXml = saveDocument(document);

    return result;
}

} // namespace taxbroker
