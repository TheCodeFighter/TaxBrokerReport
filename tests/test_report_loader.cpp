#include <report_loader.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <iostream>
#include <regex>

// Define paths for the input PDF and the output JSON file
const std::filesystem::path projectRoot {PROJECT_SOURCE_DIR};
const std::filesystem::path pdfPath = projectRoot / "tmp" / "test_report.pdf";
const std::filesystem::path outputJsonPath = projectRoot / "tmp" / "generated_test_output.json";
const std::filesystem::path expectedJson = projectRoot / "tests" / "testData" /"expected_test_output.json";
const std::filesystem::path txtPdfData = projectRoot / "tests" / "testData" /"generated_test_data.txt";
const std::filesystem::path rawDataPath = projectRoot / "tmp" / "generated_test_data.txt";

#ifndef SAVE_GENERATED_TXT_FILES
    #define SAVE_GENERATED_TXT_FILES 0
#endif

// Test helper: subclass ReportLoader to fake PDF extraction when the real PDF is not present.
class TestReportLoader : public ReportLoader {
    public:
        void getRawPdfData(const std::string& aPdfPath, ProcessingMode aMode = ProcessingMode::InMemory) {
            if (std::filesystem::exists(aPdfPath)) {
                // Use PDF if available
                ReportLoader::getRawPdfData(aPdfPath, aMode);
                return;
            }

            // Fallback for CI: read pre-extracted text and set it as raw text
            std::ifstream txtFile(txtPdfData);
            if (!txtFile.is_open()) {
                throw std::runtime_error("Required test data missing: " + txtPdfData.string());
            }
            std::stringstream buffer;
            buffer << txtFile.rdbuf();
            setRawText(buffer.str());
        }
};

TEST(ReportLoaderTest, GetRawPdfData_InMemory) {
    ASSERT_TRUE(std::filesystem::exists(expectedJson)) << "Expected JSON file does not exist: " << expectedJson;
    ASSERT_TRUE(std::filesystem::exists(txtPdfData)) << "PDF TXT file does not exist: " << txtPdfData;

    std::ifstream expectedJsonFile(expectedJson);
    ASSERT_TRUE(expectedJsonFile.is_open()) << "Failed to open the expected JSON file: " << expectedJson;

    nlohmann::json expectedParsedJson;
    expectedJsonFile >> expectedParsedJson;
    expectedJsonFile.close();
    
    std::string rawPdfText;
    {
        std::ifstream txtFile(txtPdfData);
        ASSERT_TRUE(txtFile.is_open()) << "Failed to open the PDF TXT file: " << txtPdfData;
        std::stringstream buffer;
        buffer << txtFile.rdbuf();
        rawPdfText = buffer.str();
        txtFile.close();
    }
    
    ReportLoader loader;
    loader.setRawText(rawPdfText);
    auto parseData = loader.convertToJson();

    ASSERT_TRUE(parseData.contains("client")) << "client field missing in parsed data";
    ASSERT_FALSE(parseData["client"].get<std::string>().empty()) << "client is empty";

    loader.clearRawText();
}

TEST(ReportLoaderTest, GetRawPdfData_InvalidPath) {
    ReportLoader loader;
    std::string invalidPath = "/nonexistent/path/to/invalid.pdf";
    
    EXPECT_THROW({
        loader.getRawPdfData(invalidPath, ReportLoader::ProcessingMode::InMemory);
    }, std::runtime_error) << "Should throw exception for invalid PDF path";
}

TEST(ReportLoaderTest, GetRawPdfData_FileBasedModeCreatesTemporaryFile) {
    TestReportLoader loader;

    // Process in FileBased mode (will use pre-extracted txt fallback on CI)
    EXPECT_NO_THROW({
        loader.getRawPdfData(pdfPath.string(), ReportLoader::ProcessingMode::FileBased);
    }) << "FileBased mode should not throw";
    
    // Verify data was extracted
    auto parseData = loader.convertToJson();
    ASSERT_TRUE(parseData.contains("client")) << "client field should be present after FileBased processing";
    ASSERT_FALSE(parseData["client"].get<std::string>().empty()) << "client should not be empty";
    
    loader.clearRawText();
}

