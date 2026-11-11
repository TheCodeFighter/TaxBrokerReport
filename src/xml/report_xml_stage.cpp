#include "xml/report_xml_stage.hpp"
#include "utils/logger.hpp"
#include "xml/dho_xml_writer.hpp"
#include "xml/div_xml_writer.hpp"
#include "xml/kdvp_xml_writer.hpp"
#include "xml/obr_xml_writer.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <iterator>
#include <string_view>
#include <utility>

namespace taxbroker {
namespace {

/// The error of a writer that stopped: no field, no ISIN, no value, only the form.
ProcessingDiagnostic serializationFailed(ReportType aReport) {
    auto message = makeDiagnosticMessage(MessageKey::XmlSerializationFailed);

    return {
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::XmlSerializationFailed,
        .mContext = {.mStage = PipelineStage::XmlGeneration, .mAffectedReports = {aReport}},
        .mMessage = std::move(message.mEnglish),
        .mMessageKey = message.mKey,
        .mMessageParams = std::move(message.mParams),
    };
}

} // namespace

void writeGuardedXml(ApplicationResultInput& aInput,
                     ReportType aReport,
                     std::string_view aFormName,
                     const std::function<XmlWriteResult()>& aWrite) {
    auto& processing = aInput.mProcessingDiagnostics;

    try
    {
        auto written = aWrite();

        std::move(written.mDiagnostics.begin(),
                  written.mDiagnostics.end(),
                  std::back_inserter(processing));
        aInput.mReports[static_cast<std::size_t>(aReport)].mXml = std::move(written.mXml);
    } catch (const std::exception& aError)
    {
        LOG_ERROR("XML writer of {} stopped: {}", aFormName, aError.what());
        processing.push_back(serializationFailed(aReport));
    } catch (...)
    {
        LOG_ERROR("XML writer of {} stopped: unknown exception", aFormName);
        processing.push_back(serializationFailed(aReport));
    }
}

void writeReportXml(ApplicationResultInput& aInput,
                    const ReportMetadata& aMetadata,
                    const ReportXmlModels& aModels) {
    if (aModels.mCapitalGains && !aModels.mCapitalGains->mLists.empty())
    {
        writeGuardedXml(aInput, ReportType::CapitalGains, "Doh-KDVP", [&] {
            return KdvpXmlWriter::write(aMetadata, *aModels.mCapitalGains);
        });
    }

    if (aModels.mDividends && !aModels.mDividends->mRows.empty())
    {
        writeGuardedXml(aInput, ReportType::Dividends, "Doh-Div", [&] {
            return DivXmlWriter::write(aMetadata, *aModels.mDividends);
        });
    }

    if (aModels.mDeposit && !aModels.mDeposit->mRows.empty())
    {
        writeGuardedXml(aInput, ReportType::BankDepositInterest, "Doh-DHO", [&] {
            return DhoXmlWriter::write(aMetadata, *aModels.mDeposit);
        });
    }

    if (aModels.mOther && !aModels.mOther->mRows.empty())
    {
        writeGuardedXml(aInput, ReportType::OtherInterest, "Doh-Obr", [&] {
            return ObrXmlWriter::write(aMetadata, *aModels.mOther);
        });
    }
}

} // namespace taxbroker
