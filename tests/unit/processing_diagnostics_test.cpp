#include "taxbroker/processing_diagnostics.hpp"

#include <gtest/gtest.h>

namespace {
using namespace taxbroker;

TEST(ProcessingDiagnosticsTest, MapsNumericFailuresToTheirSuppliedScopeWithoutRawValues) {
    ProcessingContext context;
    context.mAffectedReports = {ReportType::OtherInterest,
                                ReportType::BankDepositInterest,
                                ReportType::OtherInterest};
    context.mIsin = "XX0000000001";
    context.mField = "gross_amount";
    const auto overflow = numericDiagnostic(NumericError::Overflow, context);

    EXPECT_EQ(overflow.mCode, ProcessingDiagnosticCode::ArithmeticOverflow);
    EXPECT_EQ(
        overflow.mContext.mAffectedReports,
        (std::vector<ReportType>{ReportType::BankDepositInterest, ReportType::OtherInterest}));
    EXPECT_EQ(overflow.mContext.mIsin, context.mIsin);
    EXPECT_EQ(overflow.mContext.mField, context.mField);
    EXPECT_EQ(overflow.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(numericDiagnostic(NumericError::UnrepresentableValue, context).mCode,
              ProcessingDiagnosticCode::UnrepresentableValue);
    EXPECT_EQ(numericDiagnostic(NumericError::InvalidExchangeRate, context).mCode,
              ProcessingDiagnosticCode::InvalidExchangeRate);
    context.mStage = PipelineStage::CorporateAction;
    context.mAffectedReports = {ReportType::CapitalGains};
    EXPECT_EQ(numericDiagnostic(NumericError::Overflow, context).mCode,
              ProcessingDiagnosticCode::CorporateActionUnrepresentable);
    context.mStage = PipelineStage::FifoTax;
    context.mAffectedReports = {ReportType::Dividends};
    EXPECT_EQ(
        numericDiagnostic(NumericError::InvalidDenominator, context).mContext.mAffectedReports,
        (std::vector<ReportType>{ReportType::Dividends}));
}

} // namespace
