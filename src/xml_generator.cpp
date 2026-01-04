#include <iostream>

#include "xml_generator.hpp"
#include "util_xml.hpp"

namespace {
    constexpr auto NS_DOH = "http://edavki.durs.si/Documents/Schemas/Doh_KDVP_9.xsd";
    constexpr auto NS_EDP = "http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd";
}

std::string XmlGenerator::gain_type_to_string(GainType t) {
    switch (t) {case GainType::A: return "A"; case GainType::B: return "B"; case GainType::C: return "C";
                case GainType::D: return "D"; case GainType::E: return "E"; case GainType::F: return "F";
                case GainType::G: return "G";
    }
    return "H";
}

std::string XmlGenerator::getDocWorkflowIDString(DocWorkflowID id) {
    if (id == DocWorkflowID::Original)
        return "O";

    return "I";
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

pugi::xml_node XmlGenerator::generate_doh_kdvp(pugi::xml_node parent, const DohKDVP_Data& data) {
    // auto doc = parent.append_child("Doh_KDVP");


    auto kdvp = parent.append_child("KDVP");

    // 1. Workflow podatki (NUJNO NA ZAČETKU)
    kdvp.append_child("DocumentWorkflowID").text().set(XmlGenerator::getDocWorkflowIDString(data.docID).c_str());
    kdvp.append_child("DocumentWorkflowName").text().set("Doh_KDVP");

    kdvp.append_child("Year").text().set(std::to_string(data.Year).c_str());

    std::string pStart = std::to_string(data.Year) + "-01-01";
    std::string pEnd = std::to_string(data.Year) + "-12-31";
    kdvp.append_child("PeriodStart").text().set(pStart.c_str());
    kdvp.append_child("PeriodEnd").text().set(pEnd.c_str());

    kdvp.append_child("IsResident").text().set(data.IsResident ? "true" : "false");
    if (data.TelephoneNumber) kdvp.append_child("TelephoneNumber").text().set(data.TelephoneNumber->c_str());
    if (data.Email)           kdvp.append_child("Email").text().set(data.Email->c_str());

    // SecurityCount – auto-count PLVP items
    int sec_count = 0;
    for (const auto& item : data.Items)
        if (item.Type == InventoryListType::PLVP) ++sec_count;
    kdvp.append_child("SecurityCount").text().set(sec_count);
    
    // TODO: for now 0, probably we don't need it.
    kdvp.append_child("SecurityShortCount").text().set(0);
    kdvp.append_child("SecurityWithContractCount").text().set(0);
    kdvp.append_child("SecurityWithContractShortCount").text().set(0);
    kdvp.append_child("ShareCount").text().set(0);

    // All KDVPItem entries
    for (const auto& item : data.Items) {
        auto item_node = parent.append_child("KDVPItem");

        if (item.ItemID) item_node.append_child("ItemID").text().set(*item.ItemID);
        item_node.append_child("InventoryListType").text().set(inventory_type_to_string(item.Type).c_str());

        if (item.HasForeignTax && *item.HasForeignTax) {
            item_node.append_child("HasForeignTax").text().set("true");
            if (item.ForeignTax)   item_node.append_child("ForeignTax").text().set(*item.ForeignTax);
            if (item.FTCountryID)  item_node.append_child("FTCountryID").text().set(item.FTCountryID->c_str());
        }

        if (item.Securities) {
            const auto& sec = *item.Securities;
            auto sec_node = item_node.append_child("Securities");

            if (sec.ISIN)  sec_node.append_child("ISIN").text().set(sec.ISIN->c_str());
            if (sec.Code)  sec_node.append_child("Code").text().set(sec.Code->c_str());
            sec_node.append_child("Name").text().set(sec.Name.c_str());
            sec_node.append_child("IsFond").text().set(sec.IsFond ? "true" : "false");
            if (sec.Resolution)     sec_node.append_child("Resolution").text().set(sec.Resolution->c_str());
            if (sec.ResolutionDate) sec_node.append_child("ResolutionDate").text().set(sec.ResolutionDate->c_str());

            for (const auto& row : sec.Rows) {
                auto row_node = sec_node.append_child("Row");
                row_node.append_child("ID").text().set(row.ID);

                if (row.Purchase) {
                    auto p = row_node.append_child("Purchase");
                    const auto& pu = *row.Purchase; // TODO: correct raw pointer usage
                    if (pu.F1)  p.append_child("F1").text().set(pu.F1->c_str());
                    if (pu.F2)  p.append_child("F2").text().set(gain_type_to_string(*pu.F2).c_str());
                    if (pu.F3)  p.append_child("F3").text().set(to_xml_decimal(pu.F3.value(), 8).c_str());
                    if (pu.F4)  p.append_child("F4").text().set(to_xml_decimal(pu.F4.value(), 8).c_str());
                    if (pu.F5)  p.append_child("F5").text().set(to_xml_decimal(pu.F5.value(), 4).c_str());
                    if (pu.F11) p.append_child("F11").text().set(to_xml_decimal(pu.F11.value(), 8).c_str());
                }

                if (row.Sale) {
                    auto s = row_node.append_child("Sale");
                    const auto& sa = *row.Sale; // TODO: correct raw pointer usage
                    if (sa.F6) s.append_child("F6").text().set(sa.F6->c_str());
                    if (sa.F7) s.append_child("F7").text().set(to_xml_decimal(sa.F7.value(), 8).c_str());
                    if (sa.F9) s.append_child("F9").text().set(to_xml_decimal(sa.F9.value(), 8).c_str());
                    if (sa.F10) s.append_child("F10").text().set(*sa.F10 ? "true" : "false");
                }

                if (row.F8) row_node.append_child("F8").text().set(to_xml_decimal(*row.F8, 8).c_str());
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
            .set(tp.taxNumber.c_str());

    taxpayer.append_child("edp:resident")
            .text()
            .set(tp.resident ? "true" : "false");
}

void XmlGenerator::append_edp_header(pugi::xml_node envelope, const TaxPayer& tp, const DocWorkflowID docWorkflowID)
{
    auto header = envelope.append_child("edp:Header");

    this->append_edp_taxpayer(header, tp);

    auto workflow = header.append_child("edp:Workflow");
    workflow.append_child("edp:DocumentWorkflowID").text().set(XmlGenerator::getDocWorkflowIDString(docWorkflowID).c_str());
    workflow.append_child("edp:DocumentWorkflowName").text().set("Doh_KDVP");   // TODO: different for different 
}

pugi::xml_document XmlGenerator::generate_envelope(const DohKDVP_Data& data, const TaxPayer& tp) {
    pugi::xml_document doc;
    auto decl = doc.prepend_child(pugi::node_declaration);
    decl.append_attribute("version") = "1.0";
    decl.append_attribute("encoding") = "UTF-8";

    auto envelope = doc.append_child("Envelope");
    envelope.append_attribute("xmlns").set_value(NS_DOH);
    envelope.append_attribute("xmlns:edp").set_value(NS_EDP);

    this->append_edp_header(envelope, tp, data.docID);

    envelope.append_child("edp:Signatures");

    auto body = envelope.append_child("body");
    body.append_child("edp:bodyContent");
    auto doh = body.append_child("Doh_KDVP");

    generate_doh_kdvp(doh, data);

    return doc;
}

void XmlGenerator::parse_json(std::map<std::string, std::vector<Transaction>>& aTransactions, TransactionType aType, const nlohmann::json& aJsonData) {
    // Extract gains_and_losses_section
    (void) aType;
    auto& gains_section = aJsonData["gains_and_losses_section"];

    if (!gains_section.is_array()) {
        throw std::runtime_error("Invalid JSON: 'gains_and_losses_section' must be an array");
    }

    for (const auto& entry : gains_section) {
        if (!entry.contains("transactions") || !entry["transactions"].is_array()) continue;

        // Go just with desired type
        if (string_to_asset_type(entry["asset_type"]) != aType) continue;

        for (const auto& tx : entry["transactions"]) {
            if (!tx.contains("isin") || !tx.contains("transaction_date") ||
                !tx.contains("transaction_type") || !tx.contains("amount_of_units")) {
                continue;  // Skip invalid transactions
            }

            std::string isin_str = tx["isin"];
            std::string isin_code;
            std::string name;
            parse_isin(isin_str, isin_code, name);

            Transaction t;
            t.date = parse_date(tx["transaction_date"].get<std::string>());
            t.type = tx["transaction_type"].get<std::string>();
            t.quantity = tx["amount_of_units"].get<double>();
            t.isin = isin_code;
            t.isin_name = name;

            // Get unit_price: prefer "unit_price", fallback to "market_value" / quantity
            if (tx.contains("unit_price")) {
                t.unit_price = tx["unit_price"].get<double>();
            } else if (tx.contains("market_value")) {
                double market_value = tx["market_value"].get<double>();
                t.unit_price = (t.quantity != 0.0) ? market_value / t.quantity : 0.0;
            } else {
                continue;  // Skip if no price info
            }

            aTransactions[isin_code].push_back(t);
        }
    }
}

DohKDVP_Data XmlGenerator::prepare_kdvp_data(std::map<std::string, std::vector<Transaction>>& aTransactions) {
    DohKDVP_Data data;
    data.Year = 2025;  // From JSON current date, but hardcoded for test
    data.IsResident = true;  // Assume
    data.docID = DocWorkflowID::Original;

    int item_id = 1;
    for (auto& [isin, txs] : aTransactions) {
        // Sort transactions by date
        std::sort(txs.begin(), txs.end(), [](const Transaction& a, const Transaction& b) {
            return a.date < b.date;
        });

        KDVPItem item;
        item.ItemID = item_id++;
        item.Type = InventoryListType::PLVP;  // Use full PLVP for detailed rows

        item.Securities = SecuritiesPLVP{};
        item.Securities->ISIN = isin;
        // item.Securities->Code = isin;  // Reuse as code if needed -> ticker, if we have isin we can skip this
        item.Securities->Name = txs[0].isin_name;       
        // item.Securities->IsFond = (name.find("ETF") != std::string::npos || name.find("Fond") != std::string::npos);  // Heuristic

        // Build rows with running stock (F8)
        double running_quantity = 0.0;
        int row_id = 0;
        for (const auto& t : txs) {
            InventoryRow row;
            row.ID = row_id++;

            if (t.type == "Trading Buy") {
                row.Purchase = RowPurchase{};
                row.Purchase->F1 = t.date;
                row.Purchase->F2 = GainType::A;

                row.Purchase->F3 = t.quantity;
                row.Purchase->F4 = t.unit_price;
                row.Purchase->F5 = 0.0;  // Assume no inheritance tax
                // F11 if needed

                running_quantity = running_quantity + t.quantity;
            } else if (t.type == "Trading Sell") {
                row.Sale = RowSale{};
                row.Sale->F6 = t.date;
                row.Sale->F7 = t.quantity;
                row.Sale->F9 = t.unit_price;
                row.Sale->F10 = true;  // losses substract gains if not bought until 30 days from loss sell pass 

                running_quantity = running_quantity - t.quantity;
            } else {
                continue;  // Skip unknown types
            }

            row.F8 = running_quantity;
            item.Securities->Rows.push_back(row);
        }

        // Only add if there are rows
        if (!item.Securities->Rows.empty()) {
            data.Items.push_back(item);
        }
    }

    return data;
}
