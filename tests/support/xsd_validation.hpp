#pragma once

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>

#ifdef TAXBROKER_HAS_LIBXML2
#include <libxml/parser.h>
#include <libxml/xmlschemas.h>

#include <cstdarg>
#include <cstdio>
#endif

namespace taxbroker::test {

/// Both copies of the Doh-KDVP schema below `resources/xml/schemas/`: the official file as
/// downloaded, and the older snapshot. Each imports the `EDP-Common-1.xsd` next to it.
inline constexpr std::array<std::string_view, 2> kKdvpSchemaFiles{
    "Doh_KDVP_9.xsd",
    "snapshot-2026-05-12/Doh_KDVP_9.xsd",
};

inline constexpr std::string_view kSkipWithoutLibXml2 =
    "libxml2 not found: XSD validation skipped, see CONTRIBUTING.md";

#ifdef TAXBROKER_HAS_LIBXML2

/// Collects libxml2 messages instead of printing them to stderr.
inline void collectLibXml2Message(void* aMessages, const char* aFormat, ...) {
    std::array<char, 1024> buffer{};
    va_list arguments;

    va_start(arguments, aFormat);
    std::vsnprintf(buffer.data(), buffer.size(), aFormat, arguments);
    va_end(arguments);

    static_cast<std::string*>(aMessages)->append(buffer.data());
}

/// The libxml2 errors of an XML document checked against a schema file below
/// `resources/xml/schemas/`. Empty when the document is valid.
inline std::string schemaErrors(const std::string& aXml, std::string_view aSchemaFile) {
    const std::string schemaPath =
        std::string{TAXBROKER_SCHEMA_DIR} + "/" + std::string{aSchemaFile};
    std::string messages;

    xmlSchemaParserCtxtPtr parserContext = xmlSchemaNewParserCtxt(schemaPath.c_str());
    xmlSchemaSetParserErrors(parserContext,
                             collectLibXml2Message,
                             collectLibXml2Message,
                             &messages);
    xmlSchemaPtr schema = xmlSchemaParse(parserContext);
    xmlSchemaFreeParserCtxt(parserContext);

    if (schema == nullptr)
    {
        return "The schema could not be read: " + messages;
    }

    xmlDocPtr document = xmlReadMemory(aXml.data(),
                                       static_cast<int>(aXml.size()),
                                       "document.xml",
                                       nullptr,
                                       XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);

    if (document == nullptr)
    {
        xmlSchemaFree(schema);

        return "The document is not well-formed XML.";
    }

    xmlSchemaValidCtxtPtr validationContext = xmlSchemaNewValidCtxt(schema);
    xmlSchemaSetValidErrors(validationContext,
                            collectLibXml2Message,
                            collectLibXml2Message,
                            &messages);
    const int result = xmlSchemaValidateDoc(validationContext, document);

    xmlSchemaFreeValidCtxt(validationContext);
    xmlFreeDoc(document);
    xmlSchemaFree(schema);

    if (result != 0 && messages.empty())
    {
        return "Validation failed without a message.";
    }

    return messages;
}

#endif

/// Checks a document against both Doh-KDVP schemas. Without libxml2 the test shows as skipped.
inline void expectValidForKdvpSchemas([[maybe_unused]] const std::string& aXml) {
#ifdef TAXBROKER_HAS_LIBXML2
    for (const auto schemaFile : kKdvpSchemaFiles)
    {
        SCOPED_TRACE(schemaFile);
        EXPECT_EQ(schemaErrors(aXml, schemaFile), "");
    }
#else
    GTEST_SKIP() << kSkipWithoutLibXml2;
#endif
}

/// Checks that both Doh-KDVP schemas reject a document. Without libxml2 the test shows as skipped.
inline void expectInvalidForKdvpSchemas([[maybe_unused]] const std::string& aXml) {
#ifdef TAXBROKER_HAS_LIBXML2
    for (const auto schemaFile : kKdvpSchemaFiles)
    {
        SCOPED_TRACE(schemaFile);
        EXPECT_NE(schemaErrors(aXml, schemaFile), "");
    }
#else
    GTEST_SKIP() << kSkipWithoutLibXml2;
#endif
}

} // namespace taxbroker::test
