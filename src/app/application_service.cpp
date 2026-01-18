#include "application_service.hpp"
#include "report_loader.hpp"
#include "xml_generator.hpp"
#include <fstream>

// THE FIX: Define the incomplete type here
struct ApplicationService::Impl {
    void generateXml(const GenerationRequest& request, 
                     const nlohmann::json& jsonData, 
                     const TaxPayer& taxpayer,
                     const FormData& formData,
                     std::vector<std::filesystem::path>& outFiles) 
    {
        XmlGenerator generator;
        Transactions transactions;
        XmlGenerator::parse_json(transactions, {TransactionType::Equities, TransactionType::Funds}, jsonData);
        
        if (request.formType == TaxFormType::Doh_KDVP) {
            auto data = XmlGenerator::prepare_kdvp_data(transactions.mGains, (FormData&)formData);
            auto doc = generator.generate_doh_kdvp_xml(data, taxpayer);
            
            auto outPath = request.outputDirectory / "Doh_KDVP.xml";
            doc.save_file(outPath.c_str());
            outFiles.push_back(outPath);
        }
        
        if (request.formType == TaxFormType::Doh_DIV) {
            auto data = XmlGenerator::prepare_div_data(transactions.mIncome.mDivTransactions, (FormData&)formData);
            auto doc = generator.generate_doh_div_xml(data, taxpayer);
            
            auto outPath = request.outputDirectory / "Doh_DIV.xml";
            doc.save_file(outPath.c_str());
            outFiles.push_back(outPath);
        }

        if (request.formType == TaxFormType::Doh_DHO) {
            auto data = XmlGenerator::prepare_dho_data(transactions.mIncome.mInterests, (FormData&)formData);
            auto doc = generator.generate_doh_dho_xml(data, taxpayer);
            
            auto outPath = request.outputDirectory / "Doh_DHO.xml";
            doc.save_file(outPath.c_str());
            outFiles.push_back(outPath);
        }
    }
};

ApplicationService::ApplicationService() : m_pImpl(std::make_unique<Impl>()) {}
ApplicationService::~ApplicationService() = default;

GenerationResult ApplicationService::processRequest(const GenerationRequest& request) {
    ReportLoader localLoader;
    return processRequest(request, localLoader);
}

GenerationResult ApplicationService::processRequest(const GenerationRequest& request, ReportLoader& loader) {
    GenerationResult result;
    try {
        if (!std::filesystem::exists(request.inputFile)) {
            throw std::runtime_error("File does not exist: " + request.inputFile.string());
        }
        if (!std::filesystem::exists(request.outputDirectory)) {
            std::filesystem::create_directories(request.outputDirectory);
        }

        std::string ext = request.inputFile.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        bool isValidExt = (ext == ".pdf" || ext == ".json");
#ifdef UNIT_TEST
        if (ext == ".txt") isValidExt = true; 
#endif

        if (!isValidExt) {
            throw std::runtime_error("Unsupported file format: " + ext + ". Please provide a .pdf or .json file.");
        }

        nlohmann::json jsonData;

        if (ext == ".json") {
            std::ifstream ifs(request.inputFile);
            jsonData = nlohmann::json::parse(ifs);

            if (!jsonData.contains("income_section") || !jsonData.contains("gains_and_losses_section")) {
                throw std::runtime_error("Invalid JSON structure: Missing Trade Republic report sections.");
            }
        } else {
#ifdef UNIT_TEST
            if (loader.getRawText().empty()) {
                loader.getRawPdfData(request.inputFile.string(), ReportLoader::ProcessingMode::InMemory);
            }
#else
            loader.getRawPdfData(request.inputFile.string(), ReportLoader::ProcessingMode::InMemory);
#endif
            jsonData = loader.convertToJson();

            if (request.jsonOnly) {
                auto jsonPath = request.outputDirectory / "intermediate_data.json";
                std::ofstream out(jsonPath);
                out << jsonData.dump(4);
                result.createdFiles.push_back(jsonPath);
                result.success = true;
                return result;
            }
        }

        // Map request to domain objects
        TaxPayer taxpayer;
        taxpayer.mTaxNumber = request.taxNumber;
        taxpayer.mType      = "FO"; // Physical person
        taxpayer.mResident  = true;
        
        if (request.taxpayerName) taxpayer.mTaxPayerName = *request.taxpayerName;
        if (request.address)      taxpayer.mAddress1     = *request.address;
        if (request.birthDate)    taxpayer.mBirthDate    = *request.birthDate;

        FormData formData;
        formData.mYear            = request.year;
        formData.mDocID           = request.formDocType;
        formData.mIsResident      = true;
        if (request.phone) formData.mTelephoneNumber = *request.phone;
        if (request.email) formData.mEmail           = *request.email;

        m_pImpl->generateXml(request, jsonData, taxpayer, formData, result.createdFiles);

        result.success = true;
    } catch (const std::exception& e) {
        result.success = false;
        result.message = e.what();
    }
    return result;
}