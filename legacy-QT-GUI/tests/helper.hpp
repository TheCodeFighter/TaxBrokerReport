#pragma once

#include <filesystem>
#include <libxml/xmlschemas.h>
#include <libxml/parser.h>
#include <iostream>
#include <fstream>
#include <cstdarg>
#include <optional>
#include <pugixml.hpp>

// Struct to pass multiple outputs to callbacks
struct ValidationContext {
    bool saveToFile;
    std::ofstream logFile;
};

using XmlDocUPtr = std::unique_ptr<_xmlDoc, void(*)(_xmlDoc*)>;

XmlDocUPtr make_xml_doc(xmlDocPtr doc);

XmlDocUPtr convertPugiToLibxml(const pugi::xml_document& pugiDoc);

// Callback for validation errors
void xmlValidationError(void* ctx, const char* msg, ...);

// Callback for validation warnings
void xmlValidationWarning(void* ctx, const char* msg, ...);

bool validateXml(const xmlDocPtr doc, const std::filesystem::path& xsdPath, std::optional<std::filesystem::path> logPath = std::nullopt);