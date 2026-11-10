#include "xml_common.hpp"
#include "utils/numeric_util.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace taxbroker::xml_common {
namespace {

constexpr std::string_view kCommonNamespace =
    "http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd";

constexpr std::string_view kTaxpayerTypeIndividual = "FO";
constexpr std::string_view kWorkflowCodeOriginal = "O";
constexpr std::string_view kWorkflowCodeSelfReport = "I";
constexpr std::string_view kWorkflowNameOriginal = "Original";
constexpr std::string_view kWorkflowNameSelfReport = "SamoPrijava";

constexpr std::string_view kXmlVersion = "1.0";
constexpr std::string_view kXmlEncoding = "UTF-8";
constexpr std::string_view kXmlIndent = "  ";

constexpr std::size_t kTaxNumberDigits = 8;
constexpr int kMaximumYear = 9999;
// `xs:date` holds a year from 1 to 9999 in four digits.
constexpr int kMinimumDateYear = 1;
constexpr std::size_t kMaximumPostNumberLength = 12;
constexpr std::size_t kCountryLength = 2;

// `Money` has four decimals and the form two, so a cent is 100 stored steps.
constexpr Money kStepsPerCent = 100;
constexpr Money kHalfCent = 50;
constexpr std::int64_t kCentsPerUnit = 100;
constexpr std::size_t kAmountDecimals = 2;

// Limits of the characters that XML accepts.
constexpr std::uint32_t kFirstPrintableCodePoint = 0x20U; // below it are the control characters
constexpr std::uint32_t kFirstSurrogate = 0xD800U;
constexpr std::uint32_t kLastSurrogate = 0xDFFFU;
constexpr std::uint32_t kNonCharacterFffe = 0xFFFEU;
constexpr std::uint32_t kNonCharacterFfff = 0xFFFFU;
constexpr std::uint32_t kLastCodePoint = 0x10FFFFU;

} // namespace

std::optional<std::size_t> xmlTextLength(std::string_view aText) {
    std::size_t length{};
    std::size_t position{};

    while (position < aText.size())
    {
        const auto lead = static_cast<unsigned char>(aText[position]);
        std::size_t byteCount{};
        std::uint32_t codePoint{};
        std::uint32_t smallestCodePoint{};

        if (lead < 0x80U)
        {
            byteCount = 1;
            codePoint = lead;
        }
        else if ((lead & 0xE0U) == 0xC0U)
        {
            byteCount = 2;
            codePoint = lead & 0x1FU;
            smallestCodePoint = 0x80U;
        }
        else if ((lead & 0xF0U) == 0xE0U)
        {
            byteCount = 3;
            codePoint = lead & 0x0FU;
            smallestCodePoint = 0x800U;
        }
        else if ((lead & 0xF8U) == 0xF0U)
        {
            byteCount = 4;
            codePoint = lead & 0x07U;
            smallestCodePoint = 0x10000U;
        }
        else
        {
            return std::nullopt;
        }

        if (aText.size() - position < byteCount)
        {
            return std::nullopt;
        }

        for (std::size_t next = 1; next < byteCount; ++next)
        {
            const auto continuation = static_cast<unsigned char>(aText[position + next]);

            if ((continuation & 0xC0U) != 0x80U)
            {
                return std::nullopt;
            }

            codePoint = (codePoint << 6U) | (continuation & 0x3FU);
        }

        // A code point written with more bytes than it needs, a UTF-16 surrogate and a value
        // above U+10FFFF are not valid UTF-8.
        if (codePoint < smallestCodePoint ||
            (codePoint >= kFirstSurrogate && codePoint <= kLastSurrogate) ||
            codePoint > kLastCodePoint)
        {
            return std::nullopt;
        }

        if (codePoint < kFirstPrintableCodePoint || codePoint == kNonCharacterFffe ||
            codePoint == kNonCharacterFfff)
        {
            return std::nullopt;
        }

        ++length;
        position += byteCount;
    }

    return length;
}

