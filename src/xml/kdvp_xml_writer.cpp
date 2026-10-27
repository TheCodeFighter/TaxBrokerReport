#include "xml/kdvp_xml_writer.hpp"

#include <pugixml.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace taxbroker {
namespace {

constexpr std::string_view kKdvpNamespace =
    "http://edavki.durs.si/Documents/Schemas/Doh_KDVP_9.xsd";
constexpr std::string_view kCommonNamespace =
    "http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd";

// Fixed values of the form: codes that the schema lists and the zeros that the form always writes.
constexpr std::string_view kTaxpayerTypeIndividual = "FO";
// Inventory list of securities
constexpr std::string_view kInventoryListTypeSecurities = "PLVP";
constexpr std::string_view kPurchaseTypeBought = "B";
constexpr std::string_view kWorkflowCodeOriginal = "O";
constexpr std::string_view kWorkflowCodeSelfReport = "I";
constexpr std::string_view kWorkflowNameOriginal = "Original";
constexpr std::string_view kWorkflowNameSelfReport = "SamoPrijava";
// F5 is the inheritance or gift tax paid at the purchase: a known zero for a bought security
// (F2 B). The FURS instructions fill column 5 on every purchase row, and the legacy output that
// eDavki accepted wrote it the same way.
constexpr std::string_view kNoInheritanceOrGiftTax = "0.0000";
// This writer lists only ordinary securities (PLVP), so the counts of the other kinds are zero.
constexpr std::string_view kZeroCount = "0";
constexpr std::string_view kPeriodStartSuffix = "-01-01";
constexpr std::string_view kPeriodEndSuffix = "-12-31";

constexpr std::string_view kXmlVersion = "1.0";
constexpr std::string_view kXmlEncoding = "UTF-8";
constexpr std::string_view kXmlIndent = "  ";

constexpr std::size_t kTaxNumberDigits = 8;
constexpr int kMinimumYear = 1000;
constexpr int kMaximumYear = 9999;
// `xs:date` holds a year from 1 to 9999 in four digits.
constexpr int kMinimumDateYear = 1;
constexpr std::size_t kMaximumIsinLength = 12;
constexpr std::size_t kMaximumNameLength = 100;
constexpr std::size_t kMaximumPostNumberLength = 12;

// Units and unit prices share the scale, so one formatter writes F3, F4, F7, F8 and F9.
static_assert(UNITS_SCALE == UNIT_PRICE_SCALE);
constexpr std::int64_t kFixedPointScale = UNITS_SCALE;
constexpr std::size_t kStoredDecimals = 8;
constexpr std::size_t kMinimumDecimals = 4;

// Limits of the characters that XML accepts.
constexpr std::uint32_t kFirstPrintableCodePoint = 0x20U; // below it are the control characters
constexpr std::uint32_t kFirstSurrogate = 0xD800U;
constexpr std::uint32_t kLastSurrogate = 0xDFFFU;
constexpr std::uint32_t kNonCharacterFffe = 0xFFFEU;
constexpr std::uint32_t kNonCharacterFfff = 0xFFFFU;
constexpr std::uint32_t kLastCodePoint = 0x10FFFFU;

/// The number of characters of a text that XML accepts: valid UTF-8 without control characters
/// (below U+0020) and without U+FFFE and U+FFFF. Returns nothing for any other text.
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

/// `YYYY-MM-DD`, or nothing for a year outside 1 to 9999, which `xs:date` cannot hold in four
/// digits (year 0000 does not exist in XML).
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

/// The exact stored value with four to eight decimals: trailing zeros are removed only down to
/// four decimals, and nothing is rounded. `200'000'000` is `2.0000`, `12'345'678` is
/// `0.12345678`. Only for values that are not negative.
std::string decimalText(std::int64_t aValue) {
    auto fraction = std::to_string(aValue % kFixedPointScale);
    fraction.insert(0, kStoredDecimals - fraction.size(), '0');

    while (fraction.size() > kMinimumDecimals && fraction.back() == '0')
    {
        fraction.pop_back();
    }

    return std::to_string(aValue / kFixedPointScale) + '.' + fraction;
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

/// The name of the workflow code.
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

/// The ISIN and the name of a list for the context of its errors, each only when it is valid XML
/// text: a broken value must not travel on into logs or JSON.
struct ListIdentity {
    std::optional<Isin> mIsin;
    std::optional<std::string> mName;
};

ListIdentity listIdentity(const KdvpInventoryList& aList) {
    ListIdentity identity;

    if (xmlTextLength(aList.mIsin))
    {
        identity.mIsin = aList.mIsin;
    }

    if (!aList.mName.empty() && xmlTextLength(aList.mName))
    {
        identity.mName = aList.mName;
    }

    return identity;
}

/// One error for a field that the form does not accept. The value is never part of the message.
void addOutOfRange(std::vector<ProcessingDiagnostic>& aDiagnostics,
                   std::string_view aField,
                   const ListIdentity& aIdentity = {}) {
    auto message = makeDiagnosticMessage(MessageKey::XmlValueOutOfRange,
                                         {textParam("field", std::string{aField})});

    aDiagnostics.push_back({
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::XmlValueOutOfRange,
        .mContext =
            {
                .mStage = PipelineStage::XmlGeneration,
                .mAffectedReports = {ReportType::CapitalGains},
                .mField = std::string{aField},
                .mInstrumentName = aIdentity.mName,
                .mIsin = aIdentity.mIsin,
            },
        .mMessage = std::move(message.mEnglish),
        .mMessageKey = message.mKey,
        .mMessageParams = std::move(message.mParams),
    });
}

/// Optional text: nothing to check when it is absent.
void checkText(std::vector<ProcessingDiagnostic>& aDiagnostics,
               std::string_view aField,
               const std::optional<std::string>& aText,
               std::optional<std::size_t> aMaximumLength = std::nullopt) {
    if (!aText)
    {
        return;
    }

    const auto length = xmlTextLength(*aText);

    if (!length || (aMaximumLength && *length > *aMaximumLength))
    {
        addOutOfRange(aDiagnostics, aField);
    }
}

void checkMetadata(std::vector<ProcessingDiagnostic>& aDiagnostics,
                   const ReportMetadata& aMetadata) {
    const auto& taxpayer = aMetadata.mTaxpayer;

    if (!isTaxNumber(taxpayer.mTaxNumber))
    {
        addOutOfRange(aDiagnostics, "taxNumber");
    }

    checkText(aDiagnostics, "name", taxpayer.mName);
    checkText(aDiagnostics, "address1", taxpayer.mAddress);
    checkText(aDiagnostics, "city", taxpayer.mCity);
    checkText(aDiagnostics, "postNumber", taxpayer.mPostCode, kMaximumPostNumberLength);
    checkText(aDiagnostics, "postName", taxpayer.mPostName);

    if (taxpayer.mBirthDate && !dateText(*taxpayer.mBirthDate))
    {
        addOutOfRange(aDiagnostics, "birthDate");
    }

    if (aMetadata.mReportingYear < kMinimumYear || aMetadata.mReportingYear > kMaximumYear)
    {
        addOutOfRange(aDiagnostics, "Year");
    }

    checkText(aDiagnostics, "TelephoneNumber", taxpayer.mPhoneNumber);
    checkText(aDiagnostics, "Email", taxpayer.mEmail);
}

void checkList(std::vector<ProcessingDiagnostic>& aDiagnostics, const KdvpInventoryList& aList) {
    const auto identity = listIdentity(aList);
    const auto isinLength = xmlTextLength(aList.mIsin);

    if (!isinLength || *isinLength == 0 || *isinLength > kMaximumIsinLength)
    {
        addOutOfRange(aDiagnostics, "ISIN", identity);
    }

    const auto nameLength = xmlTextLength(aList.mName);

    if (!nameLength || *nameLength > kMaximumNameLength)
    {
        addOutOfRange(aDiagnostics, "Name", identity);
    }

    const auto checkNotNegative = [&aDiagnostics, &identity](std::string_view aField,
                                                             std::int64_t aValue) {
        if (aValue < 0)
        {
            addOutOfRange(aDiagnostics, aField, identity);
        }
    };

    const auto checkDate = [&aDiagnostics, &identity](std::string_view aField, const Date& aDate) {
        if (!dateText(aDate))
        {
            addOutOfRange(aDiagnostics, aField, identity);
        }
    };

    for (const auto& row : aList.mRows)
    {
        const auto checkEntry = [&checkNotNegative, &checkDate](const auto& aEntry) {
            using Entry = std::decay_t<decltype(aEntry)>;

            if constexpr (std::is_same_v<Entry, KdvpPurchaseRow>)
            {
                checkDate("F1", aEntry.mDate);
                checkNotNegative("F3", aEntry.mUnits);
                checkNotNegative("F4", aEntry.mUnitPrice);
            }
            else if constexpr (std::is_same_v<Entry, KdvpSaleRow>)
            {
                checkDate("F6", aEntry.mDate);
                checkNotNegative("F7", aEntry.mUnits);
                checkNotNegative("F9", aEntry.mUnitPrice);
            }
            else
            {
                static_assert(std::is_same_v<Entry, void>, "Missing check for this KDVP row.");
            }
        };

        std::visit(checkEntry, row.mEntry);

        // The schema allows a negative F8, but only the developer export (#188) may write one.
        checkNotNegative("F8", row.mStock);
    }
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

void appendKdvp(pugi::xml_node aDohKdvp,
                const ReportMetadata& aMetadata,
                const CapitalGainsReportKDVP& aReport) {
    const auto& taxpayer = aMetadata.mTaxpayer;
    const auto year = std::to_string(aMetadata.mReportingYear);
    auto kdvp = aDohKdvp.append_child("KDVP");

    appendText(kdvp, "DocumentWorkflowID", workflowCode(aMetadata.mFilingType));
    appendText(kdvp, "DocumentWorkflowName", workflowName(aMetadata.mFilingType));
    appendText(kdvp, "Year", year);
    appendText(kdvp, "PeriodStart", year + std::string{kPeriodStartSuffix});
    appendText(kdvp, "PeriodEnd", year + std::string{kPeriodEndSuffix});
    appendBoolean(kdvp, "IsResident", taxpayer.mIsResident);
    appendOptionalText(kdvp, "TelephoneNumber", taxpayer.mPhoneNumber);
    appendText(kdvp, "SecurityCount", std::to_string(aReport.mLists.size()));
    appendText(kdvp, "SecurityShortCount", kZeroCount);
    appendText(kdvp, "SecurityWithContractCount", kZeroCount);
    appendText(kdvp, "SecurityWithContractShortCount", kZeroCount);
    appendText(kdvp, "ShareCount", kZeroCount);
    appendOptionalText(kdvp, "Email", taxpayer.mEmail);
}

void appendRow(pugi::xml_node aSecurities, std::size_t aRowId, const KdvpRow& aRow) {
    auto row = aSecurities.append_child("Row");

    appendText(row, "ID", std::to_string(aRowId));

    const auto appendEntry = [&row](const auto& aEntry) {
        using Entry = std::decay_t<decltype(aEntry)>;

        if constexpr (std::is_same_v<Entry, KdvpPurchaseRow>)
        {
            auto purchase = row.append_child("Purchase");

            appendText(purchase, "F1", dateText(aEntry.mDate).value());
            appendText(purchase, "F2", kPurchaseTypeBought);
            appendText(purchase, "F3", decimalText(aEntry.mUnits));
            appendText(purchase, "F4", decimalText(aEntry.mUnitPrice));
            appendText(purchase, "F5", kNoInheritanceOrGiftTax);
        }
        else if constexpr (std::is_same_v<Entry, KdvpSaleRow>)
        {
            auto sale = row.append_child("Sale");

            appendText(sale, "F6", dateText(aEntry.mDate).value());
            appendText(sale, "F7", decimalText(aEntry.mUnits));
            appendText(sale, "F9", decimalText(aEntry.mUnitPrice));
            appendBoolean(sale, "F10", aEntry.mLossRule == KdvpLossRule::NotBlocked);
        }
        else
        {
            static_assert(std::is_same_v<Entry, void>, "Missing XML for this KDVP row.");
        }
    };

    std::visit(appendEntry, aRow.mEntry);

    appendText(row, "F8", decimalText(aRow.mStock));
}

void appendList(pugi::xml_node aDohKdvp, std::size_t aItemId, const KdvpInventoryList& aList) {
    auto item = aDohKdvp.append_child("KDVPItem");

    appendText(item, "ItemID", std::to_string(aItemId));
    appendText(item, "InventoryListType", kInventoryListTypeSecurities);

    auto securities = item.append_child("Securities");

    appendText(securities, "ISIN", aList.mIsin);

    if (!aList.mName.empty())
    {
        appendText(securities, "Name", aList.mName);
    }

    appendBoolean(securities, "IsFond", aList.mIsFund);

    for (std::size_t rowId = 0; rowId < aList.mRows.size(); ++rowId)
    {
        appendRow(securities, rowId, aList.mRows[rowId]);
    }
}

} // namespace

XmlWriteResult KdvpXmlWriter::write(const ReportMetadata& aMetadata,
                                    const CapitalGainsReportKDVP& aReport) {
    XmlWriteResult result;

    checkMetadata(result.mDiagnostics, aMetadata);

    for (const auto& list : aReport.mLists)
    {
        checkList(result.mDiagnostics, list);
    }

    if (!result.mDiagnostics.empty())
    {
        return result;
    }

    pugi::xml_document document;
    auto declaration = document.append_child(pugi::node_declaration);

    declaration.append_attribute("version").set_value(std::string{kXmlVersion}.c_str());
    declaration.append_attribute("encoding").set_value(std::string{kXmlEncoding}.c_str());

    auto envelope = document.append_child("Envelope");

    envelope.append_attribute("xmlns").set_value(std::string{kKdvpNamespace}.c_str());
    envelope.append_attribute("xmlns:edp").set_value(std::string{kCommonNamespace}.c_str());

    appendHeader(envelope, aMetadata);
    envelope.append_child("edp:Signatures");

    auto body = envelope.append_child("body");

    body.append_child("edp:bodyContent");

    auto dohKdvp = body.append_child("Doh_KDVP");

    appendKdvp(dohKdvp, aMetadata, aReport);

    // ItemID counts from 1 and the row ID from 0 in each list, as in the files eDavki accepted.
    for (std::size_t index = 0; index < aReport.mLists.size(); ++index)
    {
        appendList(dohKdvp, index + 1, aReport.mLists[index]);
    }

    std::ostringstream xml;
    document.save(xml, std::string{kXmlIndent}.c_str(), pugi::format_default, pugi::encoding_utf8);
    result.mXml = xml.str();

    return result;
}

} // namespace taxbroker
