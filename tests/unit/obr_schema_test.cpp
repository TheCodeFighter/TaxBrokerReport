#include "../support/xsd_validation.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {
using taxbroker::test::expectInvalidForObrSchema;
using taxbroker::test::expectValidForObrSchema;

TEST(DohObrSchemaTest, ExampleFileIsValid) {
    // The example file holds one cash-interest row (type 4) and one bond-coupon row (type 2), with
    // every field the form contract writes. Keep it in step with docs/tax_rules.md.
    const std::filesystem::path path = std::filesystem::path{__FILE__}.parent_path().parent_path() /
                                       "test_data" / "obr" / "Doh-Obr_2025_example.xml";
    std::ifstream input{path, std::ios::binary};
    ASSERT_TRUE(input.is_open());

    const std::string xml{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};

    expectValidForObrSchema(xml);
}

TEST(DohObrSchemaTest, DocumentWithoutInterestRowsIsValid) {
    // The schema makes every element optional, so an empty form passes it. The writer, not the
    // schema, must refuse to write a document without data.
    const std::string xml = R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_Obr_2.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
  <edp:Header>
    <edp:taxpayer>
      <edp:taxNumber>12345678</edp:taxNumber>
      <edp:taxpayerType>FO</edp:taxpayerType>
    </edp:taxpayer>
    <edp:Workflow>
      <edp:DocumentWorkflowID>O</edp:DocumentWorkflowID>
      <edp:DocumentWorkflowName>Original</edp:DocumentWorkflowName>
    </edp:Workflow>
  </edp:Header>
  <edp:Signatures />
  <body>
    <edp:bodyContent />
    <Doh_Obr>
      <SelfReport>false</SelfReport>
      <Period>2025</Period>
      <DocumentWorkflowID>O</DocumentWorkflowID>
      <ResidentOfRepublicOfSlovenia>true</ResidentOfRepublicOfSlovenia>
      <Country>SI</Country>
    </Doh_Obr>
  </body>
</Envelope>
)xml";

    expectValidForObrSchema(xml);
}

TEST(DohObrSchemaTest, ValueOfZeroPassesTheSchema) {
    // The instructions say the interest must be greater than zero, the schema accepts 0.00.
    // The writer must therefore skip such a row.
    const std::string xml = R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_Obr_2.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
  <edp:Header>
    <edp:taxpayer>
      <edp:taxNumber>12345678</edp:taxNumber>
      <edp:taxpayerType>FO</edp:taxpayerType>
    </edp:taxpayer>
    <edp:Workflow>
      <edp:DocumentWorkflowID>O</edp:DocumentWorkflowID>
      <edp:DocumentWorkflowName>Original</edp:DocumentWorkflowName>
    </edp:Workflow>
  </edp:Header>
  <edp:Signatures />
  <body>
    <edp:bodyContent />
    <Doh_Obr>
      <SelfReport>false</SelfReport>
      <Period>2025</Period>
      <DocumentWorkflowID>O</DocumentWorkflowID>
      <ResidentOfRepublicOfSlovenia>true</ResidentOfRepublicOfSlovenia>
      <Country>SI</Country>
      <Interest>
        <Date>2025-03-31</Date>
        <IdentificationNumber>DE000000000</IdentificationNumber>
        <Name>Example Broker AG</Name>
        <Address>Example Street 1, 10115 Berlin</Address>
        <Country>DE</Country>
        <Type>4</Type>
        <Value>0.00</Value>
        <ForeignTax>0</ForeignTax>
        <Country2>DE</Country2>
      </Interest>
    </Doh_Obr>
  </body>
</Envelope>
)xml";

    expectValidForObrSchema(xml);
}