bool isCountryCode(std::string_view aText) {
    return aText.size() == kCountryLength &&
           std::all_of(aText.begin(), aText.end(), [](char aCharacter) {
               return aCharacter >= 'A' && aCharacter <= 'Z';
           });
}

std::int64_t roundedCents(Money aValue) {
    // Divides before it adds, so the largest `Money` value cannot overflow.
    std::int64_t cents = aValue / kStepsPerCent;

    if (aValue % kStepsPerCent >= kHalfCent)
    {
        ++cents;
    }

    return cents;
}

std::string amountText(std::int64_t aCents) {
    return formatFixedPoint(aCents, kCentsPerUnit, kAmountDecimals);
}

bool isTaxNumber(std::string_view aText) {
    if (aText.size() != kTaxNumberDigits)
    {
        return false;
    }

    bool hasNonZeroDigit = false;

    for (const char character : aText)
    {
        if (character < '0' || character > '9')
        {
            return false;
        }

        hasNonZeroDigit = hasNonZeroDigit || character != '0';
    }

    // The schema type is a positive integer, so 00000000 is not a tax number.
    return hasNonZeroDigit;
}

std::optional<std::string> dateText(const Date& aDate) {
    const std::chrono::year_month_day date{std::chrono::sys_days{aDate}};
    const int year = static_cast<int>(date.year());

    if (year < kMinimumDateYear || year > kMaximumYear)
    {
        return std::nullopt;
    }

    std::ostringstream text;
    text << std::setfill('0') << std::setw(4) << year << '-' << std::setw(2)
         << static_cast<unsigned>(date.month()) << '-' << std::setw(2)
         << static_cast<unsigned>(date.day());

    return text.str();
}

std::string_view workflowCode(FilingType aFilingType) {
    switch (aFilingType)
    {
    case FilingType::Original:
        return kWorkflowCodeOriginal;
    case FilingType::SelfReport:
        return kWorkflowCodeSelfReport;
    }

    throw std::logic_error{"Unknown filing type"};
}

std::string_view workflowName(FilingType aFilingType) {
    switch (aFilingType)
    {
    case FilingType::Original:
        return kWorkflowNameOriginal;
    case FilingType::SelfReport:
        return kWorkflowNameSelfReport;
    }

    throw std::logic_error{"Unknown filing type"};
}

void addOutOfRange(std::vector<ProcessingDiagnostic>& aDiagnostics,
                   ReportType aReport,
                   std::string_view aField,
                   const ErrorIdentity& aIdentity) {
    auto message = makeDiagnosticMessage(MessageKey::XmlValueOutOfRange,
                                         {textParam("field", std::string{aField})});

    aDiagnostics.push_back({
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::XmlValueOutOfRange,
        .mContext =
            {
                .mStage = PipelineStage::XmlGeneration,
                .mAffectedReports = {aReport},
                .mField = std::string{aField},
                .mInstrumentName = aIdentity.mName,
                .mIsin = aIdentity.mIsin,
            },
        .mMessage = std::move(message.mEnglish),
        .mMessageKey = message.mKey,
        .mMessageParams = std::move(message.mParams),
    });
}

void checkText(std::vector<ProcessingDiagnostic>& aDiagnostics,
               ReportType aReport,
               std::string_view aField,
               const std::optional<std::string>& aText,
               std::optional<std::size_t> aMaximumLength) {
    if (!aText)
    {
        return;
    }

    const auto length = xmlTextLength(*aText);

    if (!length || (aMaximumLength && *length > *aMaximumLength))
    {
        addOutOfRange(aDiagnostics, aReport, aField);
    }
}

