#pragma once

#include <iostream>
#include <sstream>
#include <iomanip>   

#include "xml_generator.hpp"

// Helper function to parse date from DD.MM.YYYY to YYYY-MM-DD
std::string parse_date(const std::string& date_str);

// Helper to extract ISIN code and name from "ISIN - Name" format
void parse_isin(const std::string& isin_str, std::string& code, std::string& name);

TransactionType string_to_asset_type(std::string mType);
std::string to_xml_decimal(double value, int precision);

// Parse gains and losses section
void parse_gains_section(const nlohmann::json& gains_section, TransactionType aType, std::map<std::string, std::vector<GainTransaction>>& aTransactions);

// Parse income (dividend) section
void parse_income_section(const nlohmann::json& div_section, std::map<std::string, std::vector<DivTransaction>>& aTransactions);