TEST(ReportLoaderTest, GetRawPdfData_ClearRawTextCleansUpResources) {
    TestReportLoader loader;

    // Process in FileBased mode (uses txt fallback if PDF missing)
    loader.getRawPdfData(pdfPath.string(), ReportLoader::ProcessingMode::FileBased);
    auto parseDataBefore = loader.convertToJson();
    ASSERT_TRUE(parseDataBefore.contains("client")) << "Should have data before clear";
    
    // Clear and verify cleanup
    loader.clearRawText();
    
    EXPECT_THROW({
        loader.convertToJson();
    }, std::runtime_error) << "Should throw after clearRawText leaves no data";
}

TEST(ReportLoaderTest, GetRawPdfData_InMemoryModeStoresInMemory) {
    TestReportLoader loader;

    // Process in InMemory mode (uses txt fallback if PDF missing)
    EXPECT_NO_THROW({
        loader.getRawPdfData(pdfPath.string(), ReportLoader::ProcessingMode::InMemory);
    }) << "InMemory mode should not throw";
    
    // Verify data was extracted
    auto parseData = loader.convertToJson();
    ASSERT_TRUE(parseData.contains("client")) << "client field should be present";
    ASSERT_TRUE(parseData.contains("period")) << "period field should be present";
    ASSERT_TRUE(parseData.contains("currency")) << "currency field should be present";
    ASSERT_TRUE(parseData.contains("country")) << "country field should be present";
    
    loader.clearRawText();
}

TEST(ReportLoaderTest, GetRawPdfData_FileBased) {
    ASSERT_TRUE(std::filesystem::exists(expectedJson)) << "Expected JSON file does not exist: " << expectedJson;
    ASSERT_TRUE(std::filesystem::exists(txtPdfData)) << "PDF TXT file does not exist: " << txtPdfData;

    std::ifstream expectedJsonFile(expectedJson);
    ASSERT_TRUE(expectedJsonFile.is_open()) << "Failed to open the expected JSON file: " << expectedJson;

    nlohmann::json expectedParsedJson;
    expectedJsonFile >> expectedParsedJson;
    expectedJsonFile.close();
    
    std::string rawPdfText;
    {
        std::ifstream txtFile(txtPdfData);
        ASSERT_TRUE(txtFile.is_open()) << "Failed to open the PDF TXT file: " << txtPdfData;
        std::stringstream buffer;
        buffer << txtFile.rdbuf();
        rawPdfText = buffer.str();
        txtFile.close();
    }
    
    ReportLoader loader;
    loader.setRawText(rawPdfText);

    auto parseData = loader.convertToJson();

    // std::cout << "Parsed data (FileBased):\n" << parseData.dump(2) << '\n';

    ASSERT_TRUE(parseData.contains("client")) << "client field missing in parsed data";
    ASSERT_FALSE(parseData["client"].get<std::string>().empty()) << "client is empty";

    loader.clearRawText();
}


