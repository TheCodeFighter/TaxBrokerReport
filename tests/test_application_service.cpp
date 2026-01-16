#include <gtest/gtest.h>
#include "application_service.hpp"
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

class ApplicationServiceApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_root = PROJECT_SOURCE_DIR;
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

    fs::path m_root, m_testOutputDir, m_mockTxtPath;
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
    request.formType = TaxFormType::Doh_KDVP;

    auto result = service.processRequest(request, loader);

    ASSERT_TRUE(result.success);
    EXPECT_TRUE(fs::exists(m_testOutputDir / "Doh_KDVP.xml"));
}