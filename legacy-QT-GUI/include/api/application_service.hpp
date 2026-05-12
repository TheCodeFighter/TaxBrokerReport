#pragma once

#include <filesystem>
#include <optional>
#include <vector>
#include <string>
#include <memory>

#include "report_loader.hpp"
#include "xml_generator.hpp"

enum class TaxFormType {
    Doh_KDVP,
    Doh_DIV,
    Doh_DHO
};

struct GenerationRequest {
    std::filesystem::path inputFile;
    std::filesystem::path outputDirectory;
    bool jsonOnly = false;
    
    // Obligatory
    TaxFormType formType;
    std::string taxNumber;
    int year;
    FormType  formDocType = FormType::Original;

    // Optional
    std::optional<std::string> phone;
    std::optional<std::string> email;
    std::optional<std::string> taxpayerName;
    std::optional<std::string> address;
    std::optional<std::string> birthDate;
};

struct GenerationResult {
    bool success = false;
    std::string message;
    std::vector<std::filesystem::path> createdFiles;
};

class ApplicationService {
public:
    ApplicationService();
    ~ApplicationService();

    // Standard method for Production/GUI
    GenerationResult processRequest(const GenerationRequest& request);

    // Overloaded method for Testing (Dependency Injection)
    GenerationResult processRequest(const GenerationRequest& request, ReportLoader& loader);

private:
    struct Impl;
    std::unique_ptr<Impl> m_pImpl;
};