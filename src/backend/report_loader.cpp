#include <poppler/cpp/poppler-document.h>
#include <poppler/cpp/poppler-page.h>
#include <stdexcept>
#include <ranges>
#include <regex>
#include <algorithm>
#include <filesystem>
#include <fstream>

#include "report_loader.hpp"

//TODO: remove for prod
#include <iostream>

constexpr size_t RAW_DATA_PAGE_SIZE_BYTES = 1024; // Estimated bytes per page
inline constexpr auto SECTION_INCOME      = "Detailed Income Section";
inline constexpr auto SECTION_GAINS       = "Detailed Gains and Losses Section";
inline constexpr auto SECTION_WITHHOLDING = "Detailed Withholding Tax Section";
inline constexpr auto SECTION_HISTORY     = "History of Transactions and Corporate Actions";

// Provide a simple implementation for getNonNegativeDouble to ensure linkage.
// Returns the value if non-negative, otherwise returns 0.0.
double getNonNegativeDouble(const double& v) {
    return (v < 0.0) ? -v : v;
}

void ReportLoader::getRawPdfData(const std::string& aPdfPath, ProcessingMode aMode) {
    std::unique_ptr<poppler::document> doc {poppler::document::load_from_file(aPdfPath)};
    if (!doc) {
        throw std::runtime_error {"Failed to load PDF: " + aPdfPath};
    }

    const auto numPages = doc->pages();
    if (numPages == 0) {
        throw std::runtime_error {"PDF has no pages: " + aPdfPath};
    }

    clearRawText();
    mMode = aMode;

    // Start from page 0 to process all pages
    constexpr int startPage {0};
    const auto numPagesToProcess = numPages - startPage;

    if (aMode == ProcessingMode::InMemory) {
        mRawText.reserve(numPagesToProcess * RAW_DATA_PAGE_SIZE_BYTES);     // TODO: check if 1024B on page is ok, and make num

        bool hasContent {false};
        for (const auto i : std::views::iota(startPage, numPages)) {
            std::unique_ptr<poppler::page> page {doc->create_page(i)};
            if (!page) {
                continue;
            }
            const auto pageText = page->text().to_utf8();
            std::string text {pageText.begin(), pageText.end()};
            if (!text.empty()) {
                mRawText += text + "\n";
                hasContent = true;
            }
        }

        if (!hasContent) {
            throw std::runtime_error {"No text extracted from PDF: " + aPdfPath};
        }
    } 
    else if (aMode == ProcessingMode::FileBased) {
        // Generate a unique temporary file name
        auto tempDir = std::filesystem::temp_directory_path();
        auto uniqueSuffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        mTempFilePath = (tempDir / ("pdf_extract_" + uniqueSuffix + ".txt")).string();
        std::ofstream tempFile {mTempFilePath, std::ios::binary};
        if (!tempFile) {
            throw std::runtime_error {"Failed to create temporary file: " + mTempFilePath};
        }

        bool hasContent = false;
        for (const auto i : std::views::iota(startPage, numPages)) {
            std::unique_ptr<poppler::page> page {doc->create_page(i)};
            if (!page) {
                continue;
            }
            const auto pageText = page->text().to_utf8();
            std::string text {pageText.begin(), pageText.end()};
            if (!text.empty()) {
                tempFile << text << "\n";
                hasContent = true;
            }
        }

        tempFile.close();  // TODO: some mutex for safly file filling?

        if (!hasContent) {
            std::filesystem::remove(mTempFilePath);
            mTempFilePath.clear();
            throw std::runtime_error {"No text extracted from PDF: " + aPdfPath};
        }
    }
    else {
        throw std::runtime_error {"Unknown processing aMode"};
    }

}

