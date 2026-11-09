#include "xml/kdvp_xml_writer.hpp"
#include "utils/numeric_util.hpp"
#include "xml_common.hpp"

#include <pugixml.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace taxbroker {

using namespace xml_common;

namespace {

constexpr std::string_view kKdvpNamespace =
    "http://edavki.durs.si/Documents/Schemas/Doh_KDVP_9.xsd";

// Fixed values of the form: codes that the schema lists and the zeros that the form always writes.
// Inventory list of securities
constexpr std::string_view kInventoryListTypeSecurities = "PLVP";
constexpr std::string_view kPurchaseTypeBought = "B";
// F5 is the inheritance or gift tax paid at the purchase: a known zero for a bought security
// (F2 B). The FURS instructions fill column 5 on every purchase row, and the legacy output that
// eDavki accepted wrote it the same way.
constexpr std::string_view kNoInheritanceOrGiftTax = "0.0000";
// This writer lists only ordinary securities (PLVP), so the counts of the other kinds are zero.
constexpr std::string_view kZeroCount = "0";
constexpr std::string_view kPeriodStartSuffix = "-01-01";
constexpr std::string_view kPeriodEndSuffix = "-12-31";

constexpr ReportType kReport = ReportType::CapitalGains;
constexpr MetadataFields kMetadataFields{
    .mYear = "Year", .mPhone = "TelephoneNumber", .mEmail = "Email"};
constexpr std::size_t kMaximumIsinLength = 12;
constexpr std::size_t kMaximumNameLength = 100;

// Units and unit prices share the scale, so one formatter writes F3, F4, F7, F8 and F9.
static_assert(UNITS_SCALE == UNIT_PRICE_SCALE);
constexpr std::int64_t kFixedPointScale = UNITS_SCALE;
constexpr std::size_t kMinimumDecimals = 4;

/// The exact stored value with four to eight decimals: trailing zeros are removed only down to
/// four decimals, and nothing is rounded. `200'000'000` is `2.0000`, `12'345'678` is
/// `0.12345678`. Only for values that are not negative.
std::string decimalText(std::int64_t aValue) {
    return formatFixedPoint(aValue, kFixedPointScale, kMinimumDecimals);
}

/// The ISIN and the name of a list for the context of its errors, each only when it is valid XML
/// text: a broken value must not travel on into logs or JSON.
ErrorIdentity listIdentity(const KdvpInventoryList& aList) {
    ErrorIdentity identity;

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

void checkList(std::vector<ProcessingDiagnostic>& aDiagnostics, const KdvpInventoryList& aList) {
    const auto identity = listIdentity(aList);
    const auto isinLength = xmlTextLength(aList.mIsin);

    if (!isinLength || *isinLength == 0 || *isinLength > kMaximumIsinLength)
    {
        addOutOfRange(aDiagnostics, kReport, "ISIN", identity);
    }

    const auto nameLength = xmlTextLength(aList.mName);

    if (!nameLength || *nameLength > kMaximumNameLength)
    {
        addOutOfRange(aDiagnostics, kReport, "Name", identity);
    }

    const auto checkNotNegative = [&aDiagnostics, &identity](std::string_view aField,
                                                             std::int64_t aValue) {
        if (aValue < 0)
        {
            addOutOfRange(aDiagnostics, kReport, aField, identity);
        }
    };

    const auto checkDate = [&aDiagnostics, &identity](std::string_view aField, const Date& aDate) {
        if (!dateText(aDate))
        {
            addOutOfRange(aDiagnostics, kReport, aField, identity);
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

    checkMetadata(result.mDiagnostics, kReport, aMetadata, kMetadataFields);

    for (const auto& list : aReport.mLists)
    {
        checkList(result.mDiagnostics, list);
    }

    if (!result.mDiagnostics.empty())
    {
        return result;
    }

    pugi::xml_document document;
    auto envelope = beginDocument(document, kKdvpNamespace);

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

    result.mXml = saveDocument(document);

    return result;
}

} // namespace taxbroker
