#include "taxbroker/processing_diagnostics.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace {
using namespace taxbroker;

TEST(ProcessingDiagnosticsTest, OverflowReturnsAControlledMessageAndSafeProvenance) {
    const SourceReference first{
        .mBroker = Broker::TradeRepublic,
        .mFilename = SourceFilename::fromPath("/private/imports/first.csv"),
        .mSourceRow = 7,
        .mTransactionId = "synthetic-first",
        .mInputSequence = {0, 5},
    };

    const SourceReference second{
        .mBroker = Broker::InteractiveBrokers,
        .mFilename = SourceFilename::fromPath(R"(C:\private\imports\second.csv)"),
        .mSourceRow = 3,
        .mTransactionId = "synthetic-second",
        .mInputSequence = {1, 1},
    };

    const ProcessingContext context{
        .mStage = PipelineStage::FifoTax,
        .mAffectedReports = {ReportType::OtherInterest,
                             ReportType::BankDepositInterest,
                             ReportType::OtherInterest},
        .mSources = {second, first, second},
        .mTaxDate =
            Date{std::chrono::sys_days{std::chrono::year{2024} / 1 / 15}.time_since_epoch()},
        .mField = "gross_amount",
        .mInstrumentName = "Synthetic Income Instrument",
        .mIsin = "XX0000000001",
    };

    const auto diagnostic = numericDiagnostic(NumericError::Overflow, context);

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::ArithmeticOverflow);
    EXPECT_EQ(diagnostic.mMessage, "The calculation exceeds the supported integer range.");
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::ArithmeticOverflow);
    EXPECT_TRUE(diagnostic.mMessageParams.empty());
    EXPECT_EQ(diagnostic.mMessage.find("private"), std::string::npos);
    EXPECT_EQ(diagnostic.mMessage.find("synthetic-first"), std::string::npos);
    EXPECT_EQ(diagnostic.mMessage.find("Synthetic Income Instrument"), std::string::npos);
    EXPECT_EQ(diagnostic.mMessage.find("XX0000000001"), std::string::npos);

    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::FifoTax);
    EXPECT_EQ(diagnostic.mContext.mAffectedReports,
              (std::vector{ReportType::BankDepositInterest, ReportType::OtherInterest}));
    ASSERT_EQ(diagnostic.mContext.mSources.size(), 2U);

    EXPECT_EQ(diagnostic.mContext.mSources, (std::vector{first, second}));
    EXPECT_EQ(diagnostic.mContext.mSources[0].mFilename.value(), "first.csv");
    EXPECT_EQ(diagnostic.mContext.mSources[1].mFilename.value(), "second.csv");
    EXPECT_EQ(diagnostic.mContext.mTaxDate, context.mTaxDate);
    EXPECT_EQ(diagnostic.mContext.mField, "gross_amount");
    EXPECT_EQ(diagnostic.mContext.mInstrumentName, "Synthetic Income Instrument");
    EXPECT_EQ(diagnostic.mContext.mIsin, "XX0000000001");

    EXPECT_EQ(context.mAffectedReports,
              (std::vector{ReportType::OtherInterest,
                           ReportType::BankDepositInterest,
                           ReportType::OtherInterest}));
    EXPECT_EQ(context.mSources, (std::vector{second, first, second}));
}

TEST(ProcessingDiagnosticsTest, UnrepresentableValueReturnsAControlledMessage) {
    const auto diagnostic = numericDiagnostic(NumericError::UnrepresentableValue, {});

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::UnrepresentableValue);
    EXPECT_EQ(diagnostic.mMessage,
              "The value cannot be represented at the required precision or range.");
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::UnrepresentableValue);
    EXPECT_TRUE(diagnostic.mMessageParams.empty());
    EXPECT_TRUE(diagnostic.mContext.mAffectedReports.empty());
    EXPECT_TRUE(diagnostic.mContext.mSources.empty());
}

TEST(ProcessingDiagnosticsTest, InvalidInputReturnsAControlledMessage) {
    const auto diagnostic = numericDiagnostic(NumericError::InvalidInput, {});

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::UnrepresentableValue);
    EXPECT_EQ(diagnostic.mMessage,
              "The value cannot be represented at the required precision or range.");
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::UnrepresentableValue);
}

TEST(ProcessingDiagnosticsTest, InvalidDenominatorRetainsOnlyTheCallerSuppliedDividendScope) {
    const ProcessingContext context{
        .mStage = PipelineStage::FifoTax,
        .mAffectedReports = {ReportType::Dividends},
    };

    const auto diagnostic = numericDiagnostic(NumericError::InvalidDenominator, context);

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::UnrepresentableValue);
    EXPECT_EQ(diagnostic.mMessage,
              "The value cannot be represented at the required precision or range.");
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::UnrepresentableValue);
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::FifoTax);
    EXPECT_EQ(diagnostic.mContext.mAffectedReports, (std::vector{ReportType::Dividends}));
}

TEST(ProcessingDiagnosticsTest, InvalidExchangeRateReturnsAControlledMessage) {
    const auto diagnostic = numericDiagnostic(NumericError::InvalidExchangeRate, {});

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::InvalidExchangeRate);
    EXPECT_EQ(diagnostic.mMessage, "The exchange rate is invalid.");
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::InvalidExchangeRate);
    EXPECT_TRUE(diagnostic.mMessageParams.empty());
}

TEST(ProcessingDiagnosticsTest,
     UnverifiedExchangeRateDirectionReturnsTheInvalidExchangeRateMessage) {
    const auto diagnostic = numericDiagnostic(NumericError::UnverifiedExchangeRateDirection, {});

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::InvalidExchangeRate);
    EXPECT_EQ(diagnostic.mMessage, "The exchange rate is invalid.");
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::InvalidExchangeRate);
    EXPECT_TRUE(diagnostic.mMessageParams.empty());
}

TEST(ProcessingDiagnosticsTest, CorporateActionContextOverridesTheGenericOverflowCode) {
    const ProcessingContext context{
        .mStage = PipelineStage::CorporateAction,
        .mAffectedReports = {ReportType::CapitalGains},
    };

    const auto diagnostic = numericDiagnostic(NumericError::Overflow, context);

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, ProcessingDiagnosticCode::CorporateActionUnrepresentable);
    EXPECT_EQ(diagnostic.mMessage,
              "The corporate action cannot preserve the position and exact basis.");
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::CorporateActionUnrepresentable);
    EXPECT_TRUE(diagnostic.mMessageParams.empty());
    EXPECT_EQ(diagnostic.mContext.mStage, PipelineStage::CorporateAction);
    EXPECT_EQ(diagnostic.mContext.mAffectedReports, (std::vector{ReportType::CapitalGains}));
}

} // namespace