nlohmann::json ReportLoader::convertToJson() {
    std::istringstream iss {};
    if (mMode == ProcessingMode::InMemory) {
        if (mRawText.empty()) {
            throw std::runtime_error {"No raw text available to convert to JSON"};
        }

        iss.str(mRawText);
    }
    else if (mMode == ProcessingMode::FileBased) {
        if (mTempFilePath.empty()) {
            throw std::runtime_error {"No temporary file available to convert to JSON"};
        }

        std::ifstream file {mTempFilePath};
        if (!file) {
            throw std::runtime_error {"Failed to open temporary file: " + mTempFilePath};
        }

        // Read the entire file into a string
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string fileContent = buffer.str();
        file.close();

        iss.str(fileContent);
    }
    else {
        throw std::runtime_error {"Unknown processing aMode"};
    }

    nlohmann::json result;
    std::vector<nlohmann::json> incomeSections;
    std::vector<nlohmann::json> gainsAndLossesSections;
    std::vector<nlohmann::json> withholdingTaxSections;
    std::vector<nlohmann::json> transactionHistory;

    std::string currentSection;
    std::regex sectionRegex(R"(^([IVX]+\.)\s+(.+)$)");

    while (auto line = extractLine(iss)) {
        std::string trimmedLine = trim(*line);
        if (trimmedLine.empty()) continue;

        std::smatch m;
        if (std::regex_match(trimmedLine, m, sectionRegex)) {
            currentSection = m[2].str();
            continue;
        }

        if (currentSection.empty()) {
            parseHeader(iss, result);
            continue;
        }

        (currentSection == SECTION_INCOME)      ? parseIncomeSection(iss, incomeSections) :
        (currentSection == SECTION_GAINS)       ? parseGainsAndLossesSection(iss, gainsAndLossesSections) :
        (currentSection == SECTION_WITHHOLDING) ? parseWithholdingTaxSection(iss, withholdingTaxSections) :
        (currentSection == SECTION_HISTORY)     ? parseTransactionHistorySection(iss, transactionHistory) :
                                        ((void)0);
    }

    result["income_section"] = incomeSections;
    result["gains_and_losses_section"] = gainsAndLossesSections;
    result["withholding_tax_section"] = withholdingTaxSections;
    result["transaction_history"] = transactionHistory;

    return result;
}

void ReportLoader::clearRawText() {
    if (mMode == ProcessingMode::InMemory) {
        mRawText.clear();
    } else {
        if (!mTempFilePath.empty()) {
            std::filesystem::remove(mTempFilePath);
            mTempFilePath.clear();
        }
    }
}

void ReportLoader::parseHeader(std::istringstream& aIss, nlohmann::json& aResult) {
    std::regex headerRegex(R"(^(Client|Period|Currency|Country):\s+(.+)$)");
    std::smatch match;

    while (auto line = extractLine(aIss)) {
        std::string trimmedLine = trim(*line);
        if (trimmedLine.empty()) continue;

        if (std::regex_match(trimmedLine, match, headerRegex)) {
            std::string key = match[1].str();
            std::string value = match[2].str();
            if (key == "Client"){
                aResult["client"] = value;
                mClientNumber = value;
            } 
            else if (key == "Period") aResult["period"] = value;
            else if (key == "Currency") aResult["currency"] = value;
            else if (key == "Country") aResult["country"] = value;
        } else {
            // Rewind the stream to the beginning of the non-header line
            aIss.seekg(-static_cast<long>(line->length() + 1), std::ios::cur);
            break;
        }
    }
}

