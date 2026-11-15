#include "taxbroker/api/report_files.hpp"

#include <gtest/gtest.h>

namespace {
using namespace taxbroker;
using taxbroker::api::reportFormCode;
using taxbroker::api::reportXmlFileName;

TEST(ReportFileNameTest, CapitalGainsIsDohKdvp) {
    EXPECT_EQ(reportXmlFileName(ReportType::CapitalGains, 2024, false), "Doh-KDVP_2024.xml");
}

TEST(ReportFileNameTest, DeveloperExportHasItsOwnName) {
    EXPECT_EQ(reportXmlFileName(ReportType::CapitalGains, 2024, true), "Doh-KDVP_2024_UNSAFE.xml");
}

TEST(ReportFileNameTest, DividendsIsDohDiv) {
    EXPECT_EQ(reportXmlFileName(ReportType::Dividends, 2024, false), "Doh-Div_2024.xml");
}

TEST(ReportFileNameTest, BankInterestIsDohDho) {
    EXPECT_EQ(reportXmlFileName(ReportType::BankDepositInterest, 2024, false), "Doh-DHO_2024.xml");
}

TEST(ReportFileNameTest, OtherInterestIsDohObr) {
    EXPECT_EQ(reportXmlFileName(ReportType::OtherInterest, 2025, false), "Doh-Obr_2025.xml");
}

TEST(ReportFileNameTest, FormCodesAreTheFursCodes) {
    EXPECT_EQ(reportFormCode(ReportType::CapitalGains), "Doh-KDVP");
    EXPECT_EQ(reportFormCode(ReportType::Dividends), "Doh-Div");
    EXPECT_EQ(reportFormCode(ReportType::BankDepositInterest), "Doh-DHO");
    EXPECT_EQ(reportFormCode(ReportType::OtherInterest), "Doh-Obr");
}

} // namespace