#if SAVE_GENERATED_TXT_FILES
TEST(ReportLoaderTest, StoreGeneratedJson) {
    // Ensure the input PDF file exists
    ASSERT_TRUE(std::filesystem::exists(pdfPath)) << "Input PDF file does not exist: " << pdfPath;

    try {
        // Process the PDF and generate JSON
        ReportLoader loader;
        loader.getRawPdfData(pdfPath.string(), ReportLoader::ProcessingMode::InMemory);

        // Save raw data to a file
        bool rawDataSaved = loader.saveRawDataToFile(rawDataPath.string());
        ASSERT_TRUE(rawDataSaved) << "Failed to save raw data to file";
        ASSERT_TRUE(std::filesystem::exists(rawDataPath)) << "Raw data file was not created";
        
        const auto parsedJson = loader.convertToJson();
        
        // Write the JSON to a file
        std::ofstream jsonFile(outputJsonPath);
        ASSERT_TRUE(jsonFile.is_open()) << "Failed to open the output JSON file: " << outputJsonPath;
        
        jsonFile << parsedJson.dump(2);  // Pretty-print with 2-space indentation
        jsonFile.close();
        
        // Verify JSON file was written successfully and matches in-memory JSON
        ASSERT_TRUE(std::filesystem::exists(outputJsonPath)) << "JSON output file was not created.";
        std::ifstream verifyFile(outputJsonPath);
        ASSERT_TRUE(verifyFile.is_open()) << "Failed to open the written JSON file: " << outputJsonPath;
        
        nlohmann::json loadedJson;
        verifyFile >> loadedJson;
        verifyFile.close();
        
        ASSERT_EQ(parsedJson, loadedJson) << "In-memory JSON does not match the JSON written to the file.";
        
        // Optional Debug Output
        std::cout << "Generated JSON has been stored at: " << outputJsonPath << std::endl;
    } catch (const std::exception& e) {
        FAIL() << "Exception occurred: " << e.what();
    }
}
#endif

TEST(ReportLoaderTest, IsParsedJsonValid) {
    ASSERT_TRUE(std::filesystem::exists(expectedJson)) << "Expected JSON file does not exist: " << expectedJson;
    ASSERT_TRUE(std::filesystem::exists(txtPdfData)) << "PDF TXT file does not exist: " << txtPdfData;

    std::ifstream expectedJsonFile(expectedJson);
    ASSERT_TRUE(expectedJsonFile.is_open()) << "Failed to open the expected JSON file: " << expectedJson;

    nlohmann::json expectedParsedJson;
    expectedJsonFile >> expectedParsedJson;
    expectedJsonFile.close();
    
    std::string rawPdfText;
    {
        std::ifstream txtFile(txtPdfData);
        ASSERT_TRUE(txtFile.is_open()) << "Failed to open the PDF TXT file: " << txtPdfData;
        std::stringstream buffer;
        buffer << txtFile.rdbuf();
        rawPdfText = buffer.str();
        txtFile.close();
    }
    
    ReportLoader loader;
    loader.setRawText(rawPdfText);

    const auto parsedJson = loader.convertToJson();

    // Convert both JSONs to pretty-formatted strings for line comparison
    const std::string parsedStr   = parsedJson.dump(4);
    const std::string expectedStr = expectedParsedJson.dump(4);

    if (parsedStr != expectedStr) {
        std::istringstream issExpected(expectedStr);
        std::istringstream issParsed(parsedStr);

        std::string lineExpected, lineParsed;
        int lineNo = 1;
        bool diffFound = false;

        while (true) {
            bool eofExpected = !std::getline(issExpected, lineExpected);
            bool eofParsed   = !std::getline(issParsed, lineParsed);

            if (eofExpected && eofParsed) break;

            if (lineExpected != lineParsed) {
                diffFound = true;
                std::cerr << "\n❌ Difference at line " << lineNo << ":\n";
                std::cerr << "Expected: " << lineExpected << "\n";
                std::cerr << "Actual:   " << lineParsed << "\n";
            }
            ++lineNo;
        }

        if (diffFound) {
            ADD_FAILURE() << "Parsed JSON does not match expected JSON. See differences above.";
        }
        } else {
            SUCCEED() << "Parsed JSON matches expected JSON.";
        }
}

