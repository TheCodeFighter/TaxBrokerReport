#include <iostream>

#include "xml_generator.hpp"
#include "util_xml.hpp"

namespace {
    constexpr auto NS_DOH_KDVP = "http://edavki.durs.si/Documents/Schemas/Doh_KDVP_9.xsd";
    constexpr auto NS_DOH_DIV = "http://edavki.durs.si/Documents/Schemas/Doh_Div_3.xsd";
    constexpr auto NS_EDP = "http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd";
}

std::string XmlGenerator::gain_type_to_string(GainType t) {
    switch (t) {case GainType::A: return "A"; case GainType::B: return "B"; case GainType::C: return "C";
                case GainType::D: return "D"; case GainType::E: return "E"; case GainType::F: return "F";
                case GainType::G: return "G";
    }
    return "H";
}

std::string XmlGenerator::inventory_type_to_string(InventoryListType t) {
    switch (t) {
        case InventoryListType::PLVP:        return "PLVP";
        case InventoryListType::PLVPSHORT:   return "PLVPSHORT";
        case InventoryListType::PLVPGB:      return "PLVPGB";
        case InventoryListType::PLVPGBSHORT: return "PLVPGBSHORT";
        case InventoryListType::PLD:         return "PLD";
        case InventoryListType::PLVPZOK:     return "PLVPZOK";
    }
    return "PLVP";
}

pugi::xml_node XmlGenerator::generate_doh_div(pugi::xml_node parent, const DohDiv_Data& data) {
    auto dohDiv = parent.append_child("Doh_Div");

    dohDiv.append_child("Period").text().set(std::to_string(data.mYear).c_str());
    if (data.mEmail) dohDiv.append_child("EmailAddress").text().set(data.mEmail->c_str());
    if (data.mTelephoneNumber) dohDiv.append_child("PhoneNumber").text().set(data.mTelephoneNumber->c_str());
    dohDiv.append_child("IsResident").text().set(data.mIsResident ? "true" : "false");

    for (const auto& item : data.mItems) {
        auto dividend = parent.append_child("Dividend");

        dividend.append_child("Date").text().set(item.mDate.c_str());
        dividend.append_child("PayerIdentificationNumber").text().set(item.mPayer.mIsin.c_str());
        if (item.mPayer.mName) dividend.append_child("PayerName").text().set(item.mPayer.mName->c_str());
        if (item.mPayer.mAddress) dividend.append_child("PayerAddress").text().set(item.mPayer.mAddress->c_str());
        if (item.mPayer.mCountryCode) dividend.append_child("PayerCountry").text().set(item.mPayer.mCountryCode->c_str());
        
        dividend.append_child("Type").text().set(item.mType.c_str());
        dividend.append_child("Value").text().set(to_xml_decimal(item.mGrossIncome, 2).c_str());   // no ptr for no optional
        if (item.mWithholdingTax) dividend.append_child("ForeignTax").text().set(to_xml_decimal(item.mWithholdingTax.value(), 2).c_str());
        if (item.mPayer.mCountryCode) dividend.append_child("SourceCountry").text().set(item.mPayer.mCountryCode->c_str());
        if (item.mForeignTaxPaid) dividend.append_child("ReliefStatement").text().set(item.mForeignTaxPaid ? "true" : "false");
    }

    return dohDiv;
}

