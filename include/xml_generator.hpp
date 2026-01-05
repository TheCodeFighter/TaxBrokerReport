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

// TODO: make all structs members refactor to mMember
struct RowPurchase {
    std::optional<std::string> mF1;  // date of acquisition
    std::optional<GainType>    mF2;  // method of acquisition
    std::optional<double>      mF3;  // quantity
    std::optional<double>      mF4;  // purchase value per unit
    std::optional<double>      mF5;  // inheritance/gift tax
    std::optional<double>      mF11; // reduced purchase value (full versions only)
};

struct RowSale {
    std::optional<std::string> mF6;  // date of disposal
    std::optional<double>      mF7;  // quantity / % / payment
    std::optional<double>      mF9;  // value at disposal
    std::optional<bool>        mF10; // rule 97.č ZDoh-2 (full versions only) -> losses substract gains if not bought until 30 days from loss sell pass
    // TODO: make if mF10 can be true
};

struct InventoryRow {
    int ID{0};
    std::optional<RowPurchase> mPurchase;
    std::optional<RowSale>     mSale;
    std::optional<double>      mF8;  // stock (can be negative) -> normaly need to be 0 or even left out
};

struct SecuritiesBase {
    std::optional<std::string> mISIN;
    std::optional<std::string> mCode;    // Ticker
    std::string                mName;
    bool                       mIsFond{false};
};

struct SecuritiesPLVP : SecuritiesBase {
    std::optional<std::string> mResolution;
    std::optional<std::string> mResolutionDate;
    std::vector<InventoryRow>  mRows;
};

struct Shares {  // PLD
    // TODO: extend later -> probably not needed for simple form
    std::string               mName;
    std::vector<InventoryRow> mRows;
    // ... add foreign company, subsequent payments, etc.
};

struct KDVPItem {
    std::optional<int>         mItemID;
    InventoryListType          mType{InventoryListType::PLVP};
    std::optional<bool>        mHasForeignTax;
    std::optional<double>      mForeignTax;
    std::optional<std::string> mFTCountryID;
    std::optional<std::string> mFTCountryName;

    // Only one of these is filled at a time
    std::optional<SecuritiesPLVP> mSecurities;
    std::optional<Shares>         mSharesData;
    // add Short, WithContract, CapitalReduction when needed
};

struct FormData {
    DocWorkflowID                     mDocID{DocWorkflowID::Original};
    int                               mYear{};
    bool                              mIsResident{true};
    std::optional<std::string>        mTelephoneNumber;
    std::optional<std::string>        mEmail;
};

struct DohKDVP_Data : public FormData {
    std::vector<KDVPItem> mItems;

    DohKDVP_Data() = default;

    explicit DohKDVP_Data(const FormData& fd)
        : FormData(fd) {}
};

// TODO: extend with: name, address, country...
struct TaxPayer {
    std::string mTaxNumber;   // 8 digits, mandatory
    bool        mResident{true};
};

struct Transaction {
    std::string mDate;
    std::string mType;  // "Trading Buy" or "Trading Sell"
    std::string mIsin;
    std::string mIsinName;
    double mQuantity = 0.0;
    double mUnitPrice = 0.0;
};

// TODO: make arguments to aArg on all places
class XmlGenerator {
public:
    // Main methodes
    static void parse_json(std::map<std::string, std::vector<Transaction>>& aTransactions, TransactionType aType, const nlohmann::json& aJsonData);
    pugi::xml_document generate_envelope(const DohKDVP_Data& data, const TaxPayer& tp);
    static DohKDVP_Data prepare_kdvp_data(std::map<std::string, std::vector<Transaction>>& aTransactions, FormData& aFormData);

    // Helper if you only need the <Doh_KDVP> part (for testing)
    static pugi::xml_node generate_doh_kdvp(pugi::xml_node parent, const DohKDVP_Data& data);

private:
    static std::string gain_type_to_string(GainType t);
    static std::string inventory_type_to_string(InventoryListType t);
    void append_edp_header(pugi::xml_node envelope, const TaxPayer& tp, const DocWorkflowID docWorkflowID);
    static void append_edp_taxpayer(pugi::xml_node header, const TaxPayer& tp);
    static std::string getDocWorkflowIDString(DocWorkflowID id);
};
