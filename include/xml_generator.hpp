#pragma once

#include <string>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>
#include <pugixml.hpp>
#include <set>


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

enum class FormType {
    Original,
    SelfReport
};

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
    std::optional<double>      mF8;  // stock (can be negative) -> normally need to be 0 or even left out
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

// in future if needed
struct Shares {  // PLD
    std::string               mName;
    std::vector<InventoryRow> mRows;
    // ... add foreign company, subsequent payments
};

struct KDVPItem {
    std::optional<int>         mItemID;
    InventoryListType          mType{InventoryListType::PLVP};
    std::optional<bool>        mHasForeignTax;
    std::optional<double>      mForeignTaxAmount;
    std::optional<std::string> mForeignCountryID;
    std::optional<std::string> mForeignCountryName;

    // Only one of these is filled at a time
    std::optional<SecuritiesPLVP> mSecurities;
    std::optional<Shares>         mSharesData;
    // add Short, WithContract, CapitalReduction when needed
};

struct DivPayer {
    std::optional<std::string> mTaxID;
    std::string                mIsin;
    std::optional<std::string> mName;
    std::optional<std::string> mAddress;
    std::optional<std::string> mCountryCode;
};

struct DivItem {
    std::string                mDate;
    DivPayer                   mPayer;
    std::string                mType{"1"}; // 1 -regular dividend, 2 - non physical person, 3 - loan gains distribution (look on furs instructions at "vrsta dividende")
    double                     mGrossIncome;
    std::optional<double>      mWithholdingTax;
    std::optional<std::string> mSourceCountryCode;
    std::optional<bool>        mForeignTaxPaid{true};   // for now we will assume its always true, maybe this will be gui user input // TODO: check
};

struct FormData {
    FormType                   mDocID{FormType::Original};
    int                        mYear{};
    bool                       mIsResident{true};
    std::optional<std::string> mTelephoneNumber;
    std::optional<std::string> mEmail;
};

struct DohKDVP_Data : public FormData {
    std::vector<KDVPItem> mItems;

    DohKDVP_Data() = default;

    explicit DohKDVP_Data(const FormData& fd)
        : FormData(fd) {}
};

struct DohDiv_Data : public FormData {
    std::vector<DivItem> mItems;

    DohDiv_Data() = default;

    explicit DohDiv_Data(const FormData& fd)
        : FormData(fd) {}
};

struct TaxPayer {
    std::string mTaxNumber;         // 8 digits, mandatory
    std::string mType{"FO"};     // FO -> phisical person
    std::optional<std::string> mTaxPayerName{std::nullopt};
    std::optional<std::string> mAddress1{std::nullopt};
    std::optional<std::string> mAddress2{std::nullopt};
    std::optional<std::string> mCity{std::nullopt};
    std::optional<std::string> mPostNumber{std::nullopt};
    std::optional<std::string> mPostName{std::nullopt};
    std::optional<std::string> mBirthDate{std::nullopt};    
    bool        mResident{true};
};

struct FormHeaderData {
    FormType mDocWorkflowID{FormType::Original};
    TaxPayer      mTaxPayer;
};

struct GainTransaction {
    std::string mDate;
    std::string mType;  // "Trading Buy" or "Trading Sell"
    std::string mIsin;
    std::string mIsinName;
    double      mQuantity{0.0};
    double      mUnitPrice{0.0};
};

// TODO: add in readme warning that country, address of payer and proof of tax witholding must be provided by user
struct DivTransaction {
    std::string mDate;
    std::string mIsin;
    std::string mIsinName;
    std::string mCountryName;
    double      mGrossIncome{0.0};
    double      mWitholdTax{0.0};
};
struct IncomeTransactions {
    std::map<std::string, std::vector<DivTransaction>> mDivTransactions;
};

struct Transactions {
    std::map<std::string, std::vector<GainTransaction>> mGains;
    IncomeTransactions mIncome;
};

class XmlGenerator {
public:
    // JSON parsing
    static void parse_json(Transactions& aTransactions, std::set<TransactionType> aTypes, const nlohmann::json& aJsonData);
    
    // XML generation
    // KDVP
    static DohKDVP_Data prepare_kdvp_data(std::map<std::string, std::vector<GainTransaction>>& aTransactions, FormData& aFormData);
    pugi::xml_document generate_doh_kdvp_xml(const DohKDVP_Data& data, const TaxPayer& tp);
    
    // Div
    static DohDiv_Data prepare_div_data(std::map<std::string, std::vector<DivTransaction>>& aTransactions, FormData& aFormData);
    pugi::xml_document generate_doh_div_xml(const DohDiv_Data& data, const TaxPayer& tp);

    
private:
    void append_edp_header(pugi::xml_node envelope, FormHeaderData headerData);
    static void append_edp_taxpayer(pugi::xml_node header, const TaxPayer& tp);
    
    static pugi::xml_node generate_doh_kdvp(pugi::xml_node parent, const DohKDVP_Data& data);
    static pugi::xml_node generate_doh_div(pugi::xml_node parent, const DohDiv_Data& data);
    
    static std::string gain_type_to_string(GainType t);
    static std::string inventory_type_to_string(InventoryListType t);
};