void ReportLoader::parseIncomeSection(std::istringstream& aIss, std::vector<nlohmann::json>& aIncomeSections) {
    TransactionContext context;
    std::regex assetTypeRegex(R"(^Asset Type: (.+)$)");
    std::regex countryRegex(R"(^Country: (.+)$)");
    std::regex isinRegex(R"(^([A-Z0-9]+ - .+)$)");
    std::regex interestPaymentRegex(R"(^(Interest payment)\s+(\d{2}\.\d{2}\.\d{4})\s+([\d,.]+)\s+([\d,.]+)\s*$)");
    std::regex dividendRegex(R"(^(Dividend)\s+(\d{2}\.\d{2}\.\d{4})\s+([\d,.]+)\s+([\d,.]+)\s*$)");
    std::regex amountRegex(R"(^EUR\s+([\d,.]+)\s+(-?[\d,.]+)?\s*([\d,.]+)?\s*$)");
    std::regex incomeTotalRegex(R"(^Total for ([A-Za-z\s]+)\s+EUR\s+([\d,.]+)\s+(-?[\d,.]+)?\s*([\d,.]+)?$)");
    std::regex sectionRegex(R"(^([IVX]+\.)\s+(.+)$)");

    bool hasTransactions = false;

    while (auto line = extractLine(aIss)) {
        std::string trimmedLine = trim(*line);
        if (trimmedLine.empty()) continue;

        std::smatch match;
        if (std::regex_match(trimmedLine, match, sectionRegex)) {
            aIss.seekg(-static_cast<long>(line->length() + 1), std::ios::cur);
            if (hasTransactions) {
                nlohmann::json section;

                if (!context.mAssetType.empty() && !context.mCountry.empty()) {
                    mLastContext.mAssetType = context.mAssetType;
                    mLastContext.mCountry = context.mCountry;

                    section["asset_type"] = mLastContext.mAssetType;
                    section["country"] = mLastContext.mCountry;
                    section["transactions"] = context.mTransactions;
                    if (context.mTotals.empty()) {
                        double grossIncome = 0.0;
                        double netIncome = 0.0;
                        for (const auto& txn : context.mTransactions) {
                            grossIncome += txn["gross_income"].get<double>();
                            netIncome += txn["net_income"].get<double>();
                        }
                        context.mTotals["gross_income"] = grossIncome;
                        context.mTotals["net_income"] = netIncome;
                    }
                    section["totals"] = context.mTotals;
                    aIncomeSections.push_back(section);
                }
                else {
                    for (auto it = aIncomeSections.rbegin(); it != aIncomeSections.rend(); ++it) {
                        if ((*it)["asset_type"] == mLastContext.mAssetType) {
                            std::copy(context.mTransactions.begin(),
                                        context.mTransactions.end(),
                                        std::back_inserter((*it)["transactions"]));

                            double grossIncome = (*it)["totals"]["gross_income"].get<double>();
                            double netIncome = (*it)["totals"]["net_income"].get<double>();
                            for (const auto& txn : context.mTransactions) {
                                grossIncome += txn["gross_income"].get<double>();
                                netIncome += txn["net_income"].get<double>();
                            }
                            context.mTotals["gross_income"] = grossIncome;
                            context.mTotals["net_income"] = netIncome;
                            (*it)["totals"] = context.mTotals;
                            
                            break;
                        }
                    }
                }
            }
            break;
        }

        if (std::regex_match(trimmedLine, match, assetTypeRegex)) {
            context.mAssetType = match[1].str();
            continue;
        }

        if (std::regex_match(trimmedLine, match, countryRegex)) {
            std::string country = match[1].str();
            if (std::count(country.begin(), country.end(), ' ') > 10) {
                continue; // skip this line if more than 3 spaces
            }

            context.mCountry = std::move(country);
            continue;
        }

        if (std::regex_match(trimmedLine, match, isinRegex)) {
            context.mIsin = match[1].str();
            continue;
        }

        if (trimmedLine == mClientNumber) {
            context.mIsin = "CLIENT";
            continue; // Skip lines that match the client number
        }

        if (std::regex_match(trimmedLine, match, interestPaymentRegex) ||
            std::regex_match(trimmedLine, match, dividendRegex)) {
            nlohmann::json transaction;

            std::string isin = !context.mIsin.empty() ? context.mIsin : mLastContext.mIsin;
            mLastContext.mIsin = isin.empty() ? mLastContext.mIsin : isin;

            if ((context.mAssetType != "Liquidity" || mLastContext.mAssetType != "Liquidity") && isin != "CLIENT") {
                transaction["isin"] = isin;
                transaction["DEPOSIT"] = false;
            }
            if ((context.mAssetType == "Liquidity" || mLastContext.mAssetType == "Liquidity") && isin == "CLIENT") {
                transaction["DEPOSIT"] = true;
            }
            transaction["transaction_type"] = match[1].str();
            transaction["value_date"] = match[2].str();
            std::string amountStr = match[3].str();
            std::string exchangeRateStr = match[4].str();
            transaction["amount_of_units"] = getNonNegativeDouble(parseDouble(amountStr).value_or(0.0));
            transaction["exchange_rate"] = parseDouble(exchangeRateStr).value_or(0.0);

            transaction["gross_income"] = 0.0;
            if (context.mAssetType != "Liquidity") {
                transaction["withholding_tax"] = 0.0;
            }
            transaction["net_income"] = 0.0;

            auto currentPos = aIss.tellg();
            bool foundAmount = false;

            // Check the next line for EUR amounts
            while (auto nextLine = extractLine(aIss)) {
                std::string trimmedNext = trim(*nextLine);
                std::smatch amountMatch;
                if (std::regex_match(trimmedNext, amountMatch, amountRegex)) {
                    std::string grossStr = amountMatch[1].str();
                    transaction["gross_income"] = parseDouble(grossStr).value_or(0.0);

                    if (amountMatch[2].matched) {
                        std::string taxStr = amountMatch[2].str();
                        if (context.mAssetType != "Liquidity") {
                            transaction["withholding_tax"] = getNonNegativeDouble(parseDouble(taxStr).value_or(0.0));
                        }
                    } else {
                        if (context.mAssetType != "Liquidity") {
                            transaction["withholding_tax"] = 0.0;
                        }
                    }

                    if (amountMatch[3].matched) {
                        std::string netStr = amountMatch[3].str();
                        transaction["net_income"] = parseDouble(netStr).value_or(0.0);
                    }
                    else if (amountMatch.size() == 4 && amountMatch[3].str().empty() && match[2].str()[0] != '-') {
                        std::string netStr = amountMatch[2].str();
                        transaction["net_income"] = parseDouble(netStr).value_or(0.0);
                    }
                    else {
                        double gross = transaction["gross_income"].get<double>();
                        if (context.mAssetType != "Liquidity") {
                            double tax = transaction["withholding_tax"].get<double>();
                            transaction["net_income"] = gross + tax;
                        }
                    }

                    foundAmount = true;
                    aIss.seekg(-static_cast<long>(nextLine->length() + 1), std::ios::cur);
                    break;
                }
                aIss.seekg(-static_cast<long>(nextLine->length() + 1), std::ios::cur);
                break; // Avoid infinite loop, only check the immediate next line
            }

            // Fallback to previous line only if next line didn't match
            if (!foundAmount && currentPos > 0) {
                aIss.seekg(-static_cast<long>(line->length() + 1), std::ios::cur);
                while (auto prevLine = extractLine(aIss)) {
                    std::string trimmedPrev = trim(*prevLine);
                    std::smatch amountMatch;
                    if (std::regex_match(trimmedPrev, amountMatch, amountRegex)) {
                        std::string grossStr = amountMatch[1].str();
                        transaction["gross_income"] = parseDouble(grossStr).value_or(0.0);

                        if (amountMatch[2].matched) {
                            std::string taxStr = amountMatch[2].str();
                            if (context.mAssetType != "Liquidity") {
                                double withholding_tax = parseDouble(taxStr).value_or(0.0);
                                transaction["withholding_tax"] = withholding_tax;
                            }
                        } else {
                            if (context.mAssetType != "Liquidity") {
                                transaction["withholding_tax"] = 0.0;
                            }
                        }

                        if (amountMatch[3].matched) {
                            std::string netStr = amountMatch[3].str();
                            transaction["net_income"] = parseDouble(netStr).value_or(0.0);
                        }
                        else if (amountMatch.size() == 4 && amountMatch[3].str().empty() && match[2].str()[0] != '-') {
                            std::string netStr = amountMatch[2].str();
                            transaction["net_income"] = parseDouble(netStr).value_or(0.0);
                        }
                        else {
                            double gross = transaction["gross_income"].get<double>();
                            if (context.mAssetType != "Liquidity") {
                                double tax = transaction["withholding_tax"].get<double>();
                                transaction["net_income"] = gross + tax;
                            }
                        }

                        foundAmount = true;
                        aIss.seekg(-static_cast<long>(prevLine->length() + 1), std::ios::cur);
                        break;
                    }
                    aIss.seekg(-static_cast<long>(prevLine->length() + 1), std::ios::cur);
                    break; // Avoid infinite loop
                }
                aIss.seekg(currentPos, std::ios::beg);
            }

            context.mTransactions.push_back(transaction);
            hasTransactions = true;
        }
    }
}