TEST(DohObrSchemaTest, ValueWithTwelveIntegerDigitsIsValid) {
    // 999999999999.99: 12 digits before the point and 2 after it, the largest Decimal12_2Type.
    const std::string xml = R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_Obr_2.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
  <edp:Header>
    <edp:taxpayer>
      <edp:taxNumber>12345678</edp:taxNumber>
      <edp:taxpayerType>FO</edp:taxpayerType>
    </edp:taxpayer>
    <edp:Workflow>
      <edp:DocumentWorkflowID>O</edp:DocumentWorkflowID>
      <edp:DocumentWorkflowName>Original</edp:DocumentWorkflowName>
    </edp:Workflow>
  </edp:Header>
  <edp:Signatures />
  <body>
    <edp:bodyContent />
    <Doh_Obr>
      <SelfReport>false</SelfReport>
      <Period>2025</Period>
      <DocumentWorkflowID>O</DocumentWorkflowID>
      <ResidentOfRepublicOfSlovenia>true</ResidentOfRepublicOfSlovenia>
      <Country>SI</Country>
      <Interest>
        <Date>2025-03-31</Date>
        <IdentificationNumber>DE000000000</IdentificationNumber>
        <Name>Example Broker AG</Name>
        <Address>Example Street 1, 10115 Berlin</Address>
        <Country>DE</Country>
        <Type>4</Type>
        <Value>999999999999.99</Value>
        <ForeignTax>0</ForeignTax>
        <Country2>DE</Country2>
      </Interest>
    </Doh_Obr>
  </body>
</Envelope>
)xml";

    expectValidForObrSchema(xml);
}

TEST(DohObrSchemaTest, ValueWithThirteenIntegerDigitsIsInvalid) {
    // 1000000000000.00 has 13 digits before the point.
    const std::string xml = R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_Obr_2.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
  <edp:Header>
    <edp:taxpayer>
      <edp:taxNumber>12345678</edp:taxNumber>
      <edp:taxpayerType>FO</edp:taxpayerType>
    </edp:taxpayer>
    <edp:Workflow>
      <edp:DocumentWorkflowID>O</edp:DocumentWorkflowID>
      <edp:DocumentWorkflowName>Original</edp:DocumentWorkflowName>
    </edp:Workflow>
  </edp:Header>
  <edp:Signatures />
  <body>
    <edp:bodyContent />
    <Doh_Obr>
      <SelfReport>false</SelfReport>
      <Period>2025</Period>
      <DocumentWorkflowID>O</DocumentWorkflowID>
      <ResidentOfRepublicOfSlovenia>true</ResidentOfRepublicOfSlovenia>
      <Country>SI</Country>
      <Interest>
        <Date>2025-03-31</Date>
        <IdentificationNumber>DE000000000</IdentificationNumber>
        <Name>Example Broker AG</Name>
        <Address>Example Street 1, 10115 Berlin</Address>
        <Country>DE</Country>
        <Type>4</Type>
        <Value>1000000000000.00</Value>
        <ForeignTax>0</ForeignTax>
        <Country2>DE</Country2>
      </Interest>
    </Doh_Obr>
  </body>
</Envelope>
)xml";

    expectInvalidForObrSchema(xml);
}

TEST(DohObrSchemaTest, NegativeValueIsInvalid) {
    // The type allows digits only, so -12.34 is rejected.
    const std::string xml = R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_Obr_2.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
  <edp:Header>
    <edp:taxpayer>
      <edp:taxNumber>12345678</edp:taxNumber>
      <edp:taxpayerType>FO</edp:taxpayerType>
    </edp:taxpayer>
    <edp:Workflow>
      <edp:DocumentWorkflowID>O</edp:DocumentWorkflowID>
      <edp:DocumentWorkflowName>Original</edp:DocumentWorkflowName>
    </edp:Workflow>
  </edp:Header>
  <edp:Signatures />
  <body>
    <edp:bodyContent />
    <Doh_Obr>
      <SelfReport>false</SelfReport>
      <Period>2025</Period>
      <DocumentWorkflowID>O</DocumentWorkflowID>
      <ResidentOfRepublicOfSlovenia>true</ResidentOfRepublicOfSlovenia>
      <Country>SI</Country>
      <Interest>
        <Date>2025-03-31</Date>
        <IdentificationNumber>DE000000000</IdentificationNumber>
        <Name>Example Broker AG</Name>
        <Address>Example Street 1, 10115 Berlin</Address>
        <Country>DE</Country>
        <Type>4</Type>
        <Value>-12.34</Value>
        <ForeignTax>0</ForeignTax>
        <Country2>DE</Country2>
      </Interest>
    </Doh_Obr>
  </body>
</Envelope>
)xml";

    expectInvalidForObrSchema(xml);
}