void checkMetadata(std::vector<ProcessingDiagnostic>& aDiagnostics,
                   ReportType aReport,
                   const ReportMetadata& aMetadata,
                   const MetadataFields& aFields) {
    const auto& taxpayer = aMetadata.mTaxpayer;

    if (!isTaxNumber(taxpayer.mTaxNumber))
    {
        addOutOfRange(aDiagnostics, aReport, "taxNumber");
    }

    checkText(aDiagnostics, aReport, "name", taxpayer.mName);
    checkText(aDiagnostics, aReport, "address1", taxpayer.mAddress);
    checkText(aDiagnostics, aReport, "city", taxpayer.mCity);
    checkText(aDiagnostics, aReport, "postNumber", taxpayer.mPostCode, kMaximumPostNumberLength);
    checkText(aDiagnostics, aReport, "postName", taxpayer.mPostName);

    if (taxpayer.mBirthDate && !dateText(*taxpayer.mBirthDate))
    {
        addOutOfRange(aDiagnostics, aReport, "birthDate");
    }

    if (aMetadata.mReportingYear < aFields.mMinimumYear || aMetadata.mReportingYear > kMaximumYear)
    {
        addOutOfRange(aDiagnostics, aReport, aFields.mYear);
    }

    checkText(aDiagnostics, aReport, aFields.mPhone, taxpayer.mPhoneNumber);
    checkText(aDiagnostics, aReport, aFields.mEmail, taxpayer.mEmail);
}

void appendText(pugi::xml_node aParent, std::string_view aName, std::string_view aText) {
    aParent.append_child(std::string{aName}.c_str()).text().set(std::string{aText}.c_str());
}

void appendBoolean(pugi::xml_node aParent, std::string_view aName, bool aValue) {
    appendText(aParent, aName, aValue ? "true" : "false");
}

void appendOptionalText(pugi::xml_node aParent,
                        std::string_view aName,
                        const std::optional<std::string>& aText) {
    if (aText)
    {
        appendText(aParent, aName, *aText);
    }
}

pugi::xml_node beginDocument(pugi::xml_document& aDocument, std::string_view aNamespace) {
    auto declaration = aDocument.append_child(pugi::node_declaration);

    declaration.append_attribute("version").set_value(std::string{kXmlVersion}.c_str());
    declaration.append_attribute("encoding").set_value(std::string{kXmlEncoding}.c_str());

    auto envelope = aDocument.append_child("Envelope");

    envelope.append_attribute("xmlns").set_value(std::string{aNamespace}.c_str());
    envelope.append_attribute("xmlns:edp").set_value(std::string{kCommonNamespace}.c_str());

    return envelope;
}

void appendHeader(pugi::xml_node aEnvelope, const ReportMetadata& aMetadata) {
    const auto& taxpayer = aMetadata.mTaxpayer;
    auto header = aEnvelope.append_child("edp:Header");
    auto taxpayerNode = header.append_child("edp:taxpayer");

    appendText(taxpayerNode, "edp:taxNumber", taxpayer.mTaxNumber);
    appendText(taxpayerNode, "edp:taxpayerType", kTaxpayerTypeIndividual);
    appendOptionalText(taxpayerNode, "edp:name", taxpayer.mName);
    appendOptionalText(taxpayerNode, "edp:address1", taxpayer.mAddress);
    appendOptionalText(taxpayerNode, "edp:city", taxpayer.mCity);
    appendOptionalText(taxpayerNode, "edp:postNumber", taxpayer.mPostCode);
    appendOptionalText(taxpayerNode, "edp:postName", taxpayer.mPostName);

    if (taxpayer.mBirthDate)
    {
        appendText(taxpayerNode, "edp:birthDate", dateText(*taxpayer.mBirthDate).value());
    }

    appendBoolean(taxpayerNode, "edp:resident", taxpayer.mIsResident);

    auto workflow = header.append_child("edp:Workflow");

    appendText(workflow, "edp:DocumentWorkflowID", workflowCode(aMetadata.mFilingType));
    appendText(workflow, "edp:DocumentWorkflowName", workflowName(aMetadata.mFilingType));
}

std::string saveDocument(const pugi::xml_document& aDocument) {
    std::ostringstream xml;

    aDocument.save(xml, std::string{kXmlIndent}.c_str(), pugi::format_default, pugi::encoding_utf8);

    return xml.str();
}

} // namespace taxbroker::xml_common