TEST(ReportLoaderTest, IncomeSectionCoverage) {
    // Real PDF format with header lines and multiple countries
    std::string testData = "Client: CLIENT001\n";
    testData += "Period: 01.01.2025 - 31.12.2025\n";
    testData += "Currency: EUR\n";
    testData += "Country: TestCountry\n";
    testData += "\n";
    testData += "V. Detailed Income Section\n";
    testData += "\n";
    testData += "ISIN - Name                                          BCCY\n";
    testData += "Value Date   Amount   Exchange Rate   Gross Income   Tax   Net Income\n";
    testData += "Transaction                                          EUR\n";
    testData += "\n";
    testData += "Asset Type: Equities\n";
    testData += "Country: CountryA\n";
    testData += "\n";
    testData += "XX0000000001 - Company A\n";
    testData += "EUR                       1.20               -0.31             0.89\n";
    testData += "Dividend 15.05.2025 0.1234 1.0000\n";
    testData += "EUR                       1.20               -0.31             0.89\n";
    testData += "Total for CountryA\n";
    testData += "\n";
    testData += "EUR                       1.20               -0.31             0.89\n";
    testData += "\n";
    testData += "Total for Equities\n";
    testData += "\n";
    testData += "Asset Type: Equities\n";
    testData += "Country: CountryB\n";
    testData += "\n";
    testData += "YY0000000002 - Company B\n";
    testData += "EUR                       0.63               -0.09             0.54\n";
    testData += "Dividend 20.06.2025 2.5678 1.0000\n";
    testData += "EUR                       0.63               -0.09             0.54\n";
    testData += "Total for CountryB\n";
    testData += "\n";
    testData += "EUR                       0.63               -0.09             0.54\n";
    testData += "\n";
    testData += "Total for Equities\n";
    testData += "\n";
    testData += "VI. Detailed Gains and Losses Section\n";

    ReportLoader loader;
    loader.setRawText(testData);
    const auto parsedJson = loader.convertToJson();
    auto& incomeSections = parsedJson["income_section"];
    
    // Core test: verify multiple countries are parsed correctly
    ASSERT_TRUE(parsedJson.contains("income_section"));
    ASSERT_GE(incomeSections.size(), 2) << "Should have at least 2 Equities sections";
    
    // Find CountryA and CountryB sections
    bool foundCountryA = false, foundCountryB = false;
    for (const auto& sec : incomeSections) {
        if (sec["country"] == "CountryA" && sec["asset_type"] == "Equities") {
            foundCountryA = true;
            ASSERT_EQ(sec["transactions"].size(), 1);
            ASSERT_EQ(sec["transactions"][0]["transaction_type"], "Dividend");
        }
        if (sec["country"] == "CountryB" && sec["asset_type"] == "Equities") {
            foundCountryB = true;
            ASSERT_EQ(sec["transactions"].size(), 1);
            ASSERT_EQ(sec["transactions"][0]["transaction_type"], "Dividend");
        }
    }
    
    ASSERT_TRUE(foundCountryA) << "Should find CountryA section";
    ASSERT_TRUE(foundCountryB) << "Should find CountryB section";
    ASSERT_TRUE(parsedJson.contains("gains_and_losses_section"));
}

TEST(ReportLoaderTest, IncomeSection_LiquidityAndPreviousLineFallback) {
    // This test ensures liquidity DEPOSIT handling and previous-line EUR fallback
    std::string testData;
    testData += "Client: X\n";
    testData += "Period: 01.01.2025 - 31.12.2025\n";
    testData += "Currency: EUR\n";
    testData += "Country: Test\n";
    testData += "\n";
    testData += "V. Detailed Income Section\n";
    testData += "\n";
    testData += "ISIN - Name\n";
    testData += "Transaction                                          EUR\n";
    testData += "\n";
    testData += "Asset Type: Liquidity\n";
    testData += "Country: LiquidityLand\n";
    testData += "\n";
    // place EUR amounts before the Interest line to trigger previous-line fallback
    testData += "EUR 1.234  -0.234 1.000\n";
    testData += "Interest payment 05.02.2025 12.34 1.0000\n";
    testData += "\n";
    testData += "Total for LiquidityLand\n";
    testData += "\n";
    testData += "Total for Liquidity\n";
    testData += "\n";
    testData += "VI. Detailed Gains and Losses Section\n";

    TestReportLoader loader;
    loader.setRawText(testData);
    const auto parsed = loader.convertToJson();
    ASSERT_TRUE(parsed.contains("income_section"));
    const auto& income = parsed["income_section"];
    ASSERT_GE(income.size(), 1);
    bool found = false;
    for (const auto &sec : income) {
        if (sec["country"] == "LiquidityLand") {
            found = true;
            ASSERT_EQ(sec["transactions"].size(), 1);
            const auto &txn = sec["transactions"][0];
            ASSERT_TRUE(txn.contains("DEPOSIT"));
            ASSERT_TRUE(txn["DEPOSIT"].get<bool>());
        }
    }
    ASSERT_TRUE(found);
}

