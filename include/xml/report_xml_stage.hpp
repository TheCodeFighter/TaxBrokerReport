#pragma once

#include "taxbroker/application_result.hpp"
#include "taxbroker/income_report_models.hpp"
#include "taxbroker/report_models.hpp"
#include "xml/xml_write_result.hpp"

#include <functional>
#include <string_view>

namespace taxbroker {

/// The model of each form to write. A null pointer means the form has nothing to write.
struct ReportXmlModels {
    const CapitalGainsReportKDVP* mCapitalGains{}; ///< Doh-KDVP; null when not written.
    const DividendReportDiv* mDividends{};         ///< Doh-Div; null when not written.
    const DepositInterestReportDHO* mDeposit{};    ///< Doh-DHO; null when not written.
    const OtherInterestReportObr* mOther{};        ///< Doh-Obr; null when not written.
};

/**
 * Runs one writer for aReport and stores its XML and errors in aInput. Any exception, of the
 * standard library or not, gives one `xml_serialization_failed` error for aReport and a log line
 * with aFormName; nothing is rethrown.
 */
void writeGuardedXml(ApplicationResultInput& aInput,
                     ReportType aReport,
                     std::string_view aFormName,
                     const std::function<XmlWriteResult()>& aWrite);

/**
 * Writes the XML of each form whose model has rows, one writer at a time, and stores it in
 * aInput with the writer's errors. A writer that throws gives `xml_serialization_failed` for its
 * form only and is logged; the other forms keep their XML. A writer throws only with a fixed
 * text, never with a value, a name or an ISIN, because the text goes to the log.
 */
void writeReportXml(ApplicationResultInput& aInput,
                    const ReportMetadata& aMetadata,
                    const ReportXmlModels& aModels);

} // namespace taxbroker
