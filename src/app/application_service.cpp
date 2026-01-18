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
        if (!std::filesystem::exists(request.outputDirectory)) {
            std::filesystem::create_directories(request.outputDirectory);
        }

        // Bypass Poppler if text is already set (for tests)
#ifdef UNIT_TEST
        if (loader.getRawText().empty()) {
            loader.getRawPdfData(request.inputPdf.string(), ReportLoader::ProcessingMode::InMemory);
        }
#else
        loader.getRawPdfData(request.inputPdf.string(), ReportLoader::ProcessingMode::InMemory);
#endif

        const auto jsonData = loader.convertToJson();

        if (request.jsonOnly) {
            auto jsonPath = request.outputDirectory / "intermediate_data.json";
            std::ofstream out(jsonPath);
            out << jsonData.dump(4);
            result.createdFiles.push_back(jsonPath);
            result.success = true;
            return result;
        }

        // Map request to domain objects
        TaxPayer taxpayer;
        taxpayer.mTaxNumber = request.taxNumber;
        // ... fill other fields ...

        FormData formData;
        formData.mYear = request.year;

        // Now m_pImpl is "complete" and this call will work
        m_pImpl->generateXml(request, jsonData, taxpayer, formData, result.createdFiles);

        result.success = true;
    } catch (const std::exception& e) {
        result.success = false;
        result.message = e.what();
    }
    return result;
}