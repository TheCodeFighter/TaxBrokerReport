#include <fstream>
#include <sstream>

#include "application_service.hpp"
#include "report_loader.hpp"
#include "xml_generator.hpp"
#include "util_xml.hpp"

struct ApplicationService::Impl {
    
    TransactionType mapToTransactionType(TaxFormType type) {
        switch(type) {
            case TaxFormType::Doh_KDVP: return TransactionType::Equities; // or Funds
            case TaxFormType::Doh_DIV:  return TransactionType::Equities;
            case TaxFormType::Doh_DHO:  return TransactionType::Bonds; // TODO
            default: return TransactionType::None;
        }
    }

    void generateXml(const GenerationRequest& request,  // FIXME: too much input param? -> struct
                     const nlohmann::json& jsonData, 
                     const TaxPayer& taxpayer,
                     const FormData& formData,
                     std::vector<std::filesystem::path>& outFiles) {

        XmlGenerator generator;
        Transactions transactions;
        
        // KDVP
        if (request.formType == TaxFormType::Doh_KDVP) {
            XmlGenerator::parse_json(transactions, 
                                   {TransactionType::Equities, TransactionType::Funds}, 
                                   jsonData);
            
            auto data = XmlGenerator::prepare_kdvp_data(transactions.mGains, (FormData&)formData);
            
            auto doc = generator.generate_doh_kdvp_xml(data, taxpayer);
            
            auto outPath = request.outputDirectory / "Doh_KDVP.xml";
            if (!doc.save_file(outPath.c_str(), "  ")) {
                throw std::runtime_error("Failed to write XML to filesystem.");
            }
            outFiles.push_back(outPath);
        }
        // TODO: DIV and DHO
    }
};

ApplicationService::ApplicationService() : m_pImpl(std::make_unique<Impl>()) {}
ApplicationService::~ApplicationService() = default;
ApplicationService::ApplicationService(ApplicationService&&) noexcept = default;
ApplicationService& ApplicationService::operator=(ApplicationService&&) noexcept = default;

GenerationResult ApplicationService::processRequest(const GenerationRequest& request) {
    GenerationResult result;
    result.success = false;

    if (!std::filesystem::exists(request.inputPdf)) {
        result.message = "Input PDF file not found: " + request.inputPdf.string();
        return result;
    }

    if (!std::filesystem::exists(request.outputDirectory)) {
        try {
            std::filesystem::create_directories(request.outputDirectory);
        } catch (const std::exception& e) {
            result.message = "Cannot access or create output directory.";
            return result;
        }
    }

    try {
        // Report Loading (PDF -> JSON)
        ReportLoader loader;
        loader.getRawPdfData(request.inputPdf.string(), ReportLoader::ProcessingMode::InMemory);
        const auto jsonData = loader.convertToJson();

        // JSON Output
        if (request.jsonOnly) {
            auto jsonPath = request.outputDirectory / "intermediate_data.json";
            std::ofstream out(jsonPath);
            out << jsonData.dump(4);
            result.createdFiles.push_back(jsonPath);
            result.success = true;
            result.message = "JSON generation successful.";
            return result;
        }

        FormData formData;
        // TODO: check matching
        formData.mDocID = FormType::Original; 
        formData.mTelephoneNumber = request.phone;
        formData.mEmail = request.email;
        formData.mYear = 2024; // TODO: need to be input

        TaxPayer taxpayer;
        taxpayer.mTaxNumber = request.taxNumber;
        taxpayer.mTaxPayerName = request.taxpayerName;
        taxpayer.mAddress1 = request.address;
        taxpayer.mBirthDate = request.birthDate;

        // XML generation
        m_pImpl->generateXml(request, jsonData, taxpayer, formData, result.createdFiles);

        result.success = true;
        result.message = "Processing completed successfully.";

    } catch (const std::exception& e) {
        result.success = false;
        result.message = std::string("Error: ") + e.what();
    } catch (...) {
        result.success = false;
        result.message = "Unknown critical error occurred.";
    }

    return result;
}