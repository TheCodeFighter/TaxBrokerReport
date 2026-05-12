#include <gtest/gtest.h>
#include <fstream>
#include <filesystem>

#include "application_service.hpp"

namespace fs = std::filesystem;

fs::path m_root = PROJECT_SOURCE_DIR;

#ifndef TEST_ALL_API
    #define TEST_ALL_API 0
#endif

class ApplicationServiceApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_testOutputDir = m_root / "tests" / "output";
        m_mockTxtPath = m_root / "tests" / "testData" / "generated_test_data.txt";
        
        if (fs::exists(m_testOutputDir)) fs::remove_all(m_testOutputDir);
        fs::create_directories(m_testOutputDir);

        std::ifstream ifs(m_mockTxtPath);
        m_rawTextContent = std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    }

    // Best Practice: Clean up all generated files automatically after each test
    void TearDown() override {
        if (fs::exists(m_testOutputDir)) {
            fs::remove_all(m_testOutputDir);
        }
    }

    fs::path m_testOutputDir, m_mockTxtPath;
    std::string m_rawTextContent;
};

TEST_F(ApplicationServiceApiTest, ShouldGenerateJsonOnly) {
    ReportLoader loader;
    loader.setRawText(m_rawTextContent);

    ApplicationService service;
    GenerationRequest request;
    request.outputDirectory = m_testOutputDir;
    // FIX: rename inputPdf -> inputFile
    request.inputFile = m_mockTxtPath; 
    request.jsonOnly = true;

    auto result = service.processRequest(request, loader);
    
    ASSERT_TRUE(result.success) << "Error: " << result.message; // Adding message helps debugging!
    EXPECT_TRUE(fs::exists(m_testOutputDir / "intermediate_data.json"));
}

TEST_F(ApplicationServiceApiTest, ShouldGenerateFullXml) {
    ReportLoader loader;
    loader.setRawText(m_rawTextContent);

    ApplicationService service;
    GenerationRequest request;
    request.outputDirectory = m_testOutputDir;
    // FIX: rename inputPdf -> inputFile
    request.inputFile = m_mockTxtPath; 
    request.taxNumber = "12345678";
    request.year = 2024;
    request.formType = TaxFormType::Doh_KDVP;
    request.jsonOnly = false;

    auto result = service.processRequest(request, loader);
    
    ASSERT_TRUE(result.success) << "Error: " << result.message;
    EXPECT_TRUE(fs::exists(m_testOutputDir / "Doh_KDVP.xml"));
}

TEST_F(ApplicationServiceApiTest, UnsupportedExtension) {
    // Create a dummy file with unsupported extension
    fs::path badFile = m_testOutputDir / "dummy.bin";
    {
        std::ofstream ofs(badFile);
        ofs << "binarydata";
    }

    ReportLoader loader;

    ApplicationService service;
    GenerationRequest request;
    request.outputDirectory = m_testOutputDir;
    request.inputFile = badFile;

    auto result = service.processRequest(request, loader);
    ASSERT_FALSE(result.success);
    ASSERT_NE(result.message.find("Unsupported file format"), std::string::npos);
}

TEST_F(ApplicationServiceApiTest, FileDoesNotExist) {
    ReportLoader loader;
    ApplicationService service;
    GenerationRequest request;
    request.outputDirectory = m_testOutputDir;
    request.inputFile = m_testOutputDir / "this_file_should_not_exist.txt";

    // Ensure file is absent
    if (fs::exists(request.inputFile)) fs::remove(request.inputFile);

    auto result = service.processRequest(request, loader);
    ASSERT_FALSE(result.success);
    ASSERT_NE(result.message.find("File does not exist"), std::string::npos);
}

TEST_F(ApplicationServiceApiTest, JsonInvalidStructure) {
    // Create a JSON file that is missing required sections
    fs::path badJson = m_testOutputDir / "bad.json";
    {
        std::ofstream ofs(badJson);
        ofs << "{ \"some\": \"value\" }";
    }

    ReportLoader loader;
    ApplicationService service;
    GenerationRequest request;
    request.outputDirectory = m_testOutputDir;
    request.inputFile = badJson;

    auto result = service.processRequest(request, loader);
    ASSERT_FALSE(result.success);
    ASSERT_NE(result.message.find("Invalid JSON structure"), std::string::npos);
}