pugi::xml_node XmlGenerator::generate_doh_kdvp(pugi::xml_node parent, const DohKDVP_Data& data) {
    auto kdvp = parent.append_child("KDVP");

    kdvp.append_child("DocumentWorkflowID").text().set(form_type_to_string_code(data.mDocID).c_str());
    kdvp.append_child("DocumentWorkflowName").text().set(form_type_to_string(data.mDocID).c_str());

    kdvp.append_child("Year").text().set(std::to_string(data.mYear).c_str());

    std::string pStart = std::to_string(data.mYear) + "-01-01";
    std::string pEnd = std::to_string(data.mYear) + "-12-31";
    kdvp.append_child("PeriodStart").text().set(pStart.c_str());
    kdvp.append_child("PeriodEnd").text().set(pEnd.c_str());

    kdvp.append_child("IsResident").text().set(data.mIsResident ? "true" : "false");
    
    if (data.mTelephoneNumber) kdvp.append_child("TelephoneNumber").text().set(data.mTelephoneNumber->c_str());

    // SecurityCount – auto-count PLVP items
    int sec_count = 0;
    for (const auto& item : data.mItems)
        if (item.mType == InventoryListType::PLVP) ++sec_count;
    kdvp.append_child("SecurityCount").text().set(sec_count);
    
    // TODO: for now 0, probably we don't need it.
    kdvp.append_child("SecurityShortCount").text().set(0);
    kdvp.append_child("SecurityWithContractCount").text().set(0);
    kdvp.append_child("SecurityWithContractShortCount").text().set(0);
    kdvp.append_child("ShareCount").text().set(0);

    if (data.mEmail) kdvp.append_child("Email").text().set(data.mEmail->c_str());

    // All KDVPItem entries
    for (const auto& item : data.mItems) {
        auto item_node = parent.append_child("KDVPItem");

        if (item.mItemID) item_node.append_child("ItemID").text().set(*item.mItemID);
        item_node.append_child("InventoryListType").text().set(inventory_type_to_string(item.mType).c_str());

        if (item.mHasForeignTax && *item.mHasForeignTax) {
            item_node.append_child("HasForeignTax").text().set("true");
            if (item.mForeignTaxAmount)   item_node.append_child("ForeignTax").text().set(*item.mForeignTaxAmount);
            if (item.mForeignCountryID)  item_node.append_child("FTCountryID").text().set(item.mForeignCountryID->c_str());
        }

        if (item.mSecurities) {
            const auto& sec = *item.mSecurities;
            auto sec_node = item_node.append_child("Securities");

            if (sec.mISIN)  sec_node.append_child("ISIN").text().set(sec.mISIN->c_str());
            if (sec.mCode)  sec_node.append_child("Code").text().set(sec.mCode->c_str());
            sec_node.append_child("Name").text().set(sec.mName.c_str());
            sec_node.append_child("IsFond").text().set(sec.mIsFond ? "true" : "false");
            if (sec.mResolution)     sec_node.append_child("Resolution").text().set(sec.mResolution->c_str());
            if (sec.mResolutionDate) sec_node.append_child("ResolutionDate").text().set(sec.mResolutionDate->c_str());

            for (const auto& row : sec.mRows) {
                auto row_node = sec_node.append_child("Row");
                row_node.append_child("ID").text().set(row.ID);

                if (row.mPurchase) {
                    auto p = row_node.append_child("Purchase");
                    const auto& pu = *row.mPurchase; // TODO: correct raw pointer usage
                    if (pu.mF1)  p.append_child("F1").text().set(pu.mF1->c_str());
                    if (pu.mF2)  p.append_child("F2").text().set(gain_type_to_string(*pu.mF2).c_str());
                    if (pu.mF3)  p.append_child("F3").text().set(to_xml_decimal(pu.mF3.value(), 8).c_str());
                    if (pu.mF4)  p.append_child("F4").text().set(to_xml_decimal(pu.mF4.value(), 8).c_str());
                    if (pu.mF5)  p.append_child("F5").text().set(to_xml_decimal(pu.mF5.value(), 4).c_str());
                    if (pu.mF11) p.append_child("F11").text().set(to_xml_decimal(pu.mF11.value(), 8).c_str());
                }

                if (row.mSale) {
                    auto s = row_node.append_child("Sale");
                    const auto& sa = *row.mSale; // TODO: correct raw pointer usage
                    if (sa.mF6) s.append_child("F6").text().set(sa.mF6->c_str());
                    if (sa.mF7) s.append_child("F7").text().set(to_xml_decimal(sa.mF7.value(), 8).c_str());
                    if (sa.mF9) s.append_child("F9").text().set(to_xml_decimal(sa.mF9.value(), 8).c_str());
                    if (sa.mF10) s.append_child("F10").text().set(*sa.mF10 ? "true" : "false");
                }

                if (row.mF8) row_node.append_child("F8").text().set(to_xml_decimal(*row.mF8, 8).c_str());
            }
        }

        // TODO: Shares (PLD), Short versions, etc.
    }

    return kdvp;
}

