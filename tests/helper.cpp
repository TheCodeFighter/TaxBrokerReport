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

bool validateXml(const char* xmlPath, const char* xsdPath, const std::filesystem::path& logPath, bool saveToFile) {
    ValidationContext vctx;
    vctx.saveToFile = saveToFile;

    if (saveToFile) {
        vctx.logFile.open(logPath.c_str());
        if (!vctx.logFile.is_open()) {
            std::cerr << "Failed to open " << logPath << " for writing\n";
            return false;
        }
    }

    // Parse XSD
    xmlSchemaParserCtxtPtr schemaParser = xmlSchemaNewParserCtxt(xsdPath);
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
    xmlDocPtr doc = xmlReadFile(xmlPath, nullptr, 0);
    if (!doc) {
        xmlSchemaFreeValidCtxt(validCtxt);
        xmlSchemaFree(schema);
        std::cerr << "Failed to parse XML file: " << xmlPath << "\n";
        if (saveToFile) vctx.logFile.close();
        return false;
    }

    // Validate
    int result = xmlSchemaValidateDoc(validCtxt, doc);
    if (result != 0) {
        std::cerr << "XML validation failed with code: " << result << "\n";
        if (saveToFile) vctx.logFile << "XML validation failed with code: " << result << "\n";
    }

    // Cleanup
    xmlFreeDoc(doc);
    xmlSchemaFreeValidCtxt(validCtxt);
    xmlSchemaFree(schema);

    if (saveToFile) vctx.logFile.close();

    return result == 0;
}