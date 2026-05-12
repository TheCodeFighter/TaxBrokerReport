#include "helper.hpp"

// Callback for validation errors
void xmlValidationError(void* ctx, const char* msg, ...) {
    va_list args;
    va_start(args, msg);

    ValidationContext* vctx = static_cast<ValidationContext*>(ctx);

    // Format message
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), msg, args);

    // Print to stderr
    std::cerr << "XML Validation Error: " << buffer;

    // Save to file if requested
    if (vctx && vctx->saveToFile && vctx->logFile.is_open()) {
        vctx->logFile << "XML Validation Error: " << buffer;
    }

    va_end(args);
}

// Callback for validation warnings
void xmlValidationWarning(void* ctx, const char* msg, ...) {
    va_list args;
    va_start(args, msg);

    ValidationContext* vctx = static_cast<ValidationContext*>(ctx);

    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), msg, args);

    std::cerr << "XML Validation Warning: " << buffer;

    if (vctx && vctx->saveToFile && vctx->logFile.is_open()) {
        vctx->logFile << "XML Validation Warning: " << buffer;
    }

    va_end(args);
}

bool validateXml(const xmlDocPtr doc, const std::filesystem::path& xsdPath, std::optional<std::filesystem::path> logPath) {
    ValidationContext vctx;
    vctx.saveToFile = logPath.has_value();

    if (vctx.saveToFile) {
        vctx.logFile.open(logPath.value().c_str());
        if (!vctx.logFile.is_open()) {
            std::cerr << "Failed to open " << logPath.value() << " for writing\n";
            return false;
        }
    }

    // Parse XSD
    xmlSchemaParserCtxtPtr schemaParser = xmlSchemaNewParserCtxt(xsdPath.c_str());
    if (!schemaParser) return false;

    xmlSchemaPtr schema = xmlSchemaParse(schemaParser);
    xmlSchemaFreeParserCtxt(schemaParser);
    if (!schema) return false;

    xmlSchemaValidCtxtPtr validCtxt = xmlSchemaNewValidCtxt(schema);
    if (!validCtxt) {
        xmlSchemaFree(schema);
        return false;
    }

    // Set error/warning callbacks with context
    xmlSchemaSetValidErrors(validCtxt, xmlValidationError, xmlValidationWarning, &vctx);

    // Parse XML
    if (!doc) {
        xmlSchemaFreeValidCtxt(validCtxt);
        xmlSchemaFree(schema);
        if (vctx.saveToFile) vctx.logFile.close();
        return false;
    }

    // Validate
    int result = xmlSchemaValidateDoc(validCtxt, doc);
    if (result != 0) {
        std::cerr << "XML validation failed with code: " << result << "\n";
        if (vctx.saveToFile) vctx.logFile << "XML validation failed with code: " << result << "\n";
    }

    // Cleanup
    xmlSchemaFreeValidCtxt(validCtxt);
    xmlSchemaFree(schema);

    if (vctx.saveToFile) vctx.logFile.close();

    return result == 0;
}

XmlDocUPtr make_xml_doc(xmlDocPtr doc) {
    return XmlDocUPtr(doc, xmlFreeDoc);
}

XmlDocUPtr convertPugiToLibxml(const pugi::xml_document& pugiDoc)
{
    std::ostringstream oss;
    pugiDoc.save(oss, "  ", pugi::format_raw);

    const std::string xml = oss.str();

    xmlDocPtr doc = xmlReadMemory(
        xml.c_str(),
        static_cast<int>(xml.size()),
        "generated.xml",
        nullptr,
        XML_PARSE_NONET | XML_PARSE_NOBLANKS
    );

    if (!doc) {
        return XmlDocUPtr(nullptr, xmlFreeDoc);
    }

    return XmlDocUPtr(doc, xmlFreeDoc);
}

