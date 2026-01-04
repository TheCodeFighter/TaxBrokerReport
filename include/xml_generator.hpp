#pragma once

#include <string>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>
#include <pugixml.hpp>


enum class InventoryListType {
    PLVP,
    PLVPSHORT,
    PLVPGB,
    PLVPGBSHORT,
    PLD,
    PLVPZOK
};

enum class GainType { 
    A, // investment of capital -> for stock exchange this is the one
    B, // buy
    C, // gaining capital company with own resources
    D, // "no data"
    E, // change of capital
    F, // inheritance
    G, // gift
};

enum class TransactionType {
    Crypto,
    Equities,
    Funds,
    Bonds,
    None
};

enum class DocWorkflowID {
    Original,
    SelfReport
};

struct RowPurchase {
    std::optional<std::string> F1;  // date of acquisition
    std::optional<GainType>    F2;  // method of acquisition
    std::optional<double>      F3;  // quantity
    std::optional<double>      F4;  // purchase value per unit
    std::optional<double>      F5;  // inheritance/gift tax
    std::optional<double>      F11; // reduced purchase value (full versions only)
};

struct RowSale {
    std::optional<std::string> F6;  // date of disposal
    std::optional<double>      F7;  // quantity / % / payment
    std::optional<double>      F9;  // value at disposal
    std::optional<bool>        F10; // rule 97.č ZDoh-2 (full versions only) -> losses substract gains if not bought until 30 days from loss sell pass
    // TODO: make if F10 can be true
};

struct InventoryRow {
    int ID{0};
    std::optional<RowPurchase> Purchase;
    std::optional<RowSale>     Sale;
    std::optional<double>      F8;  // stock (can be negative) -> normaly need to be 0 or even left out
};

struct SecuritiesBase {
    std::optional<std::string> ISIN;
    std::optional<std::string> Code;    // Ticker
    std::string                Name;
    bool                       IsFond{false};
};

struct SecuritiesPLVP : SecuritiesBase {
    std::optional<std::string> Resolution;
    std::optional<std::string> ResolutionDate;
    std::vector<InventoryRow>  Rows;
};

struct Shares {  // PLD
    // TODO: extend later -> probably not needed for simple form
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
    DocWorkflowID                     docID{DocWorkflowID::Original};
    int                               Year{2025};
    bool                              IsResident{true};
    std::optional<std::string>        TelephoneNumber;
    std::optional<std::string>        Email;
    std::vector<KDVPItem>             Items;
    // TODO: TaxRelief, TaxBaseDecrease, Attachments, not sure if needed
};

// TODO: extend with: name, address, country...
struct TaxPayer {
    std::string taxNumber;   // 8 digits, mandatory
    bool        resident{true};
};

struct Transaction {
    std::string date;
    std::string type;  // "Trading Buy" or "Trading Sell"
    std::string isin;
    std::string isin_name;
    double quantity = 0.0;
    double unit_price = 0.0;
};

class XmlGenerator {
public:
    // Main methodes
    static void parse_json(std::map<std::string, std::vector<Transaction>>& aTransactions, TransactionType aType, const nlohmann::json& aJsonData);
    pugi::xml_document generate_envelope(const DohKDVP_Data& data, const TaxPayer& tp);
    static DohKDVP_Data prepare_kdvp_data(std::map<std::string, std::vector<Transaction>>& aTransactions);

    // Helper if you only need the <Doh_KDVP> part (for testing)
    static pugi::xml_node generate_doh_kdvp(pugi::xml_node parent, const DohKDVP_Data& data);

private:
    static std::string gain_type_to_string(GainType t);
    static std::string inventory_type_to_string(InventoryListType t);
    void append_edp_header(pugi::xml_node envelope, const TaxPayer& tp, const DocWorkflowID docWorkflowID);
    static void append_edp_taxpayer(pugi::xml_node header, const TaxPayer& tp);
    static std::string getDocWorkflowIDString(DocWorkflowID id);
};
