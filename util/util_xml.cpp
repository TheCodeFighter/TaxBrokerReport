#include "util_xml.hpp"
#include <algorithm>

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
TransactionType string_to_asset_type(std::string type) {
    if (type == "Crypto Currency") return TransactionType::Crypto;
    if (type == "Equities") return TransactionType::Equities;
    if (type == "Funds") return TransactionType::Funds;
    if (type == "Bonds") return TransactionType::Bonds;

    return TransactionType::None;
}



std::string to_xml_decimal(double value, int precision) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << value;
    return oss.str();
}