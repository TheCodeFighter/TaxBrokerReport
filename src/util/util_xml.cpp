#include "util_xml.hpp"
#include <algorithm>

static void parse_div_section(const nlohmann::json& div_section,  std::map<std::string, std::vector<DivTransaction>>& aTransactions, std::string country_name);
static void parse_interests_section(const nlohmann::json& interests_section,  std::map<std::string, std::vector<DhoTransaction>>& aTransactions);

// Helper function to parse date from DD.MM.YYYY to YYYY-MM-DD
std::string parse_date(const std::string& date_str) {
    if (date_str.empty()) return "";
    std::tm tm = {};
    std::istringstream ss(date_str);
    ss.imbue(std::locale("C"));
    ss >> std::get_time(&tm, "%d.%m.%Y");
    if (ss.fail()) {
        throw std::runtime_error("Failed to parse date: " + date_str);
    }
    std::ostringstream oss;
    oss.imbue(std::locale("C"));
    oss << std::put_time(&tm, "%Y-%m-%d");
    return oss.str();
}

// Helper to calculate days between two dates in YYYY-MM-DD format
int days_between(const std::string& from, const std::string& to)
{
    using namespace std::chrono;

    auto parse = [](const std::string& s) -> sys_days {
        int y, m, d;
        if (std::sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) != 3)
            throw std::invalid_argument("Invalid date format");

        year_month_day ymd{
            year{y},
            month{static_cast<unsigned>(m)},
            day{static_cast<unsigned>(d)}
        };

        if (!ymd.ok())
            throw std::invalid_argument("Invalid calendar date");

        return sys_days{ymd};
    };

    return (parse(to) - parse(from)).count();
}

// Helper to extract ISIN code and name from "ISIN - Name" format
void parse_isin(const std::string& isin_str, std::string& code, std::string& name) {
    size_t dash_pos = isin_str.find(" - ");
    if (dash_pos != std::string::npos) {
        code = isin_str.substr(0, dash_pos);
        name = isin_str.substr(dash_pos + 3);
    } else {
        code = isin_str;
        name = "Unknown";
    }
}

// Helper to get string from enum
TransactionType string_to_asset_type(std::string mType) {
    if (mType == "Crypto Currency") return TransactionType::Crypto;
    if (mType == "Equities") return TransactionType::Equities;
    if (mType == "Funds") return TransactionType::Funds;
    if (mType == "Bonds") return TransactionType::Bonds;

    return TransactionType::None;
}

// Helper to get string from FormType enum
std::string form_type_to_string(FormType t) {
    if (t == FormType::Original) return "Original";
    if (t == FormType::SelfReport) return "SelfReport";
    return "Unknown";
}

// Helper to get string code from FormType enum
std::string form_type_to_string_code(FormType t) {
    if (t == FormType::Original) return "O";
    if (t == FormType::SelfReport) return "S";
    return "Unknown";
}

std::string to_xml_decimal(double value, int precision) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << value;
    return oss.str();
}

void parse_gains_section(const nlohmann::json& gains_section, std::set<TransactionType> aTypes, std::map<std::string, std::vector<GainTransaction>>& aTransactions) {
    for (const auto& entry : gains_section) {
        if (!entry.contains("transactions") || !entry["transactions"].is_array()) continue;

        // Go just with desired types
        if (std::find(aTypes.begin(), aTypes.end(), string_to_asset_type(entry["asset_type"])) == aTypes.end()) continue;

        for (const auto& tx : entry["transactions"]) {
            if (!tx.contains("isin") || !tx.contains("transaction_date") ||
                !tx.contains("transaction_type") || !tx.contains("amount_of_units")) {
                continue;  // Skip invalid transactions
            }

            std::string isin_str = tx["isin"];
            std::string isin_code;
            std::string name;
            parse_isin(isin_str, isin_code, name);

            GainTransaction t;
            t.mDate = parse_date(tx["transaction_date"].get<std::string>());
            t.mType = tx["transaction_type"].get<std::string>();
            t.mQuantity = tx["amount_of_units"].get<double>();
            t.mIsin = isin_code;
            t.mIsinName = name;

            // Get unit_price: prefer "unit_price", fallback to "market_value" / quantity
            if (tx.contains("unit_price")) {
                t.mUnitPrice = tx["unit_price"].get<double>();
            } 
            else if (tx.contains("market_value")) {
                double market_value = tx["market_value"].get<double>();
                t.mUnitPrice = (t.mQuantity != 0.0) ? market_value / t.mQuantity : 0.0;
            } 
            else {
                continue;  // Skip if no price info
            }

            aTransactions[isin_code].push_back(t);
        }
    }
}

void parse_income_section(const nlohmann::json& income_section, IncomeTransactions& aTransactions) {
    for (const auto& entry : income_section) {
        if (!entry.contains("transactions") || !entry["transactions"].is_array()) continue;


        if (entry["asset_type"] == "Equities" || entry["asset_type"] == "Funds") {
            parse_div_section(entry, aTransactions.mDivTransactions, entry["country"].get<std::string>());
        }

        if (entry["asset_type"] == "Liquidity") {
            parse_interests_section(entry, aTransactions.mInterests);
        }

    }
}

static void parse_div_section(const nlohmann::json& div_section,  std::map<std::string, std::vector<DivTransaction>>& aTransactions, std::string country_name) {
    for (const auto& tx : div_section["transactions"]) {

        std::string isin_str = tx["isin"];
        std::string isin_code;
        std::string name;
        parse_isin(isin_str, isin_code, name);

        DivTransaction t;
        t.mDate = parse_date(tx["value_date"].get<std::string>());
        t.mIsin = isin_code;
        t.mIsinName = name;
        t.mGrossIncome = tx["gross_income"].get<double>();
        t.mWitholdTax = tx["withholding_tax"].get<double>();
        t.mCountryName = country_name;

        aTransactions[isin_code].push_back(t);
    }
}

static void parse_interests_section(const nlohmann::json& interests_section,  std::map<std::string, std::vector<DhoTransaction>>& aTransactions) {
    DhoTransaction income;

    income.mPayer = "Trade Republic";
    income.mGrossIncome = interests_section["totals"]["gross_income"].get<double>();
    income.mWitholdingTax = interests_section["totals"]["gross_income"].get<double>() - interests_section["totals"]["net_income"].get<double>();

    aTransactions["Trade Republic"].push_back(income);
}