#pragma once

#include <iostream>
#include <sstream>
#include <iomanip>   
#include <set>
#include <chrono>

#include "xml_generator.hpp"

// Helper function to parse date from DD.MM.YYYY to YYYY-MM-DD
std::string parse_date(const std::string& date_str);

// Helper to calculate days between two dates in YYYY-MM-DD format
int days_between(const std::string& from, const std::string& to);

// Helper to extract ISIN code and name from "ISIN - Name" format
void parse_isin(const std::string& isin_str, std::string& code, std::string& name);

TransactionType string_to_asset_type(std::string mType);
std::string form_type_to_string(FormType t);
std::string form_type_to_string_code(FormType t);
std::string to_xml_decimal(double value, int precision);

// Parse gains and losses section
void parse_gains_section(const nlohmann::json& gains_section, std::set<TransactionType> aTypes, std::map<std::string, std::vector<GainTransaction>>& aTransactions);

// Parse income (dividend) section
void parse_income_section(const nlohmann::json& div_section, IncomeTransactions& aTransactions);