#include "taxbroker/api/report_files.hpp"

namespace taxbroker::api {

std::string_view reportFormCode(ReportType aType) {
    switch (aType)
    {
    case ReportType::CapitalGains:
        return "Doh-KDVP";
    case ReportType::Dividends:
        return "Doh-Div";
    case ReportType::BankDepositInterest:
        return "Doh-DHO";
    case ReportType::OtherInterest:
        break;
    }

    return "Doh-Obr";
}

std::string reportXmlFileName(ReportType aType, int aYear, bool aUnsafe) {
    return std::string{reportFormCode(aType)} + "_" + std::to_string(aYear) +
           (aUnsafe ? "_UNSAFE" : "") + ".xml";
}

} // namespace taxbroker::api