void ReportLoader::parseGainsAndLossesSection(std::istringstream& aIss, std::vector<nlohmann::json>& aGainsSections) {
    TransactionContext context;
    std::regex assetTypeRegex(R"(^Asset Type: (.+)$)");
    std::regex countryRegex(R"(^Country: (.+)$)");
    std::regex isinRegex(R"(^([A-Z0-9]+ - .+)$)");
    std::regex gainsTransactionRegex(R"(^(Trading Buy|Trading Sell)\s+(\d{2}\.\d{2}\.\d{4})\s+(-?\d{1,3}(?:,\d{3})*(?:\.\d+)?)\s+(-?\d{1,3}(?:,\d{3})*(?:\.\d+)?)\s*$)");
    std::regex gainsAmountRegex(R"(^EUR\s+(-?\d{1,3}(?:,\d{3})*(?:\.\d+)?)\s+(-?\d{1,3}(?:,\d{3})*(?:\.\d+)?)\s+(-?\d{1,3}(?:,\d{3})*(?:\.\d+)?)\s+(-?\d{1,3}(?:,\d{3})*(?:\.\d+)?)\s+(-?\d{1,3}(?:,\d{3})*(?:\.\d+)?)\s+(-?\d{1,3}(?:,\d{3})*(?:\.\d+)?)$)");
    std::regex gainsTotalRegex(R"(^(Gains|Losses)\s+EUR\s+(-?\d{1,3}(?:,\d{3})*(?:\.\d+)?)\s+(-?\d{1,3}(?:,\d{3})*(?:\.\d+)?)\s+(-?\d{1,3}(?:,\d{3})*(?:\.\d+)?)$)");
    std::regex sectionRegex(R"(^([IVX]+\.)\s+(.+)$)");
    std::regex reportIDRegex(R"(^Report ID:\s+(.+)$)");

    bool inTransactionBlock = false;

    while (auto line = extractLine(aIss)) {
        std::string trimmedLine = trim(*line);
        if (trimmedLine.empty()) continue;

        std::smatch match;
        if (std::regex_match(trimmedLine, match, sectionRegex)) {
            aIss.seekg(-static_cast<long>(line->length() + 1), std::ios::cur);
            break;
        }

        if (std::regex_match(trimmedLine, match, assetTypeRegex)) {
            context.mAssetType = match[1].str();
            continue;
        }

        if (std::regex_match(trimmedLine, match, countryRegex)) {
            context.mCountry = match[1].str();
            continue;
        }

        if (std::regex_match(trimmedLine, match, isinRegex)) {
            context.mIsin = match[1].str();
            inTransactionBlock = true;
            continue;
        }

        if (std::regex_match(trimmedLine, match, gainsTotalRegex) && !context.mIsin.empty()) {
            std::string type = match[1].str();

            nlohmann::json section;
            section["asset_type"] = context.mAssetType;
            section["country"] = context.mCountry;
            section["transactions"] = context.mTransactions;
            // section["totals"] = context.mTotals;
            aGainsSections.push_back(section);
            context = TransactionContext();
            inTransactionBlock = false;
            continue;
        }

        if (std::regex_match(trimmedLine, match, gainsTransactionRegex) && !inTransactionBlock) {
            inTransactionBlock = true;
            context = mLastContext;
        }

        if (inTransactionBlock) {
            if (std::regex_match(trimmedLine, match, gainsTransactionRegex)) {
                nlohmann::json transaction;
                transaction["isin"] = context.mIsin;
                transaction["transaction_type"] = match[1].str();
                transaction["transaction_date"] = match[2].str();

                auto amount = match.size() > 3 ? parseDouble(match[3].str()) : std::nullopt;
                auto rate   = match.size() > 4 ? parseDouble(match[4].str()) : std::nullopt;

                transaction["amount_of_units"] = getNonNegativeDouble(amount.value_or(0.0));
                transaction["exchange_rate"]   = rate.value_or(0.0);

                // Look for the EUR line with additional details
                if (auto nextLine = extractLine(aIss)) {
                    std::string trimmedNext = trim(*nextLine);
                    if (std::regex_match(trimmedNext, match, gainsAmountRegex)) {
                        transaction["unit_price"] = parseDouble(match[1].str()).value_or(0.0);
                    } else {
                        std::vector<std::string> amounts {ReportLoader::normalizeSpaces(trimmedNext)};
                        transaction["unit_price"] = parseDouble(amounts[0]).value_or(0.0);
                    }
                }

                context.mTransactions.push_back(transaction);
            } 
            else if (std::regex_match(trimmedLine, match, reportIDRegex) && inTransactionBlock){
                mLastContext = context;
            }
            continue;
        }
    }
}

