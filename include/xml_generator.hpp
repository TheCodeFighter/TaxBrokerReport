#pragma once

#include <string>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>
#include <pugixml.hpp>

namespace edavki::doh_kdvp {

enum class InventoryListType {
    PLVP,
    PLVPSHORT,
    PLVPGB,
    PLVPGBSHORT,
    PLD,
    PLVPZOK
};

enum class GainType { A, B, C, D, E, F, G, H, I };

struct RowPurchase {
    std::optional<std::string> F1;  // datum pridobitve
    std::optional<GainType>    F2;  // način pridobitve
    std::optional<double>      F3;  // količina
    std::optional<double>      F4;  // nabavna vrednost/enoto
    std::optional<double>      F5;  // davek ded./dar.
    std::optional<double>      F11; // zmanjšana nabavna vrednost (samo polne verzije)
};

struct RowSale {
    std::optional<std::string> F6;  // datum odsvojitve
    std::optional<double>      F7;  // količina / % / izplačilo
    std::optional<double>      F9;  // vrednost ob odsvojitvi
    std::optional<bool>        F10; // pravilo 97.č ZDoh-2 (samo polne verzije)
};

struct InventoryRow {
    int ID{0};
    std::optional<RowPurchase> Purchase;
    std::optional<RowSale>     Sale;
    std::optional<double>      F8;  // zaloga (lahko negativna)
};

struct SecuritiesBase {
    std::optional<std::string> ISIN;
    std::optional<std::string> Code;
    std::string                Name;
    bool                       IsFond{false};
};

struct SecuritiesPLVP : SecuritiesBase {
    std::optional<std::string> Resolution;
    std::optional<std::string> ResolutionDate;
    std::vector<InventoryRow>  Rows;
};

struct Shares {  // PLD
    // TODO: extend later
    std::string               Name;
    std::vector<InventoryRow> Rows;
    // ... add foreign company, subsequent payments, etc.
};

struct KDVPItem {
    std::optional<int>         ItemID;
    InventoryListType          Type{InventoryListType::PLVP};
    std::optional<bool>        HasForeignTax;
    std::optional<double>      ForeignTax;
    std::optional<std::string> FTCountryID;
    std::optional<std::string> FTCountryName;

    // Only one of these is filled at a time
    std::optional<SecuritiesPLVP> Securities;
    std::optional<Shares>         SharesData;
    // add Short, WithContract, CapitalReduction when needed
};

struct DohKDVP_Data {
    int                               Year{2025};
    bool                              IsResident{true};
    std::optional<std::string>        TelephoneNumber;
    std::optional<std::string>        Email;
    std::vector<KDVPItem>             Items;
    // later: TaxRelief, TaxBaseDecrease, Attachments, etc.
};

class XmlGenerator {
public:
    /**
     * Main entry point – call this from main.cpp
     */
    static pugi::xml_document generate_envelope(const DohKDVP_Data& data);

    /**
     * Helper if you only need the <Doh_KDVP> part (for testing)
     */
    static pugi::xml_node generate_doh_kdvp(pugi::xml_node parent, const DohKDVP_Data& data);

private:
    static void append_header_and_signatures(pugi::xml_node envelope);
    static std::string gain_type_to_string(GainType t);
    static std::string inventory_type_to_string(InventoryListType t);
};

} // namespace edavki::doh_kdvp