TEST(DohObrSchemaTest, ValueWithThreeDecimalsIsInvalid) {
    // 12.345 has 3 decimals, the type allows at most 2.
    const std::string xml = R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_Obr_2.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
  <edp:Header>
    <edp:taxpayer>
      <edp:taxNumber>12345678</edp:taxNumber>
      <edp:taxpayerType>FO</edp:taxpayerType>
    </edp:taxpayer>
    <edp:Workflow>
      <edp:DocumentWorkflowID>O</edp:DocumentWorkflowID>
      <edp:DocumentWorkflowName>Original</edp:DocumentWorkflowName>
    </edp:Workflow>
  </edp:Header>
  <edp:Signatures />
  <body>
    <edp:bodyContent />
    <Doh_Obr>
      <SelfReport>false</SelfReport>
      <Period>2025</Period>
      <DocumentWorkflowID>O</DocumentWorkflowID>
      <ResidentOfRepublicOfSlovenia>true</ResidentOfRepublicOfSlovenia>
      <Country>SI</Country>
      <Interest>
        <Date>2025-03-31</Date>
        <IdentificationNumber>DE000000000</IdentificationNumber>
        <Name>Example Broker AG</Name>
        <Address>Example Street 1, 10115 Berlin</Address>
        <Country>DE</Country>
        <Type>4</Type>
        <Value>12.345</Value>
        <ForeignTax>0</ForeignTax>
        <Country2>DE</Country2>
      </Interest>
    </Doh_Obr>
  </body>
</Envelope>
)xml";

    expectInvalidForObrSchema(xml);
}

TEST(DohObrSchemaTest, NegativeForeignTaxIsInvalid) {
    // ForeignTax uses the same type as Value, so -3.75 is rejected.
    const std::string xml = R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_Obr_2.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
  <edp:Header>
    <edp:taxpayer>
      <edp:taxNumber>12345678</edp:taxNumber>
      <edp:taxpayerType>FO</edp:taxpayerType>
    </edp:taxpayer>
    <edp:Workflow>
      <edp:DocumentWorkflowID>O</edp:DocumentWorkflowID>
      <edp:DocumentWorkflowName>Original</edp:DocumentWorkflowName>
    </edp:Workflow>
  </edp:Header>
  <edp:Signatures />
  <body>
    <edp:bodyContent />
    <Doh_Obr>
      <SelfReport>false</SelfReport>
      <Period>2025</Period>
      <DocumentWorkflowID>O</DocumentWorkflowID>
      <ResidentOfRepublicOfSlovenia>true</ResidentOfRepublicOfSlovenia>
      <Country>SI</Country>
      <Interest>
        <Date>2025-06-15</Date>
        <IdentificationNumber>FR0000000000</IdentificationNumber>
        <Name>Example Issuer SA</Name>
        <Address>1 Rue Exemple, 75001 Paris</Address>
        <Country>FR</Country>
        <Type>2</Type>
        <Value>25.00</Value>
        <ForeignTax>-3.75</ForeignTax>
        <Country2>FR</Country2>
      </Interest>
    </Doh_Obr>
  </body>
</Envelope>
)xml";

    expectInvalidForObrSchema(xml);
}

TEST(DohObrSchemaTest, IdentificationNumberOfThirtyCharactersIsValid) {
    // 30 characters: the longest allowed payer identification number.
    const std::string xml = R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_Obr_2.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
  <edp:Header>
    <edp:taxpayer>
      <edp:taxNumber>12345678</edp:taxNumber>
      <edp:taxpayerType>FO</edp:taxpayerType>
    </edp:taxpayer>
    <edp:Workflow>
      <edp:DocumentWorkflowID>O</edp:DocumentWorkflowID>
      <edp:DocumentWorkflowName>Original</edp:DocumentWorkflowName>
    </edp:Workflow>
  </edp:Header>
  <edp:Signatures />
  <body>
    <edp:bodyContent />
    <Doh_Obr>
      <SelfReport>false</SelfReport>
      <Period>2025</Period>
      <DocumentWorkflowID>O</DocumentWorkflowID>
      <ResidentOfRepublicOfSlovenia>true</ResidentOfRepublicOfSlovenia>
      <Country>SI</Country>
      <Interest>
        <Date>2025-03-31</Date>
        <IdentificationNumber>AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA</IdentificationNumber>
        <Name>Example Broker AG</Name>
        <Address>Example Street 1, 10115 Berlin</Address>
        <Country>DE</Country>
        <Type>4</Type>
        <Value>12.34</Value>
        <ForeignTax>0</ForeignTax>
        <Country2>DE</Country2>
      </Interest>
    </Doh_Obr>
  </body>
</Envelope>
)xml";

    expectValidForObrSchema(xml);
}