void ReportLoader::parseWithholdingTaxSection(std::istringstream& aIss, std::vector<nlohmann::json>& aWithholdingSections) {
    TransactionContext context;
    std::regex countryRegex(R"(^([A-Za-z\s]+)$)");
    std::regex isinRegex(R"(^([A-Z0-9]+ - .+)$)");
    std::regex withholdingTaxRegex(R"(^(Dividend)\s+(\d{2}\.\d{2}\.\d{4})\s+([\d\.]+)\s*$)");
    std::regex withholdingAmountRegex(R"(^EUR\s+([\d\.]+)\s+([\d\.]+)\s+([\d\.]+%)\s+([\d\.]+)\s*$)");
    std::regex withholdingTotalRegex(R"(^(Total for|Overall Total In EUR)\s+([\d\.]+)\s+([\d\.]+)$)");
    std::regex sectionRegex(R"(^([IVX]+\.)\s+(.+)$)");

    while (auto line = extractLine(aIss)) {
        std::string trimmedLine = trim(*line);
        if (trimmedLine.empty()) continue;

        std::smatch match;
        if (std::regex_match(trimmedLine, match, sectionRegex)) {
            aIss.seekg(-static_cast<long>(line->length() + 1), std::ios::cur);
            break;
        }

        if (std::regex_match(trimmedLine, match, countryRegex) && !trimmedLine.starts_with("Total")) {
            context.mCountry = match[1].str();
            continue;
        }

        if (std::regex_match(trimmedLine, match, isinRegex)) {
            context.mIsin = match[1].str();
            continue;
        }

        if (std::regex_match(trimmedLine, match, withholdingTaxRegex)) {
                nlohmann::json transaction;
                transaction["isin"] = context.mIsin;
                transaction["transaction_type"] = match[1].str();
            transaction["payment_date"] = match[2].str();
            transaction["exchange_rate"] = parseDouble(match[3].str()).value_or(0.0);

                if (auto nextLine = extractLine(aIss)) {
                    std::string trimmedNext = trim(*nextLine);
                if (std::regex_match(trimmedNext, match, withholdingAmountRegex)) {
                    transaction["income_in_eur"] = parseDouble(match[1].str()).value_or(0.0);
                    transaction["withholding_tax_amount_in_eur"] = parseDouble(match[2].str()).value_or(0.0);
                    transaction["withholding_tax_rate"] = match[3].str();
                    transaction["dtt_amount_in_eur"] = parseDouble(match[4].str()).value_or(0.0);
                }
            }

            if (auto nextLine = extractLine(aIss)) {
                std::string trimmedNext = trim(*nextLine);
                if (std::regex_match(trimmedNext, match, withholdingAmountRegex)) {
                    transaction["income_in_eur"] = parseDouble(match[1].str()).value_or(0.0);
                    transaction["withholding_tax_amount_in_eur"] = parseDouble(match[2].str()).value_or(0.0);
                    transaction["dtt_rate"] = match[3].str();
                    transaction["dtt_amount_in_eur"] = parseDouble(match[4].str()).value_or(0.0);
                    }
                }
                context.mTransactions.push_back(transaction);
        } else if (std::regex_match(trimmedLine, match, withholdingTotalRegex)) {
            std::string totalType = match[1].str();
            (void)totalType;
            nlohmann::json totals;
            totals["withholding_tax_amount_in_eur"] = parseDouble(match[2].str()).value_or(0.0);
            totals["dtt_amount_in_eur"] = parseDouble(match[3].str()).value_or(0.0);
            context.mTotals = totals;
            nlohmann::json section;
            section["country"] = context.mCountry;
            section["transactions"] = context.mTransactions;
            section["totals"] = context.mTotals;
            aWithholdingSections.push_back(section);
            context = TransactionContext();
        }
    }
}

