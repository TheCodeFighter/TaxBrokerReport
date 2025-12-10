#include <gtest/gtest.h>
#include <xml_generator.hpp>
#include <pugixml.hpp>

using namespace edavki::doh_kdvp;

TEST(XmlGeneratorTest, GenerateDummyXmlInMemory) {
    // Minimal dummy data
    DohKDVP_Data data;
    data.Year = 2025;
    data.IsResident = true;

    KDVPItem item;
    item.Type = InventoryListType::PLVP;
    item.Securities = SecuritiesPLVP{};
    item.Securities->Name = "DummySecurity";  // assign individually

    data.Items.push_back(item);

    // Generate XML document
    pugi::xml_document doc = XmlGenerator::generate_envelope(data);

    // Check that the document is not empty
    auto root = doc.first_child();
    ASSERT_TRUE(root) << "Generated XML is empty";

    // Optionally print XML for debugging
    // doc.save(std::cout);
}