TEST(DohObrSchemaTest, IdentificationNumberOfThirtyOneCharactersIsInvalid) {
    // 31 characters: one more than the schema allows.
    const std::string xml = R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_Obr_2.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
  <edp:Header>
    <edp:taxpayer>
      <edp:taxNumber>12345678</edp:taxNumber>
      <edp:taxpayerType>FO</edp:taxpayerType>
    </edp:taxpayer>
    <edp:Workflow>
      <edp:DocumentWorkflowID>O</edp:DocumentWorkflowID>
      <edp:DocumentWorkflowName>Original</edp:DocumentWorkflowName>
    </edp:Workflow>
  </edp:Header>
  <edp:Signatures />
  <body>
    <edp:bodyContent />
    <Doh_Obr>
      <SelfReport>false</SelfReport>
      <Period>2025</Period>
      <DocumentWorkflowID>O</DocumentWorkflowID>
      <ResidentOfRepublicOfSlovenia>true</ResidentOfRepublicOfSlovenia>
      <Country>SI</Country>
      <Interest>
        <Date>2025-03-31</Date>
        <IdentificationNumber>AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA</IdentificationNumber>
        <Name>Example Broker AG</Name>
        <Address>Example Street 1, 10115 Berlin</Address>
        <Country>DE</Country>
        <Type>4</Type>
        <Value>12.34</Value>
        <ForeignTax>0</ForeignTax>
        <Country2>DE</Country2>
      </Interest>
    </Doh_Obr>
  </body>
</Envelope>
)xml";

    expectInvalidForObrSchema(xml);
}

TEST(DohObrSchemaTest, InterestFieldsOutOfOrderAreInvalid) {
    // Value comes before Type here; the schema is a sequence in the order of the instructions.
    const std::string xml = R"xml(<?xml version="1.0" encoding="UTF-8"?>
<Envelope xmlns="http://edavki.durs.si/Documents/Schemas/Doh_Obr_2.xsd" xmlns:edp="http://edavki.durs.si/Documents/Schemas/EDP-Common-1.xsd">
  <edp:Header>
    <edp:taxpayer>
      <edp:taxNumber>12345678</edp:taxNumber>
      <edp:taxpayerType>FO</edp:taxpayerType>
    </edp:taxpayer>
    <edp:Workflow>
      <edp:DocumentWorkflowID>O</edp:DocumentWorkflowID>
      <edp:DocumentWorkflowName>Original</edp:DocumentWorkflowName>
    </edp:Workflow>
  </edp:Header>
  <edp:Signatures />
  <body>
    <edp:bodyContent />
    <Doh_Obr>
      <SelfReport>false</SelfReport>
      <Period>2025</Period>
      <DocumentWorkflowID>O</DocumentWorkflowID>
      <ResidentOfRepublicOfSlovenia>true</ResidentOfRepublicOfSlovenia>
      <Country>SI</Country>
      <Interest>
        <Date>2025-03-31</Date>
        <IdentificationNumber>DE000000000</IdentificationNumber>
        <Name>Example Broker AG</Name>
        <Address>Example Street 1, 10115 Berlin</Address>
        <Country>DE</Country>
        <Value>12.34</Value>
        <Type>4</Type>
        <ForeignTax>0</ForeignTax>
        <Country2>DE</Country2>
      </Interest>
    </Doh_Obr>
  </body>
</Envelope>
)xml";

    expectInvalidForObrSchema(xml);
}

} // namespace
