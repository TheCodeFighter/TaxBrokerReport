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
    request.jsonOnly = true;
    request.formType = TaxFormType::Doh_KDVP;
    request.year = 2024;

    auto result = service.processRequest(request, loader);

    ASSERT_TRUE(result.success);
    EXPECT_TRUE(fs::exists(m_testOutputDir / "intermediate_data.json"));
    EXPECT_FALSE(fs::exists(m_testOutputDir / "Doh_KDVP.xml"));
}

TEST_F(ApplicationServiceApiTest, ShouldGenerateFullXml) {
    ReportLoader loader;
    loader.setRawText(m_rawTextContent);

    ApplicationService service;
    GenerationRequest request;
    request.outputDirectory = m_testOutputDir;
    request.taxNumber = "12345678";
    request.year = 2024;
    request.formType = TaxFormType::Doh_KDVP;

    auto result = service.processRequest(request, loader);

    ASSERT_TRUE(result.success);
    EXPECT_TRUE(fs::exists(m_testOutputDir / "Doh_KDVP.xml"));
}

#if TEST_ALL_API

std::string TAX_NUM = "12345678";

TEST_F(ApplicationServiceApiTest, ApiKDVP) {
    ApplicationService service;

    GenerationRequest request;
    
    request.outputDirectory = m_root / "tmp" / "api_test";
    request.inputPdf = m_root / "tmp" / "TaxReport2024.pdf";
    request.outputDirectory = m_root / "tmp" / "api_test_output";
    request.formType = TaxFormType::Doh_KDVP;
    request.email = "john@example.com";
    request.phone = "0123456789";
    request.year = 2025;

    request.taxNumber = TAX_NUM;
    
    auto result = service.processRequest(request);
    
    ASSERT_TRUE(result.success);
    EXPECT_TRUE(fs::exists(request.outputDirectory / "Doh_KDVP.xml"));
}

TEST_F(ApplicationServiceApiTest, ApiDIV) {
    ApplicationService service;

    GenerationRequest request;
    
    request.outputDirectory = m_root / "tmp" / "api_test";
    request.inputPdf = m_root / "tmp" / "TaxReport2024.pdf";
    request.outputDirectory = m_root / "tmp" / "api_test_output";
    request.formType = TaxFormType::Doh_DIV;
    request.email = "john@example.com";
    request.phone = "0123456789";
    request.year = 2025;

    request.taxNumber = TAX_NUM;
    
    auto result = service.processRequest(request);
    
    ASSERT_TRUE(result.success);
    EXPECT_TRUE(fs::exists(request.outputDirectory / "Doh_DIV.xml"));
}

TEST_F(ApplicationServiceApiTest, ApiDHO) {
    ApplicationService service;

    GenerationRequest request;
    
    request.outputDirectory = m_root / "tmp" / "api_test";
    request.inputPdf = m_root / "tmp" / "TaxReport2024.pdf";
    request.outputDirectory = m_root / "tmp" / "api_test_output";
    request.formType = TaxFormType::Doh_DHO;
    request.email = "john@example.com";
    request.phone = "0123456789";
    request.year = 2025;

    request.taxNumber = TAX_NUM;
    
    auto result = service.processRequest(request);
    
    ASSERT_TRUE(result.success);
    EXPECT_TRUE(fs::exists(request.outputDirectory / "Doh_DIV.xml"));
}

#endif