void XmlGenerator::append_edp_taxpayer(pugi::xml_node header, const TaxPayer& tp)
{
    auto taxpayer = header.append_child("edp:taxpayer");

    taxpayer.append_child("edp:taxNumber")
            .text()
            .set(tp.mTaxNumber.c_str());

    taxpayer.append_child("edp:taxpayerType")
            .text()
            .set(tp.mType.c_str());

    if (tp.mTaxPayerName) {
        taxpayer.append_child("edp:name")
                .text()
                .set(tp.mTaxPayerName->c_str());
    }

    if (tp.mAddress1) {
        taxpayer.append_child("edp:address1")
                .text()
                .set(tp.mAddress1->c_str());
    }

    if (tp.mAddress2) {
        taxpayer.append_child("edp:address2")
            .text()
            .set(tp.mAddress2->c_str());
    }

    if (tp.mCity) {
        taxpayer.append_child("edp:city")
            .text()
            .set(tp.mCity->c_str());
    }

    if (tp.mPostNumber) {
        taxpayer.append_child("edp:postNumber")
                .text()
                .set(tp.mPostNumber->c_str());
    }

    if (tp.mPostName) {
        taxpayer.append_child("edp:postName")
                .text()
                .set(tp.mPostName->c_str());
    }

    if (tp.mBirthDate) {
        taxpayer.append_child("edp:birthDate")
                .text()
                .set(tp.mBirthDate->c_str());
    }

    taxpayer.append_child("edp:resident")
            .text()
            .set(tp.mResident ? "true" : "false");
}

void XmlGenerator::append_edp_header(pugi::xml_node envelope, FormHeaderData headerData)
{
    auto header = envelope.append_child("edp:Header");

    this->append_edp_taxpayer(header, headerData.mTaxPayer);

    auto workflow = header.append_child("edp:Workflow");
    workflow.append_child("edp:DocumentWorkflowID").text().set(form_type_to_string_code(headerData.mDocWorkflowID).c_str());
    workflow.append_child("edp:DocumentWorkflowName").text().set(form_type_to_string(headerData.mDocWorkflowID).c_str());
}

pugi::xml_document XmlGenerator::generate_doh_kdvp_xml(const DohKDVP_Data& data, const TaxPayer& tp) {
    pugi::xml_document doc;
    auto decl = doc.prepend_child(pugi::node_declaration);
    decl.append_attribute("version") = "1.0";
    decl.append_attribute("encoding") = "UTF-8";

    auto envelope = doc.append_child("Envelope");
    envelope.append_attribute("xmlns").set_value(NS_DOH_KDVP);
    envelope.append_attribute("xmlns:edp").set_value(NS_EDP);

    FormHeaderData headerData {
        .mDocWorkflowID = data.mDocID,
        .mTaxPayer = tp
    };

    this->append_edp_header(envelope, headerData);

    envelope.append_child("edp:Signatures");

    auto body = envelope.append_child("body");
    body.append_child("edp:bodyContent");
    auto doh = body.append_child("Doh_KDVP");

    generate_doh_kdvp(doh, data);

    return doc;
}

pugi::xml_document XmlGenerator::generate_doh_div_xml(const DohDiv_Data& data, const TaxPayer& tp) {
    pugi::xml_document doc;
    auto decl = doc.prepend_child(pugi::node_declaration);
    decl.append_attribute("version") = "1.0";
    decl.append_attribute("encoding") = "UTF-8";

    auto envelope = doc.append_child("Envelope");
    envelope.append_attribute("xmlns").set_value(NS_DOH_DIV);
    envelope.append_attribute("xmlns:edp").set_value(NS_EDP);

    FormHeaderData headerData {
        .mDocWorkflowID = data.mDocID,
        .mTaxPayer = tp
    };

    this->append_edp_header(envelope, headerData);

    envelope.append_child("edp:Signatures");

    auto body = envelope.append_child("body");

    generate_doh_div(body, data);

    return doc;
}

