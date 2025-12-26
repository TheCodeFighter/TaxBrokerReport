#pragma once

#include <filesystem>
#include <libxml/xmlschemas.h>
#include <libxml/parser.h>
#include <iostream>
#include <fstream>
#include <cstdarg>

// Struct to pass multiple outputs to callbacks
struct ValidationContext {
    bool saveToFile;
    std::ofstream logFile;
};

// Callback for validation errors
void xmlValidationError(void* ctx, const char* msg, ...);

// Callback for validation warnings
void xmlValidationWarning(void* ctx, const char* msg, ...);

bool validateXml(const char* xmlPath, const char* xsdPath, const std::filesystem::path& logPath, bool saveToFile = false);