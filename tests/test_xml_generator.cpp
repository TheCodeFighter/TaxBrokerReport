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
const std::filesystem::path xsdDoh_Div_Path = projectRoot / "resources" / "xml" / "edavk" / "schemas" / "Doh_Div_3.xsd";
const std::filesystem::path logXmlValidationPath = projectRoot / "tmp" / "xml_validation_log.txt";


pugi::xml_document generateDummyXml(void) {
    DohKDVP_Data data;
    
    TaxPayer tp {
        .mTaxNumber = "12345678",
        .mType = "FO",
        .mResident  = data.mIsResident
    };
    
    data.mYear = 2025;
    data.mIsResident = true;
    data.mDocID = DocWorkflowID::Original;

    KDVPItem item;
    item.mType = InventoryListType::PLVP;
    item.mSecurities = SecuritiesPLVP{};
    item.mSecurities->mName = "DummySecurity";  // assign individually

    data.mItems.push_back(item);

    // Generate XML document
    auto generator = XmlGenerator{};
    pugi::xml_document doc = generator.generate_doh_kdvp_xml(data, tp);

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

    xmlDocPtr doc = xmlReadFile(xmlPath.c_str(), nullptr, 0);

    // Validate
    bool isValid = validateXml(doc, xsdDoh_KDVP_Path.c_str(), logXmlValidationPath);
    ASSERT_TRUE(isValid) << "Generated XML does not conform to XSD schema";
}
#endif

TEST(XmlGenerator, ParseJson) {
    ASSERT_TRUE(std::filesystem::exists(jsonPath)) << "JSON file does not exist: " << jsonPath;

    // Read and parse JSON
    std::ifstream json_file(jsonPath.string());
    nlohmann::json json_data;
    json_file >> json_data;

    Transactions transactions;

    XmlGenerator::parse_json(transactions, TransactionType::Funds, json_data);

    auto& tx = transactions.mGains;

    ASSERT_FALSE(tx.empty()) << "No transactions parsed";

    ASSERT_EQ(transactions.mGains.size(), 1);

    ASSERT_TRUE(transactions.mGains.contains("AE0000000001"));
    ASSERT_EQ(transactions.mGains["AE0000000001"][2].mDate, "2024-08-09");
    ASSERT_EQ(transactions.mGains["AE0000000001"][2].mType, "Trading Sell");
    ASSERT_EQ(transactions.mGains["AE0000000001"][2].mQuantity, 0.1099);
}

Transactions transactions;

TEST(XmlGenerator, PreTest) {
    ASSERT_TRUE(std::filesystem::exists(jsonPath)) << "JSON file does not exist: " << jsonPath;

    // Read and parse JSON
    std::ifstream json_file(jsonPath.string());
    nlohmann::json json_data;
    json_file >> json_data;


    XmlGenerator::parse_json(transactions, TransactionType::Equities, json_data);

    ASSERT_FALSE(transactions.mGains.empty()) << "No transactions parsed";
}

//// This will be selected by user in gui or cli
FormData formData{
    .mDocID = DocWorkflowID::Original,
    .mYear = 2025,
    .mIsResident = true,
    .mTelephoneNumber = std::optional<std::string>("012345678"),
    .mEmail = std::optional<std::string>("jon@test.si")
};

// minimal working data
TaxPayer taxPayer{
    .mTaxNumber = "12345678",
    .mType = "FO",
    .mResident = true
    // .mTaxPayerName = "John Doe";
    // .mAddress1 = "123 Main Street";
    // .mCity = "Ljubljana";
    // .mPostNumber = "1000";
    // .mPostName = "Ljubljana";
    // .mBirthDate = "1990-01-01";
};
//// end of user input

// Capital gains test
TEST(XmlGenerator, GenerateKdvpXml) {   
    DohKDVP_Data data = XmlGenerator::prepare_kdvp_data(transactions.mGains, formData);

    // Generate XML
    auto generator = XmlGenerator{};
    pugi::xml_document doc = generator.generate_doh_kdvp_xml(data, taxPayer);

    auto libxmlDoc = convertPugiToLibxml(doc);
    ASSERT_TRUE(libxmlDoc);

    bool isValid = validateXml(libxmlDoc.get(), xsdDoh_KDVP_Path);
    ASSERT_TRUE(isValid) << "Generated XML document does not conform to XSD schema";

#if SAVE_GENERATED_XML_FILES
    std::filesystem::path output_xml = projectRoot / "tmp" / "test.xml";
    ASSERT_TRUE(doc.save_file(output_xml.c_str())) << "Failed to save generated XML";
#endif
}

// Dividend test
TEST(XmlGenerator, GenerateDivXml) {
    DohDiv_Data data = XmlGenerator::prepare_div_data(transactions.mDividends, formData);

    // Generate XML
    auto generator = XmlGenerator{};
    pugi::xml_document doc = generator.generate_doh_div_xml(data, taxPayer);

    auto libxmlDoc = convertPugiToLibxml(doc);
    ASSERT_TRUE(libxmlDoc);

    bool isValid = validateXml(libxmlDoc.get(), xsdDoh_Div_Path);
    ASSERT_TRUE(isValid) << "Generated XML document does not conform to XSD schema";

#if SAVE_GENERATED_XML_FILES
    std::filesystem::path output_xml = projectRoot / "tmp" / "test.xml";
    ASSERT_TRUE(doc.save_file(output_xml.c_str())) << "Failed to save generated XML";
#endif
}

