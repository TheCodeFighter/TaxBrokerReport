#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>
#include <sstream>
#include <string_view>

class ReportLoader {
    public:
        enum class ProcessingMode {
            InMemory,  // Store the raw text in memory (suitable for small PDFs)
            FileBased  // Write the raw text to a temporary file (suitable for large PDFs)
        };

        ReportLoader() = default;

        void getRawPdfData(const std::string& aPdfPath, ProcessingMode aMode = ProcessingMode::InMemory);
        nlohmann::json convertToJson();
        void clearRawText();
        
        #ifdef UNIT_TEST
        bool saveRawDataToFile(const std::string& aOutputPath) const;
        void setRawText(const std::string& aText);
        const std::string& getRawText() const;
        #endif

    private:
        struct TransactionContext {
            std::string mIsin;
            std::string mAssetType;
            std::string mCountry;
            std::vector<nlohmann::json> mTransactions;
            nlohmann::json mTotals;
        };

        ProcessingMode mMode = ProcessingMode::InMemory;
        std::string mRawText {};
        std::string mTempFilePath {};
        std::string mClientNumber {};
        TransactionContext mLastContext {};
        
        void parseHeader(std::istringstream& aIss, nlohmann::json& aResult);
        void parseIncomeSection(std::istringstream& aIss, std::vector<nlohmann::json>& aIncomeSections);
        void parseGainsAndLossesSection(std::istringstream& aIss, std::vector<nlohmann::json>& aGainsSections);
        void parseWithholdingTaxSection(std::istringstream& aIss, std::vector<nlohmann::json>& aWithholdingSections);
        void parseTransactionHistorySection(std::istringstream& aIss, std::vector<nlohmann::json>& aTransactionHistory);
        
        std::string trim(std::string_view aLine) const;
        std::optional<std::string> extractLine(std::istringstream& aIss) const;
        std::optional<double> parseDouble(const std::string& aValue) const;
        std::vector<std::string> tokenize(std::string_view aLine) const;
        std::vector<std::string> normalizeSpaces(const std::string &aLine) const;
};