void ReportLoader::parseTransactionHistorySection(std::istringstream& aIss, std::vector<nlohmann::json>& aTransactionHistory) {
    TransactionContext context;
    std::regex isinRegex(R"(^([A-Z0-9]+ - .+)$)");
    std::regex transactionHistoryRegex(
    R"(^(Trading Buy|Trading Sell)\s+(\d{2}\.\d{2}\.\d{4})\s+(\d{2}\.\d{2}\.\d{4})\s+EUR\s+([\d\.,]+)\s+(-?[\d\.,]+)\s+([\d\.,]+)\s+([\d\.,]+)$)");
    std::regex sectionRegex(R"(^([IVX]+\.)\s+(.+)$)");

    while (auto line = extractLine(aIss)) {
        std::string trimmedLine = trim(*line);
        if (trimmedLine.empty()) continue;

        std::smatch match;
        if (std::regex_match(trimmedLine, match, sectionRegex)) {
            aIss.seekg(-static_cast<long>(line->length() + 1), std::ios::cur);
            break;
        }

        if (std::regex_match(trimmedLine, match, isinRegex)) {
            if (!context.mTransactions.empty() && !context.mIsin.empty()) {
                nlohmann::json group;
                group["isin"] = context.mIsin;
                group["transactions"] = context.mTransactions;
                aTransactionHistory.push_back(group);
                context.mTransactions.clear();
            }
            context.mIsin = match[1].str();
            continue;
        }
        
        if (std::regex_match(trimmedLine, match, transactionHistoryRegex)) {
            nlohmann::json transaction;
            transaction["transaction_type"] = match[1].str();
            transaction["transaction_date"] = match[2].str();
            transaction["value_date"] = match[3].str();
            transaction["currency"] = "EUR";
            transaction["exchange_rate"] = parseDouble(match[4].str()).value_or(0.0);
            transaction["amount_of_units"] = getNonNegativeDouble(parseDouble(match[5].str()).value_or(0.0));
            transaction["market_value"] = parseDouble(match[6].str()).value_or(0.0);

            // If we have next page, before we finish all transactions, we need to check if ISIN is empty and get last ISIN
            if (context.mIsin.empty() && !mLastContext.mIsin.empty()) {
                for (auto it = aTransactionHistory.rbegin(); it != aTransactionHistory.rend(); ++it) {
                    if ((*it)["isin"] == mLastContext.mIsin) {
                        (*it)["transactions"].push_back(transaction);
                        break;
                    }
                }
            continue; // skip creating a new group
            }
            else {
                context.mTransactions.push_back(transaction);
            }
        }
    }

    if (!context.mTransactions.empty()) {
        nlohmann::json group;
        group["isin"] = context.mIsin;
        group["transactions"] = context.mTransactions;
        aTransactionHistory.push_back(group);

        // Remeber last valid ISIN and transactions for next page processing
        if (!context.mIsin.empty()) {
            mLastContext.mIsin = context.mIsin;
        }
    }
}

