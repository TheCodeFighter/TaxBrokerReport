#include <report_loader.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <iostream>

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