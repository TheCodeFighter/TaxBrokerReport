#include "xml_generator.hpp"
#include <iostream>

namespace edavki::doh_kdvp {

namespace {
    constexpr auto NS_DOH = "http://edavki.durs.si/Documents/Schemas/Doh_KDVP_9.xsd";
    constexpr auto NS_EDP = "http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd";
}

std::string XmlGenerator::gain_type_to_string(GainType t) {
    switch (t) { case GainType::A: return "A"; case GainType::B: return "B"; case GainType::C: return "C";
                 case GainType::D: return "D"; case GainType::E: return "E"; case GainType::F: return "F";
                 case GainType::G: return "G"; case GainType::H: return "H"; case GainType::I: return "I"; }
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

void XmlGenerator::append_header_and_signatures(pugi::xml_node envelope) {
    // In real app you get these from your signing service
    envelope.append_child("edp:Header");
    envelope.append_child("edp:Signatures");
}

pugi::xml_node XmlGenerator::generate_doh_kdvp(pugi::xml_node parent, const DohKDVP_Data& data) {
    auto doh = parent.append_child("Doh_KDVP");

    auto kdvp_wrapper = doh.append_child("KDVP");
    auto kdvp = kdvp_wrapper.append_child("KDVP");

    kdvp.append_child("Year").text().set(data.Year);
    kdvp.append_child("IsResident").text().set(data.IsResident ? "true" : "false");
    if (data.TelephoneNumber) kdvp.append_child("TelephoneNumber").text().set(data.TelephoneNumber->c_str());
    if (data.Email)           kdvp.append_child("Email").text().set(data.Email->c_str());

    // SecurityCount – auto-count PLVP items
    int sec_count = 0;
    for (const auto& item : data.Items)
        if (item.Type == InventoryListType::PLVP) ++sec_count;
    kdvp.append_child("SecurityCount").text().set(sec_count);

    // All KDVPItem entries
    for (const auto& item : data.Items) {
        auto item_node = doh.append_child("KDVPItem");

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
                    const auto& pu = *row.Purchase;
                    if (pu.F1)  p.append_child("F1").text().set(pu.F1->c_str());
                    if (pu.F2)  p.append_child("F2").text().set(gain_type_to_string(*pu.F2).c_str());
                    if (pu.F3)  p.append_child("F3").text().set(pu.F3.value());
                    if (pu.F4)  p.append_child("F4").text().set(pu.F4.value());
                    if (pu.F5)  p.append_child("F5").text().set(pu.F5.value());
                    if (pu.F11) p.append_child("F11").text().set(pu.F11.value());
                }

                if (row.Sale) {
                    auto s = row_node.append_child("Sale");
                    const auto& sa = *row.Sale;
                    if (sa.F6) s.append_child("F6").text().set(sa.F6->c_str());
                    if (sa.F7) s.append_child("F7").text().set(sa.F7.value());
                    if (sa.F9) s.append_child("F9").text().set(sa.F9.value());
                    if (sa.F10) s.append_child("F10").text().set(*sa.F10 ? "true" : "false");
                }

                if (row.F8) row_node.append_child("F8").text().set(*row.F8);
            }
        }

        // TODO: Shares (PLD), Short versions, etc.
    }

    return doh;
}

pugi::xml_document XmlGenerator::generate_envelope(const DohKDVP_Data& data) {
    pugi::xml_document doc;
    auto decl = doc.prepend_child(pugi::node_declaration);
    decl.append_attribute("version") = "1.0";
    decl.append_attribute("encoding") = "UTF-8";

    auto envelope = doc.append_child("Envelope");
    envelope.append_attribute("xmlns") = NS_DOH;
    envelope.append_attribute("xmlns:edp") = NS_EDP;

    append_header_and_signatures(envelope);

    auto body = envelope.append_child("body");
    body.append_child("edp:bodyContent");  // required empty element
    generate_doh_kdvp(body, data);

    return doc;
}

} // namespace edavki::doh_kdvp