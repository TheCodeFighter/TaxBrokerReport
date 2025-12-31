#include <gtest/gtest.h>
#include <pugixml.hpp>
#include <filesystem>
#include <iostream>
#include <fstream>

#include "xml_generator.hpp"
#include "helper.hpp"
#include "util_xml.hpp"


#ifndef SAVE_GENERATED_XML_FILES
    #define SAVE_GENERATED_XML_FILES 0
#endif

#ifndef XML_VALIDATION
    #define XML_VALIDATION 0
#endif


// Paths for files
const std::filesystem::path projectRoot {PROJECT_SOURCE_DIR};
const std::filesystem::path jsonTmpPath = projectRoot / "tmp" / "generated_test_output.json";
const std::filesystem::path jsonPath = projectRoot / "tests" / "testData" / "expected_test_output.json";
const std::filesystem::path xmlPath = projectRoot / "tmp" / "generated_test_output.xml";
const std::filesystem::path xsdDoh_KDVP_Path = projectRoot / "resources" / "xml" / "edavk" / "schemas" / "Doh_KDVP_9.xsd";
const std::filesystem::path logXmlValidationPath = projectRoot / "tmp" / "xml_validation_log.txt";


pugi::xml_document generateDummyXml(void) {
    DohKDVP_Data data;
    
    TaxPayer tp {
        .taxNumber = "12345678",
        .resident  = data.IsResident
    };
    
    data.Year = 2025;
    data.IsResident = true;

    KDVPItem item;
    item.Type = InventoryListType::PLVP;
    item.Securities = SecuritiesPLVP{};
    item.Securities->Name = "DummySecurity";  // assign individually

    data.Items.push_back(item);

    // Generate XML document
    pugi::xml_document doc = XmlGenerator::generate_envelope(data, tp);

    return doc;
}

TEST(XmlGeneratorTest, GenerateDummyXmlInMemory) {
    auto doc = generateDummyXml();

    // Check that the document is not empty
    auto root = doc.first_child();
    ASSERT_TRUE(root) << "Generated XML is empty";

    // Optionally print XML for debugging
    // doc.save(std::cout);
}

#if SAVE_GENERATED_XML_FILES
TEST(XmlGeneratorTest, StoreGeneratedXMl) {
    // Ensure file exist
    // TODO: integrate with real data
    ASSERT_TRUE(std::filesystem::exists(jsonTmpPath)) << "Input file does not exist: " << jsonTmpPath;

    try {
        auto doc = generateDummyXml();
        ASSERT_TRUE(doc.save_file(xmlPath.c_str()));

    } catch (const std::exception& e) {
        FAIL() << "Exception occured: " << e.what();
    }
}
#endif

#if XML_VALIDATION
TEST(XmlGeneratorTest, ValidateGeneratedXmlAgainstXsd) {
    // Ensure XML file exist
    ASSERT_TRUE(std::filesystem::exists(xmlPath)) << "XML file does not exist: " << xmlPath;

    // XSD path
    ASSERT_TRUE(std::filesystem::exists(xsdDoh_KDVP_Path)) << "XSD file does not exist: " << xsdDoh_KDVP_Path;

    // Validate
    bool isValid = validateXml(xmlPath.c_str(), xsdDoh_KDVP_Path.c_str(), logXmlValidationPath, true);
    ASSERT_TRUE(isValid) << "Generated XML does not conform to XSD schema";
}
#endif

TEST(XmlGenerator, ParseJson) {
    ASSERT_TRUE(std::filesystem::exists(jsonPath)) << "JSON file does not exist: " << jsonPath;

    // Read and parse JSON
    std::ifstream json_file(jsonPath.string());
    nlohmann::json json_data;
    json_file >> json_data;

    std::map<std::string, std::vector<Transaction>> transactions;

    XmlGenerator::parse_json(transactions, TransactionType::Funds, json_data);

    ASSERT_FALSE(transactions.empty()) << "No transactions parsed";

    ASSERT_EQ(transactions.size(), 1);

    ASSERT_TRUE(transactions.contains("AE0000000001"));

    ASSERT_EQ(transactions["AE0000000001"][2].date, "2024-08-09");
    ASSERT_EQ(transactions["AE0000000001"][2].type, "Trading Sell");
    ASSERT_EQ(transactions["AE0000000001"][2].quantity, 0.1099);
}