TEST(ReportLoaderTest, GainsAndLosses_UnitPriceParsing) {
    // Tests Trading Buy with explicit EUR detail line
    std::string data;
    data += "V. Detailed Income Section\n"; // ensure header parsing isn't interfering
    data += "VI. Detailed Gains and Losses Section\n";
    data += "Asset Type: Equities\n";
    data += "Country: GainsLand\n";
    data += "ZZ1111111111 - Some Asset\n";
    data += "Trading Buy 10.03.2025 100 1.2000\n";
    data += "EUR 12.34 0.00 0.00 0.00 0.00 9.99\n"; // matches gainsAmountRegex
    data += "Gains EUR 0.00 0.00 0.00\n";

    TestReportLoader loader;
    loader.setRawText(data);
    const auto parsed = loader.convertToJson();
    ASSERT_TRUE(parsed.contains("gains_and_losses_section"));
    const auto& gains = parsed["gains_and_losses_section"];
    ASSERT_GE(gains.size(), 1);
    bool ok = false;
    for (const auto &sec : gains) {
        if (sec["country"] == "GainsLand") {
            ok = true;
            ASSERT_EQ(sec["transactions"].size(), 1);
            ASSERT_TRUE(sec["transactions"][0].contains("unit_price"));
        }
    }
    ASSERT_TRUE(ok);
}

TEST(ReportLoaderTest, WithholdingTax_TotalsAndAmounts) {
    // Tests withholding parsing including totals
    std::string data;
    data += "VII. Detailed Withholding Tax Section\n";
    data += "SomeCountry\n";
    data += "AA222222222 - Issuer\n";
    data += "Dividend 15.04.2025 123.45\n";
    data += "EUR 100.00 10.00 10% 90.00\n";
    data += "EUR 100.00 10.00 10% 90.00\n";
    data += "Total for  200.00 20.00\n";

    TestReportLoader loader;
    loader.setRawText(data);
    const auto parsed = loader.convertToJson();
    ASSERT_TRUE(parsed.contains("withholding_tax_section"));
    const auto& w = parsed["withholding_tax_section"];
    ASSERT_GE(w.size(), 1);
    const auto& section = w[0];
    ASSERT_TRUE(section.contains("totals"));
    ASSERT_TRUE(section["totals"].contains("withholding_tax_amount_in_eur"));
}

TEST(ReportLoaderTest, TransactionHistory_GroupingAndLastIsinCarry) {
    // Ensure transaction grouping by ISIN and carry-over of last ISIN works
    std::string data;
    data += "VIII. History of Transactions and Corporate Actions\n";
    data += "AA333333333 - Some\n";
    data += "Trading Buy 01.01.2025 02.01.2025 EUR 100.00 10.00 1000.00 50.00\n";
    // Simulate next page where ISIN missing but mLastContext should be used
    data += "Trading Buy 03.01.2025 04.01.2025 EUR 200.00 20.00 2000.00 100.00\n";

    TestReportLoader loader;
    loader.setRawText(data);
    const auto parsed = loader.convertToJson();
    ASSERT_TRUE(parsed.contains("transaction_history"));
    const auto& th = parsed["transaction_history"];
    ASSERT_GE(th.size(), 1);
    // There should be at least one group with transactions
    bool found = false;
    for (const auto &g : th) {
        if (g.contains("transactions") && g["transactions"].size() >= 1) {
            found = true;
        }
    }
    ASSERT_TRUE(found);
}