void XmlGenerator::parse_json(Transactions& aTransactions, std::set<TransactionType> aTypes, const nlohmann::json& aJsonData) {
    // Extract gains_and_losses_section
    auto& gains_section = aJsonData["gains_and_losses_section"];

    if (!gains_section.is_array()) {
        throw std::runtime_error("Invalid JSON: 'gains_and_losses_section' must be an array");
    }

    auto& income_section = aJsonData["income_section"];

    if (!income_section.is_array()) {
        throw std::runtime_error("Invalid JSON: 'income_section' must be an array");
    }

    parse_gains_section(gains_section, aTypes, aTransactions.mGains);
    parse_income_section(income_section, aTransactions.mDividends);
}

DohKDVP_Data XmlGenerator::prepare_kdvp_data(std::map<std::string, std::vector<GainTransaction>>& aTransactions, FormData& aFormData) {
    DohKDVP_Data data(aFormData);

    int item_id = 1;
    for (auto& [mIsin, txs] : aTransactions) {
        // Sort transactions by date
        std::sort(txs.begin(), txs.end(), [](const GainTransaction& a, const GainTransaction& b) {
            return a.mDate < b.mDate;
        });

        KDVPItem item;
        item.mItemID = item_id++;
        item.mType = InventoryListType::PLVP;  // Use full PLVP for detailed rows

        item.mSecurities = SecuritiesPLVP{};
        item.mSecurities->mISIN = mIsin;
        // item.Securities->mCode = mIsin;  // Reuse as code if needed -> ticker, if we have isin we can skip this
        item.mSecurities->mName = txs[0].mIsinName;       // we take firs element name, as we group by isin and all elements have same isin

        // Build rows with running stock (mF8)
        double running_quantity = 0.0;
        int row_id = 0;
        for (const auto& t : txs) {
            InventoryRow row;
            row.ID = row_id++;

            if (t.mType == "Trading Buy") {
                row.mPurchase = RowPurchase{};
                row.mPurchase->mF1 = t.mDate;
                row.mPurchase->mF2 = GainType::A;

                row.mPurchase->mF3 = t.mQuantity;
                row.mPurchase->mF4 = t.mUnitPrice;
                row.mPurchase->mF5 = 0.0;  // Assume no inheritance tax
                // mF11 if needed

                running_quantity = running_quantity + t.mQuantity;
            } else if (t.mType == "Trading Sell") {
                row.mSale = RowSale{};
                row.mSale->mF6 = t.mDate;
                row.mSale->mF7 = t.mQuantity;
                row.mSale->mF9 = t.mUnitPrice;
                row.mSale->mF10 = true;  // losses substract gains if not bought until 30 days from loss sell pass 

                running_quantity = running_quantity - t.mQuantity;
            } else {
                continue;  // Skip unknown types
            }

            row.mF8 = running_quantity;
            item.mSecurities->mRows.push_back(row);
        }

        // Only add if there are rows
        if (!item.mSecurities->mRows.empty()) {
            data.mItems.push_back(item);
        }
    }

    return data;
}

DohDiv_Data XmlGenerator::prepare_div_data(std::map<std::string, std::vector<DivTransaction>>& aTransactions, FormData& aFormData) {
    DohDiv_Data data(aFormData);

    for (auto& [mIsin, txs] : aTransactions) {
        // Sort transactions by date
        std::sort(txs.begin(), txs.end(), [](const DivTransaction& a, const DivTransaction& b) {
            return a.mDate < b.mDate;
        });

        DivItem item;

        for (const auto& tx : txs) {
            item.mDate = tx.mDate;

            item.mPayer.mIsin = mIsin;
            item.mPayer.mName = tx.mIsinName;
            // TODO: make country dictionary: from county name to country code
            // std::string country_code = getCountryCode(txs[0].mCountryName);
            // if (country_code) item.mPayer.mCountryCode = country_code;
            // if (country_code) item.mPayer.mSourceCountryCode = country_code; // TODO: after function is made move it out of payer part

            item.mGrossIncome = tx.mGrossIncome;
            item.mWithholdingTax = tx.mWitholdTax;

            // mType remains default for physical person dividend ("1")
            // mForeignTaxPaid remains default true

            data.mItems.push_back(item);
        }
    }

    return data;
}