std::string ReportLoader::trim(std::string_view aLine) const {
    auto trimmed = aLine | std::ranges::views::drop_while([](char c) { return std::isspace(c); })
                        | std::ranges::views::reverse
                        | std::ranges::views::drop_while([](char c) { return std::isspace(c); })
                        | std::ranges::views::reverse;
    return std::string(trimmed.begin(), trimmed.end());
}

std::optional<std::string> ReportLoader::extractLine(std::istringstream& aIss) const {
    std::string line;
    if (std::getline(aIss, line)) {
        if (line.starts_with("1: ")) {
            line = line.substr(3);
        }
        return line;
    }
    return std::nullopt;
}

std::optional<double> ReportLoader::parseDouble(const std::string& mValue) const {
    std::string s = mValue;

    // Remove thousands separators (comma)
    s.erase(std::remove(s.begin(), s.end(), ','), s.end());

    // Remove spaces
    s.erase(std::remove_if(s.begin(), s.end(), ::isspace), s.end());

    try {
        size_t pos;
        double result = std::stod(s, &pos);
        if (pos == s.length()) {
            return result;
        }
    } catch (...) {
        // Ignore parsing errors for now
    }
    return std::nullopt;
}

std::vector<std::string> ReportLoader::tokenize(std::string_view aLine) const {
    std::vector<std::string> tokens;
    std::istringstream token_stream{std::string(aLine)};
    std::string token;
    while (token_stream >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

// Replace common "weird" whitespace and split into array of number strings
std::vector<std::string> ReportLoader::normalizeSpaces(const std::string &aLine) const {
    std::vector<std::string> tokens;
    std::string current;
    for (unsigned char c : aLine) {
        if ((c >= '0' && c <= '9') || c == '-' || c == '.') {
            current += c;
        } else {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
        }
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

#ifdef UNIT_TEST
bool ReportLoader::saveRawDataToFile(const std::string& aOutputPath) const {
    try {
        std::ofstream outputFile(aOutputPath, std::ios::binary);
        if (!outputFile) {
            std::cerr << "Failed to create output file: " << aOutputPath << std::endl;
            return false;
        }

        if (mMode == ProcessingMode::InMemory) {
            if (mRawText.empty()) {
                std::cerr << "No raw text available to save" << std::endl;
                return false;
            }
            outputFile << mRawText;
        } else {
            if (mTempFilePath.empty()) {
                std::cerr << "No temporary file available to save" << std::endl;
                return false;
            }

            std::ifstream tempFile(mTempFilePath, std::ios::binary);
            if (!tempFile) {
                std::cerr << "Failed to open temporary file: " << mTempFilePath << std::endl;
                return false;
            }

            outputFile << tempFile.rdbuf();
        }

        outputFile.close();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Exception while saving raw data: " << e.what() << std::endl;
        return false;
    }
}

void ReportLoader::setRawText(const std::string& aText) {
    mRawText = aText;
}

const std::string& ReportLoader::getRawText() const {
    return mRawText;
}
#endif