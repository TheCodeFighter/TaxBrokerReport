#pragma once

#include <gtest/gtest.h>

#include <array>
#include <functional>
#include <map>
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

/// The Doh-Obr schema below `resources/xml/schemas/`; it imports `EDP-Common-1.xsd` next to it.
inline constexpr std::string_view kObrSchemaFile = "Doh_Obr_2.xsd";

/// The Doh-Div schema below `resources/xml/schemas/`; it imports `EDP-Common-1.xsd` next to it.
inline constexpr std::string_view kDivSchemaFile = "Doh_Div_3.xsd";

/// The Doh-DHO schema below `resources/xml/schemas/`; it imports `EDP-Common-1.xsd` next to it.
inline constexpr std::string_view kDhoSchemaFile = "Doh_DHO_4.xsd";

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

/// A schema file read once per test process: the schema, or the messages of why it was not read.
struct CachedSchema {
    xmlSchemaPtr mSchema = nullptr;
    std::string mReadErrors;

    CachedSchema() = default;
    CachedSchema(const CachedSchema&) = delete;
    CachedSchema& operator=(const CachedSchema&) = delete;

    ~CachedSchema() {
        xmlSchemaFree(mSchema);
    }
};

/// Reads a schema file below `resources/xml/schemas/` the first time it is asked for. Parsing the
/// schema files is the most expensive part of a check, and many tests check a document.
inline const CachedSchema& cachedSchema(std::string_view aSchemaFile) {
    static std::map<std::string, CachedSchema, std::less<>> schemas;

    const auto found = schemas.find(aSchemaFile);

    if (found != schemas.end())
    {
        return found->second;
    }

    CachedSchema& cached = schemas[std::string{aSchemaFile}];
    const std::string schemaPath =
        std::string{TAXBROKER_SCHEMA_DIR} + "/" + std::string{aSchemaFile};

    xmlSchemaParserCtxtPtr parserContext = xmlSchemaNewParserCtxt(schemaPath.c_str());
    xmlSchemaSetParserErrors(parserContext,
                             collectLibXml2Message,
                             collectLibXml2Message,
                             &cached.mReadErrors);
    cached.mSchema = xmlSchemaParse(parserContext);
    xmlSchemaFreeParserCtxt(parserContext);

    return cached;
}

/// The libxml2 errors of an XML document checked against a schema file below
/// `resources/xml/schemas/`. Empty when the document is valid.
inline std::string schemaErrors(const std::string& aXml, std::string_view aSchemaFile) {
    const CachedSchema& cached = cachedSchema(aSchemaFile);
    std::string messages;

    if (cached.mSchema == nullptr)
    {
        return "The schema could not be read: " + cached.mReadErrors;
    }

    xmlDocPtr document = xmlReadMemory(aXml.data(),
                                       static_cast<int>(aXml.size()),
                                       "document.xml",
                                       nullptr,
                                       XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);

    if (document == nullptr)
    {
        return "The document is not well-formed XML.";
    }

    xmlSchemaValidCtxtPtr validationContext = xmlSchemaNewValidCtxt(cached.mSchema);
    xmlSchemaSetValidErrors(validationContext,
                            collectLibXml2Message,
                            collectLibXml2Message,
                            &messages);
    const int result = xmlSchemaValidateDoc(validationContext, document);

    xmlSchemaFreeValidCtxt(validationContext);
    xmlFreeDoc(document);

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

/// Checks a document against the Doh-Obr schema. Without libxml2 the test shows as skipped.
inline void expectValidForObrSchema([[maybe_unused]] const std::string& aXml) {
#ifdef TAXBROKER_HAS_LIBXML2
    EXPECT_EQ(schemaErrors(aXml, kObrSchemaFile), "");
#else
    GTEST_SKIP() << kSkipWithoutLibXml2;
#endif
}

/// Checks that the Doh-Obr schema rejects a document. Without libxml2 the test shows as skipped.
inline void expectInvalidForObrSchema([[maybe_unused]] const std::string& aXml) {
#ifdef TAXBROKER_HAS_LIBXML2
    EXPECT_NE(schemaErrors(aXml, kObrSchemaFile), "");
#else
    GTEST_SKIP() << kSkipWithoutLibXml2;
#endif
}

/// Checks a document against the Doh-Div schema. Without libxml2 the test shows as skipped.
inline void expectValidForDivSchema([[maybe_unused]] const std::string& aXml) {
#ifdef TAXBROKER_HAS_LIBXML2
    EXPECT_EQ(schemaErrors(aXml, kDivSchemaFile), "");
#else
    GTEST_SKIP() << kSkipWithoutLibXml2;
#endif
}

/// Checks that the Doh-Div schema rejects a document. Without libxml2 the test shows as skipped.
inline void expectInvalidForDivSchema([[maybe_unused]] const std::string& aXml) {
#ifdef TAXBROKER_HAS_LIBXML2
    EXPECT_NE(schemaErrors(aXml, kDivSchemaFile), "");
#else
    GTEST_SKIP() << kSkipWithoutLibXml2;
#endif
}

/// Checks a document against the Doh-DHO schema. Without libxml2 the test shows as skipped.
inline void expectValidForDhoSchema([[maybe_unused]] const std::string& aXml) {
#ifdef TAXBROKER_HAS_LIBXML2
    EXPECT_EQ(schemaErrors(aXml, kDhoSchemaFile), "");
#else
    GTEST_SKIP() << kSkipWithoutLibXml2;
#endif
}

/// Checks that the Doh-DHO schema rejects a document. Without libxml2 the test shows as skipped.
inline void expectInvalidForDhoSchema([[maybe_unused]] const std::string& aXml) {
#ifdef TAXBROKER_HAS_LIBXML2
    EXPECT_NE(schemaErrors(aXml, kDhoSchemaFile), "");
#else
    GTEST_SKIP() << kSkipWithoutLibXml2;
#endif
}

} // namespace taxbroker::test
