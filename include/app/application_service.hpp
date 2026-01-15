#pragma once

#include <filesystem>
#include <optional>
#include <vector>
#include <string>
#include <memory>

enum class TaxFormType {
    Doh_KDVP,
    Doh_DIV,
    Doh_DHO
};

struct GenerationRequest {
    std::filesystem::path inputPdf;
    std::filesystem::path outputDirectory;
    
    bool jsonOnly = false;
    
    // Obligatory
    TaxFormType formType;
    std::string taxNumber;

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

    ApplicationService(const ApplicationService&) = delete;
    ApplicationService& operator=(const ApplicationService&) = delete;
    
    ApplicationService(ApplicationService&&) noexcept;
    ApplicationService& operator=(ApplicationService&&) noexcept;

    // main processing method for GUI
    GenerationResult processRequest(const GenerationRequest& request);

private:
    // Forward declaration of implementation struct
    struct Impl;
    std::unique_ptr<Impl> m_pImpl;
};