TEST_F(ApplicationServiceApiTest, ShouldGenerateDivXml) {
    ReportLoader loader;
    loader.setRawText(m_rawTextContent);

    ApplicationService service;
    GenerationRequest request;
    request.outputDirectory = m_testOutputDir;
    request.inputFile = m_mockTxtPath;
    request.taxNumber = "12345678";
    request.year = 2024;
    request.formType = TaxFormType::Doh_DIV;
    request.jsonOnly = false;

    auto result = service.processRequest(request, loader);
    ASSERT_TRUE(result.success) << "Error: " << result.message;
    EXPECT_TRUE(fs::exists(m_testOutputDir / "Doh_DIV.xml"));
}

TEST_F(ApplicationServiceApiTest, ShouldGenerateDhoXml) {
    ReportLoader loader;
    loader.setRawText(m_rawTextContent);

    ApplicationService service;
    GenerationRequest request;
    request.outputDirectory = m_testOutputDir;
    request.inputFile = m_mockTxtPath;
    request.taxNumber = "12345678";
    request.year = 2024;
    request.formType = TaxFormType::Doh_DHO;
    request.jsonOnly = false;

    auto result = service.processRequest(request, loader);
    ASSERT_TRUE(result.success) << "Error: " << result.message;
    EXPECT_TRUE(fs::exists(m_testOutputDir / "Doh_DHO.xml"));
}

TEST_F(ApplicationServiceApiTest, JsonInput_JsonOnly) {
    // Use the existing expected JSON from testData
    fs::path jsonFile = m_root / "tests" / "testData" / "expected_test_output.json";
    ASSERT_TRUE(fs::exists(jsonFile));

    ApplicationService service;
    GenerationRequest request;
    request.outputDirectory = m_testOutputDir;
    request.inputFile = jsonFile;
    request.jsonOnly = false;
    request.formType = TaxFormType::Doh_KDVP;
    request.taxNumber = "999";
    request.year = 2024;

    auto result = service.processRequest(request);
    ASSERT_TRUE(result.success) << "Error: " << result.message;
    EXPECT_TRUE(fs::exists(m_testOutputDir / "Doh_KDVP.xml"));
}

#if TEST_ALL_API

class ApplicationApiTest : public ApplicationServiceApiTest {
protected:
    void SetUp() override {
        ApplicationServiceApiTest::SetUp();
        
        request.email = "john@example.com";
        request.phone = "0123456789";
        request.year = 2025;
        request.outputDirectory = m_root / "tmp" / "api_test_output";
        request.taxNumber = "12345678";
        request.inputFile = m_root / "tmp" / "TaxReport2024.pdf";
        request.formDocType = FormType::Original;
        
        if (!fs::exists(request.outputDirectory)) {
            fs::create_directories(request.outputDirectory);
        }
    }

    ApplicationService service;
    GenerationRequest request;
};

TEST_F(ApplicationApiTest, ApiKDVP) {
    request.formType = TaxFormType::Doh_KDVP;
    auto result = service.processRequest(request);
    
    ASSERT_TRUE(result.success);
    EXPECT_TRUE(fs::exists(request.outputDirectory / "Doh_KDVP.xml"));
}

TEST_F(ApplicationApiTest, ApiDIV) {
    request.formType = TaxFormType::Doh_DIV;
    auto result = service.processRequest(request);
    
    ASSERT_TRUE(result.success);
    EXPECT_TRUE(fs::exists(request.outputDirectory / "Doh_DIV.xml"));
}

TEST_F(ApplicationApiTest, ApiDHO) {
    request.formType = TaxFormType::Doh_DHO;
    auto result = service.processRequest(request);
    
    ASSERT_TRUE(result.success);
    EXPECT_TRUE(fs::exists(request.outputDirectory / "Doh_DHO.xml"));
}

#endif