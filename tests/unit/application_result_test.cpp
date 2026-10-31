#include "taxbroker/application_result.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace taxbroker;

constexpr auto kCapital = ReportType::CapitalGains;
constexpr auto kDividends = ReportType::Dividends;
constexpr auto kBank = ReportType::BankDepositInterest;
constexpr auto kOther = ReportType::OtherInterest;

Date day(int aYear, unsigned aMonth, unsigned aDay) {
    return Date{std::chrono::sys_days{std::chrono::year{aYear} / aMonth / aDay}.time_since_epoch()};
}

// Report and overall status. The scenarios follow "Result examples" in diagnostics.md.

TEST(ApplicationResultStatusTest, CompleteSuccessHasNoDiagnostics) {
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mReports[0].mXml = "<capital/>";
    input.mReports[1].mXml = "<dividends/>";

    const auto result = ApplicationResultBuilder::build(std::move(input));

    EXPECT_EQ(result.mStatus, OverallStatus::Success);
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kDividends).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kBank).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(kOther).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(kCapital).mXml, "<capital/>");
    EXPECT_EQ(result.report(kDividends).mXml, "<dividends/>");
    EXPECT_FALSE(result.report(kBank).mXml.has_value());
    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_TRUE(result.mRequests.empty());
}

TEST(ApplicationResultStatusTest, FourEmptyReportsAreASuccess) {
    const auto result = ApplicationResultBuilder::build(ApplicationResultInput{});

    EXPECT_EQ(result.mStatus, OverallStatus::Success);

    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::NoData);
    EXPECT_FALSE(result.report(kCapital).mXml.has_value());
    EXPECT_FALSE(result.report(kCapital).mUnsafe);

    EXPECT_EQ(result.report(kDividends).mStatus, ReportStatus::NoData);
    EXPECT_FALSE(result.report(kDividends).mXml.has_value());
    EXPECT_FALSE(result.report(kDividends).mUnsafe);

    EXPECT_EQ(result.report(kBank).mStatus, ReportStatus::NoData);
    EXPECT_FALSE(result.report(kBank).mXml.has_value());
    EXPECT_FALSE(result.report(kBank).mUnsafe);

    EXPECT_EQ(result.report(kOther).mStatus, ReportStatus::NoData);
    EXPECT_FALSE(result.report(kOther).mXml.has_value());
    EXPECT_FALSE(result.report(kOther).mUnsafe);
}

TEST(ApplicationResultStatusTest, OneOpenRequestNeedsInputAndKeepsOtherXml) {
    // Incomplete history is an error for capital gains; its failure-resolution request is open.
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mProcessingDiagnostics = {ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::IncompleteHistory,
        .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kCapital}},
        .mMessage = "Synthetic processing message.",
    }};
    input.mRequests = {RequestInput{
        .mQuestion =
            {
                .mType = RequestType::CapitalGainsFailureResolutionRequired,
                .mAffectedReports = {kCapital},
                .mData =
                    FailureResolutionQuestion{
                        .mInvestments = {FailedInvestment{
                            .mIsin = "XX0000000001",
                            .mName = "Synthetic Share",
                            .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
                        }}},
            },
        .mProcessingDiagnostics = {0},
    }};
    input.mReports[1].mXml = "<dividends/>";

    const auto result = ApplicationResultBuilder::build(std::move(input));

    EXPECT_EQ(result.mStatus, OverallStatus::NeedsInput);
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::NeedsInput);
    EXPECT_EQ(result.report(kCapital).mRequestIds, (std::vector<std::string>{"request-0001"}));
    EXPECT_EQ(result.report(kCapital).mDiagnosticIds,
              (std::vector<std::string>{"diagnostic-0001"}));
    EXPECT_EQ(result.report(kDividends).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kDividends).mXml, "<dividends/>");

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_TRUE(result.mDiagnostics[0].mBlocking);
    EXPECT_EQ(result.mDiagnostics[0].mRequestId, "request-0001");
}

TEST(ApplicationResultStatusTest, BlockingErrorBeatsGeneratedXmlOfTheSameReport) {
    // The dividends report stage handed over XML, but a blocking error affects dividends.
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mProcessingDiagnostics = {ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
        .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kDividends}},
        .mMessage = "Synthetic processing message.",
    }};
    input.mReports[0].mXml = "<capital/>";
    input.mReports[1].mXml = "<dividends/>";

    const auto result = ApplicationResultBuilder::build(std::move(input));

    EXPECT_EQ(result.mStatus, OverallStatus::PartialSuccess);
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kCapital).mXml, "<capital/>");
    EXPECT_EQ(result.report(kDividends).mStatus, ReportStatus::Failed);
    EXPECT_FALSE(result.report(kDividends).mXml.has_value());
}

TEST(ApplicationResultStatusTest, OnlyCapitalGainsFailsAndOthersStayAvailable) {
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mProcessingDiagnostics = {ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
        .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kCapital}},
        .mMessage = "Synthetic processing message.",
    }};
    input.mReports[1].mXml = "<dividends/>";
    input.mReports[2].mNoDataReason = NoDataReason::BelowDepositInterestThreshold;

    const auto result = ApplicationResultBuilder::build(std::move(input));

    EXPECT_EQ(result.mStatus, OverallStatus::PartialSuccess);
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(kCapital).mDiagnosticIds,
              (std::vector<std::string>{"diagnostic-0001"}));
    EXPECT_EQ(result.report(kDividends).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kBank).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(kOther).mStatus, ReportStatus::NoData);
    EXPECT_TRUE(result.report(kDividends).mDiagnosticIds.empty());
}

TEST(ApplicationResultStatusTest, OnlyDividendsFailsAndOthersStayAvailable) {
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mProcessingDiagnostics = {ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
        .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kDividends}},
        .mMessage = "Synthetic processing message.",
    }};
    input.mReports[0].mXml = "<capital/>";

    const auto result = ApplicationResultBuilder::build(std::move(input));

    EXPECT_EQ(result.mStatus, OverallStatus::PartialSuccess);
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kDividends).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(kBank).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(kOther).mStatus, ReportStatus::NoData);
}

TEST(ApplicationResultStatusTest, OnlyBankInterestFailsAndOthersStayAvailable) {
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mProcessingDiagnostics = {ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
        .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kBank}},
        .mMessage = "Synthetic processing message.",
    }};
    input.mReports[0].mXml = "<capital/>";
    input.mReports[1].mXml = "<dividends/>";

    const auto result = ApplicationResultBuilder::build(std::move(input));

    EXPECT_EQ(result.mStatus, OverallStatus::PartialSuccess);
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kDividends).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kBank).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(kOther).mStatus, ReportStatus::NoData);
}

TEST(ApplicationResultStatusTest, OnlyOtherInterestFailsAndOthersStayAvailable) {
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mProcessingDiagnostics = {ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
        .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kOther}},
        .mMessage = "Synthetic processing message.",
    }};
    input.mReports[0].mXml = "<capital/>";
    input.mReports[2].mXml = "<bank/>";

    const auto result = ApplicationResultBuilder::build(std::move(input));

    EXPECT_EQ(result.mStatus, OverallStatus::PartialSuccess);
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kDividends).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(kBank).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kOther).mStatus, ReportStatus::Failed);
}

TEST(ApplicationResultStatusTest, FileLevelParseFailureFailsAllFourReports) {
    // A file-level failure has no row kind, so the app cannot know which forms the file held.
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mMergeDiagnostics = {SourcedParseDiagnostic{
        .mSourceIndex = 0,
        .mBroker = Broker::TradeRepublic,
        .mDiagnostic =
            ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = DiagnosticCode::ParseError,
                .mSourceFile = "export.csv",
                .mMessage = "The file could not be read.",
            },
    }};

    const auto result = ApplicationResultBuilder::build(std::move(input));

    EXPECT_EQ(result.mStatus, OverallStatus::Failed);

    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(kCapital).mDiagnosticIds,
              (std::vector<std::string>{"diagnostic-0001"}));

    EXPECT_EQ(result.report(kDividends).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(kDividends).mDiagnosticIds,
              (std::vector<std::string>{"diagnostic-0001"}));

    EXPECT_EQ(result.report(kBank).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(kBank).mDiagnosticIds, (std::vector<std::string>{"diagnostic-0001"}));

    EXPECT_EQ(result.report(kOther).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(kOther).mDiagnosticIds, (std::vector<std::string>{"diagnostic-0001"}));
}

TEST(ApplicationResultStatusTest, NeedsInputOutranksAFailedReportInTheOverallStatus) {
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mProcessingDiagnostics = {
        ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::IncompleteHistory,
            .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kCapital}},
            .mMessage = "Synthetic processing message.",
        },
        ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
            .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kDividends}},
            .mMessage = "Synthetic processing message.",
        },
    };
    input.mRequests = {RequestInput{
        .mQuestion =
            {
                .mType = RequestType::CapitalGainsFailureResolutionRequired,
                .mAffectedReports = {kCapital},
                .mData =
                    FailureResolutionQuestion{
                        .mInvestments = {FailedInvestment{
                            .mIsin = "XX0000000001",
                            .mName = "Synthetic Share",
                            .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
                        }}},
            },
        .mProcessingDiagnostics = {0},
    }};

    const auto result = ApplicationResultBuilder::build(std::move(input));

    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::NeedsInput);
    EXPECT_EQ(result.report(kDividends).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.mStatus, OverallStatus::NeedsInput);
}

TEST(ApplicationResultStatusTest, ABlockingErrorWithoutARequestFailsTheReportDespiteAnOpenRequest) {
    // The incomplete history has its request, but the conflicting duplicate has none: asking the
    // user about the first problem would produce no valid report.
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mMergeDiagnostics = {MergeDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = MergeDiagnosticCode::ConflictingDuplicate,
        .mMessage = "Synthetic merge message.",
        .mEventKinds = {StatementEventKind::Trade},
    }};
    input.mProcessingDiagnostics = {ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::IncompleteHistory,
        .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kCapital}},
        .mMessage = "Synthetic processing message.",
    }};
    input.mRequests = {RequestInput{
        .mQuestion =
            {
                .mType = RequestType::CapitalGainsFailureResolutionRequired,
                .mAffectedReports = {kCapital},
                .mData =
                    FailureResolutionQuestion{
                        .mInvestments = {FailedInvestment{
                            .mIsin = "XX0000000001",
                            .mName = "Synthetic Share",
                            .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
                        }}},
            },
        .mProcessingDiagnostics = {0},
    }};

    const auto result = ApplicationResultBuilder::build(std::move(input));

    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Failed);
    ASSERT_EQ(result.mRequests.size(), 1U);

    EXPECT_EQ(result.report(kCapital).mRequestIds, (std::vector<std::string>{"request-0001"}));
}

TEST(ApplicationResultStatusTest, ARequestOfOneReportDoesNotResolveTheErrorForAnotherReport) {
    // The error affects capital gains and dividends, but its request covers capital gains only.
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mProcessingDiagnostics = {ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
        .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kCapital, kDividends}},
        .mMessage = "Synthetic processing message.",
    }};
    input.mRequests = {RequestInput{
        .mQuestion =
            {
                .mType = RequestType::CapitalGainsFailureResolutionRequired,
                .mAffectedReports = {kCapital},
                .mData =
                    FailureResolutionQuestion{
                        .mInvestments = {FailedInvestment{
                            .mIsin = "XX0000000001",
                            .mName = "Synthetic Share",
                            .mReasonCodes = {ProcessingDiagnosticCode::ArithmeticOverflow},
                        }}},
            },
        .mProcessingDiagnostics = {0},
    }};
    input.mReports[1].mXml = "<dividends/>";

    const auto result = ApplicationResultBuilder::build(std::move(input));

    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::NeedsInput);
    EXPECT_EQ(result.report(kDividends).mStatus, ReportStatus::Failed);
    EXPECT_FALSE(result.report(kDividends).mXml.has_value());
    EXPECT_EQ(result.report(kDividends).mDiagnosticIds,
              (std::vector<std::string>{"diagnostic-0001"}));
    EXPECT_TRUE(result.report(kDividends).mRequestIds.empty());
    EXPECT_EQ(result.mStatus, OverallStatus::NeedsInput);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mRequestId, "request-0001");
}

TEST(ApplicationResultStatusTest, ALinkedRequestWithoutReportsResolvesNoError) {
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mProcessingDiagnostics = {ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::IncompleteHistory,
        .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kCapital}},
        .mMessage = "Synthetic processing message.",
    }};
    input.mRequests = {RequestInput{
        .mQuestion =
            {
                .mType = RequestType::CapitalGainsFailureResolutionRequired,
                .mAffectedReports = {},
                .mData =
                    FailureResolutionQuestion{
                        .mInvestments = {FailedInvestment{
                            .mIsin = "XX0000000001",
                            .mName = "Synthetic Share",
                            .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
                        }}},
            },
        .mProcessingDiagnostics = {0},
    }};
    input.mReports[0].mXml = "<capital/>";

    const auto result = ApplicationResultBuilder::build(std::move(input));

    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Failed);
    EXPECT_FALSE(result.report(kCapital).mXml.has_value());
    EXPECT_TRUE(result.report(kCapital).mRequestIds.empty());
    EXPECT_EQ(result.mStatus, OverallStatus::PartialSuccess);
}

TEST(ApplicationResultStatusTest, AnErrorLinkedToARequestOfEachReportNeedsInputInBoth) {
    // The first request in the final order is the one the diagnostic names.
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mProcessingDiagnostics = {ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
        .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kCapital, kOther}},
        .mMessage = "Synthetic processing message.",
    }};
    input.mRequests = {
        RequestInput{
            .mQuestion =
                {
                    .mType = RequestType::OtherInterestFailureResolutionRequired,
                    .mAffectedReports = {kOther},
                    .mData =
                        FailureResolutionQuestion{
                            .mInvestments = {FailedInvestment{
                                .mIsin = "XX0000000001",
                                .mName = "Synthetic Share",
                                .mReasonCodes = {ProcessingDiagnosticCode::ArithmeticOverflow},
                            }}},
                },
            .mProcessingDiagnostics = {0},
        },
        RequestInput{
            .mQuestion =
                {
                    .mType = RequestType::CapitalGainsFailureResolutionRequired,
                    .mAffectedReports = {kCapital},
                    .mData =
                        FailureResolutionQuestion{
                            .mInvestments = {FailedInvestment{
                                .mIsin = "XX0000000001",
                                .mName = "Synthetic Share",
                                .mReasonCodes = {ProcessingDiagnosticCode::ArithmeticOverflow},
                            }}},
                },
            .mProcessingDiagnostics = {0},
        },
    };

    const auto result = ApplicationResultBuilder::build(std::move(input));

    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::NeedsInput);
    EXPECT_EQ(result.report(kCapital).mRequestIds, (std::vector<std::string>{"request-0001"}));
    EXPECT_EQ(result.report(kOther).mStatus, ReportStatus::NeedsInput);
    EXPECT_EQ(result.report(kOther).mRequestIds, (std::vector<std::string>{"request-0002"}));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mRequestId, "request-0001");
}

TEST(ApplicationResultStatusTest, AWarningNeverBlocksAReport) {
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mMergeDiagnostics = {MergeDiagnostic{
        .mSeverity = DiagnosticSeverity::Warning,
        .mCode = MergeDiagnosticCode::DuplicateTransactionInFile,
        .mMessage = "Synthetic merge message.",
        .mEventKinds = {StatementEventKind::Trade},
    }};
    input.mReports[0].mXml = "<capital/>";

    const auto result = ApplicationResultBuilder::build(std::move(input));

    EXPECT_EQ(result.mStatus, OverallStatus::CompletedWithWarnings);
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kCapital).mXml, "<capital/>");

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_FALSE(result.mDiagnostics[0].mBlocking);
    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports, (std::vector{kCapital}));
}

TEST(ApplicationResultStatusTest, AnUnsupportedRowTypeIsVisibleButBlocksNoReport) {
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mMergeDiagnostics = {SourcedParseDiagnostic{
        .mSourceIndex = 0,
        .mBroker = Broker::TradeRepublic,
        .mDiagnostic =
            ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = DiagnosticCode::UnsupportedRowType,
                .mSourceFile = "export.csv",
                .mRowIndex = 7,
                .mMessage = "Synthetic parser message.",
                .mRowKind = std::nullopt,
            },
    }};
    input.mReports[0].mXml = "<capital/>";

    const auto result = ApplicationResultBuilder::build(std::move(input));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mSeverity, DiagnosticSeverity::Error);
    EXPECT_TRUE(result.mDiagnostics[0].mAffectedReports.empty());
    EXPECT_FALSE(result.mDiagnostics[0].mBlocking);

    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.mStatus, OverallStatus::CompletedWithWarnings);
}

TEST(ApplicationResultStatusTest, AnUnsupportedInterestSubtypeStillBlocksBothInterestForms) {
    // The row is an interest row, so the code unsupported_row_type does not free it from the
    // interest forms.
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mMergeDiagnostics = {SourcedParseDiagnostic{
        .mSourceIndex = 0,
        .mBroker = Broker::TradeRepublic,
        .mDiagnostic =
            ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = DiagnosticCode::UnsupportedRowType,
                .mSourceFile = "export.csv",
                .mRowIndex = 7,
                .mField = "type",
                .mMessage = "Synthetic parser message.",
                .mRowKind = ParsedRowKind::UnknownInterest,
            },
    }};
    input.mReports[0].mXml = "<capital/>";
    input.mReports[2].mXml = "<bank/>";

    const auto result = ApplicationResultBuilder::build(std::move(input));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports, (std::vector{kBank, kOther}));
    EXPECT_TRUE(result.mDiagnostics[0].mBlocking);

    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kBank).mStatus, ReportStatus::Failed);
    EXPECT_FALSE(result.report(kBank).mXml.has_value());
    EXPECT_EQ(result.report(kOther).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.mStatus, OverallStatus::PartialSuccess);
}

TEST(ApplicationResultStatusTest, NoDataKeepsItsReasonAndIsASuccess) {
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mReports[0].mNoDataReason = NoDataReason::NoSelectedYearDisposals;
    input.mReports[2].mNoDataReason = NoDataReason::BelowDepositInterestThreshold;

    const auto result = ApplicationResultBuilder::build(std::move(input));

    EXPECT_EQ(result.mStatus, OverallStatus::Success);
    EXPECT_EQ(result.report(kCapital).mNoDataReason, NoDataReason::NoSelectedYearDisposals);
    EXPECT_EQ(result.report(kBank).mNoDataReason, NoDataReason::BelowDepositInterestThreshold);
    EXPECT_FALSE(result.report(kDividends).mNoDataReason.has_value());
}

// Report scope of diagnostics. The rules are in diagnostics.md, "Failure scope".

TEST(ApplicationResultScopeTest, ARejectedTradeRowAffectsCapitalGainsOnly) {
    const SourcedParseDiagnostic diagnostic{
        .mSourceIndex = 0,
        .mBroker = Broker::TradeRepublic,
        .mDiagnostic =
            ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = DiagnosticCode::MissingField,
                .mSourceFile = "export.csv",
                .mRowIndex = 5,
                .mMessage = "Synthetic parser message.",
                .mRowKind = ParsedRowKind::Trade,
            },
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = {diagnostic}});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports, (std::vector{kCapital}));
}

TEST(ApplicationResultScopeTest, ARejectedCorporateActionRowAffectsCapitalGainsOnly) {
    const SourcedParseDiagnostic diagnostic{
        .mSourceIndex = 0,
        .mBroker = Broker::TradeRepublic,
        .mDiagnostic =
            ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = DiagnosticCode::MissingField,
                .mSourceFile = "export.csv",
                .mRowIndex = 5,
                .mMessage = "Synthetic parser message.",
                .mRowKind = ParsedRowKind::CorporateAction,
            },
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = {diagnostic}});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports, (std::vector{kCapital}));
}

TEST(ApplicationResultScopeTest, ARejectedDividendRowAffectsDividendsOnly) {
    const SourcedParseDiagnostic diagnostic{
        .mSourceIndex = 0,
        .mBroker = Broker::TradeRepublic,
        .mDiagnostic =
            ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = DiagnosticCode::MissingField,
                .mSourceFile = "export.csv",
                .mRowIndex = 5,
                .mMessage = "Synthetic parser message.",
                .mRowKind = ParsedRowKind::Dividend,
            },
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = {diagnostic}});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports, (std::vector{kDividends}));
}

TEST(ApplicationResultScopeTest, ARejectedBondInterestRowAffectsOtherInterestOnly) {
    const SourcedParseDiagnostic diagnostic{
        .mSourceIndex = 0,
        .mBroker = Broker::TradeRepublic,
        .mDiagnostic =
            ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = DiagnosticCode::MissingField,
                .mSourceFile = "export.csv",
                .mRowIndex = 5,
                .mMessage = "Synthetic parser message.",
                .mRowKind = ParsedRowKind::BondInterest,
            },
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = {diagnostic}});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports, (std::vector{kOther}));
}

TEST(ApplicationResultScopeTest, ATradeRepublicInterestRowIsDepositInterestFrom2024) {
    const SourcedParseDiagnostic diagnostic{
        .mSourceIndex = 0,
        .mBroker = Broker::TradeRepublic,
        .mDiagnostic =
            ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = DiagnosticCode::MissingField,
                .mSourceFile = "export.csv",
                .mRowIndex = 5,
                .mMessage = "Synthetic parser message.",
                .mRowKind = ParsedRowKind::BrokerInterest,
            },
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = {diagnostic}});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports, (std::vector{kBank}));
}

TEST(ApplicationResultScopeTest, ATradeRepublicInterestRowCanAlsoBeOtherInterestIn2023) {
    // The row has no trusted date; interest before 6 December 2023 belongs to Doh-Obr.
    const SourcedParseDiagnostic diagnostic{
        .mSourceIndex = 0,
        .mBroker = Broker::TradeRepublic,
        .mDiagnostic =
            ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = DiagnosticCode::MissingField,
                .mSourceFile = "export.csv",
                .mRowIndex = 5,
                .mMessage = "Synthetic parser message.",
                .mRowKind = ParsedRowKind::BrokerInterest,
            },
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2023, .mMergeDiagnostics = {diagnostic}});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports, (std::vector{kBank, kOther}));
}

TEST(ApplicationResultScopeTest, AnInteractiveBrokersInterestRowAffectsOtherInterestOnly) {
    const SourcedParseDiagnostic diagnostic{
        .mSourceIndex = 0,
        .mBroker = Broker::InteractiveBrokers,
        .mDiagnostic =
            ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = DiagnosticCode::MissingField,
                .mSourceFile = "export.csv",
                .mRowIndex = 5,
                .mMessage = "Synthetic parser message.",
                .mRowKind = ParsedRowKind::BrokerInterest,
            },
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = {diagnostic}});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports, (std::vector{kOther}));
}

TEST(ApplicationResultScopeTest, AnInterestRowOfUnknownSubtypeAffectsBothInterestForms) {
    const SourcedParseDiagnostic diagnostic{
        .mSourceIndex = 0,
        .mBroker = Broker::TradeRepublic,
        .mDiagnostic =
            ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = DiagnosticCode::MissingField,
                .mSourceFile = "export.csv",
                .mRowIndex = 5,
                .mMessage = "Synthetic parser message.",
                .mRowKind = ParsedRowKind::UnknownInterest,
            },
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = {diagnostic}});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports, (std::vector{kBank, kOther}));
}

TEST(ApplicationResultScopeTest, ARejectedPrivateMarketRowAffectsNoReport) {
    // Private-market events are skipped, so a rejected one changes no form.
    const SourcedParseDiagnostic diagnostic{
        .mSourceIndex = 0,
        .mBroker = Broker::TradeRepublic,
        .mDiagnostic =
            ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = DiagnosticCode::MissingField,
                .mSourceFile = "export.csv",
                .mRowIndex = 5,
                .mMessage = "Synthetic parser message.",
                .mRowKind = ParsedRowKind::PrivateMarket,
            },
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = {diagnostic}});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mSeverity, DiagnosticSeverity::Error);
    EXPECT_TRUE(result.mDiagnostics[0].mAffectedReports.empty());
    EXPECT_FALSE(result.mDiagnostics[0].mBlocking);
}

TEST(ApplicationResultScopeTest, ABenefitRowAffectsNoReport) {
    const SourcedParseDiagnostic diagnostic{
        .mSourceIndex = 0,
        .mBroker = Broker::TradeRepublic,
        .mDiagnostic =
            ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = DiagnosticCode::MissingField,
                .mSourceFile = "export.csv",
                .mRowIndex = 5,
                .mMessage = "Synthetic parser message.",
                .mRowKind = ParsedRowKind::Benefit,
            },
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = {diagnostic}});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_TRUE(result.mDiagnostics[0].mAffectedReports.empty());
    EXPECT_FALSE(result.mDiagnostics[0].mBlocking);
}

TEST(ApplicationResultScopeTest, ARowOfUnknownMeaningAffectsAllFourReports) {
    // No row kind: for example a row with an unknown type.
    const SourcedParseDiagnostic diagnostic{
        .mSourceIndex = 0,
        .mBroker = Broker::TradeRepublic,
        .mDiagnostic =
            ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = DiagnosticCode::UnknownRowType,
                .mSourceFile = "export.csv",
                .mRowIndex = 5,
                .mMessage = "Synthetic parser message.",
                .mRowKind = std::nullopt,
            },
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = {diagnostic}});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports,
              (std::vector{kCapital, kDividends, kBank, kOther}));
}

TEST(ApplicationResultScopeTest, AnInterestRowFromAnUnknownBrokerAffectsBothInterestForms) {
    const SourcedParseDiagnostic diagnostic{
        .mSourceIndex = 0,
        .mBroker = Broker::Unknown,
        .mDiagnostic =
            ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = DiagnosticCode::MissingField,
                .mSourceFile = "export.csv",
                .mRowIndex = 5,
                .mMessage = "Synthetic parser message.",
                .mRowKind = ParsedRowKind::BrokerInterest,
            },
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = {diagnostic}});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports, (std::vector{kBank, kOther}));
}

TEST(ApplicationResultScopeTest, AConflictingTradeAffectsCapitalGainsOnly) {
    const MergeDiagnostic diagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = MergeDiagnosticCode::ConflictingDuplicate,
        .mMessage = "Synthetic merge message.",
        .mEventKinds = {StatementEventKind::Trade},
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = {diagnostic}});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports, (std::vector{kCapital}));
}

TEST(ApplicationResultScopeTest, AConflictingDividendAffectsDividendsOnly) {
    const MergeDiagnostic diagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = MergeDiagnosticCode::ConflictingDuplicate,
        .mMessage = "Synthetic merge message.",
        .mEventKinds = {StatementEventKind::Dividend},
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = {diagnostic}});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports, (std::vector{kDividends}));
}

TEST(ApplicationResultScopeTest, AConflictingTradeRepublicInterestAffectsBothInterestForms) {
    // The merger does not say whether it is a bond coupon or deposit interest.
    const MergeDiagnostic diagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = MergeDiagnosticCode::ConflictingDuplicate,
        .mMessage = "Synthetic merge message.",
        .mSources = {SourceReference{
            .mBroker = Broker::TradeRepublic,
            .mFilename = SourceFilename::fromPath("a.csv"),
            .mSourceRow = 3,
            .mTransactionId = std::nullopt,
            .mInputSequence = {0, 1},
        }},
        .mEventKinds = {StatementEventKind::Interest},
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = {diagnostic}});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports, (std::vector{kBank, kOther}));
}

TEST(ApplicationResultScopeTest, AConflictingInteractiveBrokersInterestAffectsOtherInterest) {
    const MergeDiagnostic diagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = MergeDiagnosticCode::ConflictingDuplicate,
        .mMessage = "Synthetic merge message.",
        .mSources = {SourceReference{
            .mBroker = Broker::InteractiveBrokers,
            .mFilename = SourceFilename::fromPath("a.csv"),
            .mSourceRow = 3,
            .mTransactionId = std::nullopt,
            .mInputSequence = {0, 1},
        }},
        .mEventKinds = {StatementEventKind::Interest},
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = {diagnostic}});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports, (std::vector{kOther}));
}

TEST(ApplicationResultScopeTest, AConflictingInterestWithoutSourcesAffectsBothInterestForms) {
    // Without a source the broker is not known, so it is not only Interactive Brokers.
    const MergeDiagnostic diagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = MergeDiagnosticCode::ConflictingDuplicate,
        .mMessage = "Synthetic merge message.",
        .mSources = {},
        .mEventKinds = {StatementEventKind::Interest},
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = {diagnostic}});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports, (std::vector{kBank, kOther}));
}

TEST(ApplicationResultScopeTest, AConflictingBenefitAffectsNoReport) {
    const MergeDiagnostic diagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = MergeDiagnosticCode::ConflictingDuplicate,
        .mMessage = "Synthetic merge message.",
        .mEventKinds = {StatementEventKind::Benefit},
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = {diagnostic}});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_TRUE(result.mDiagnostics[0].mAffectedReports.empty());
}

TEST(ApplicationResultScopeTest, AConflictingPrivateMarketEventAffectsNoReport) {
    // Private-market events are skipped, so a rejected one changes no form.
    const MergeDiagnostic diagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = MergeDiagnosticCode::ConflictingDuplicate,
        .mMessage = "Synthetic merge message.",
        .mEventKinds = {StatementEventKind::PrivateMarket},
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = {diagnostic}});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_TRUE(result.mDiagnostics[0].mAffectedReports.empty());
    EXPECT_FALSE(result.mDiagnostics[0].mBlocking);
}

TEST(ApplicationResultScopeTest, AConflictingAssetClassAffectsCapitalGainsOnly) {
    const MergeDiagnostic diagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = MergeDiagnosticCode::InstrumentAssetClassConflict,
        .mMessage = "Synthetic merge message.",
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = {diagnostic}});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports, (std::vector{kCapital}));
}

TEST(ApplicationResultScopeTest, ANameConflictOfADividendInstrumentAffectsDividendsOnly) {
    // The merger gives a name conflict the kind of the instrument's collection.
    const MergeDiagnostic diagnostic{
        .mSeverity = DiagnosticSeverity::Warning,
        .mCode = MergeDiagnosticCode::InstrumentNameConflict,
        .mMessage = "Synthetic merge message.",
        .mIsin = "XX0000000001",
        .mEventKinds = {StatementEventKind::Dividend},
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = {diagnostic}});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports, (std::vector{kDividends}));
}

TEST(ApplicationResultScopeTest, AMergeDiagnosticWithoutEventKindsAffectsAllFourReports) {
    const MergeDiagnostic diagnostic{
        .mSeverity = DiagnosticSeverity::Warning,
        .mCode = MergeDiagnosticCode::InstrumentNameConflict,
        .mMessage = "Synthetic merge message.",
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = {diagnostic}});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports,
              (std::vector{kCapital, kDividends, kBank, kOther}));
}

TEST(ApplicationResultScopeTest, AConflictAcrossKindsAffectsEveryKindInTheFixedOrder) {
    const MergeDiagnostic diagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = MergeDiagnosticCode::ConflictingDuplicate,
        .mMessage = "Synthetic merge message.",
        .mEventKinds = {StatementEventKind::Dividend, StatementEventKind::Trade},
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = {diagnostic}});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports, (std::vector{kCapital, kDividends}));
}

TEST(ApplicationResultScopeTest, ADuplicateSourceIndexAffectsAllFourReports) {
    const MergeDiagnostic diagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = MergeDiagnosticCode::DuplicateSourceIndex,
        .mMessage = "Synthetic merge message.",
        .mSourceIndex = 1,
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = {diagnostic}});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports,
              (std::vector{kCapital, kDividends, kBank, kOther}));
}

TEST(ApplicationResultScopeTest, ProcessingReportsAreSortedAndDeduplicated) {
    const ProcessingDiagnostic diagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
        .mContext = {.mStage = PipelineStage::FifoTax,
                     .mAffectedReports = {kOther, kCapital, kCapital}},
        .mMessage = "Synthetic processing message.",
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mProcessingDiagnostics = {diagnostic}});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports, (std::vector{kCapital, kOther}));
}

// Deterministic order and IDs. The keys are listed in diagnostics.md, "Deterministic ordering".

TEST(ApplicationResultOrderTest, StageComesBeforeEverythingElse) {
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mMergeDiagnostics = {
        MergeDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = MergeDiagnosticCode::ConflictingDuplicate,
            .mMessage = "Synthetic merge message.",
            .mEventKinds = {StatementEventKind::Trade},
        },
        SourcedParseDiagnostic{
            .mSourceIndex = 3,
            .mBroker = Broker::TradeRepublic,
            .mDiagnostic =
                ParseDiagnostic{
                    .mSeverity = DiagnosticSeverity::Error,
                    .mCode = DiagnosticCode::MissingField,
                    .mSourceFile = "export.csv",
                    .mRowIndex = 9,
                    .mMessage = "Synthetic parser message.",
                    .mRowKind = ParsedRowKind::Trade,
                },
        },
    };
    input.mProcessingDiagnostics = {ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
        .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kCapital}},
        .mMessage = "Synthetic processing message.",
    }};

    const auto result = ApplicationResultBuilder::build(std::move(input));

    ASSERT_EQ(result.mDiagnostics.size(), 3U);

    EXPECT_EQ(result.mDiagnostics[0].mStage, PipelineStage::Parsing);
    EXPECT_EQ(result.mDiagnostics[1].mStage, PipelineStage::Merging);
    EXPECT_EQ(result.mDiagnostics[2].mStage, PipelineStage::FifoTax);
    EXPECT_EQ(result.mDiagnostics[0].mId, "diagnostic-0001");
    EXPECT_EQ(result.mDiagnostics[1].mId, "diagnostic-0002");
    EXPECT_EQ(result.mDiagnostics[2].mId, "diagnostic-0003");
}

TEST(ApplicationResultOrderTest, TheEarlierFileComesFirst) {
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mMergeDiagnostics = {
        SourcedParseDiagnostic{
            .mSourceIndex = 1,
            .mBroker = Broker::TradeRepublic,
            .mDiagnostic =
                ParseDiagnostic{
                    .mSeverity = DiagnosticSeverity::Error,
                    .mCode = DiagnosticCode::MissingField,
                    .mSourceFile = "export.csv",
                    .mRowIndex = 3,
                    .mMessage = "Synthetic parser message.",
                    .mRowKind = ParsedRowKind::Trade,
                },
        },
        SourcedParseDiagnostic{
            .mSourceIndex = 0,
            .mBroker = Broker::TradeRepublic,
            .mDiagnostic =
                ParseDiagnostic{
                    .mSeverity = DiagnosticSeverity::Error,
                    .mCode = DiagnosticCode::MissingField,
                    .mSourceFile = "export.csv",
                    .mRowIndex = 9,
                    .mMessage = "Synthetic parser message.",
                    .mRowKind = ParsedRowKind::Trade,
                },
        },
    };

    const auto result = ApplicationResultBuilder::build(std::move(input));

    ASSERT_EQ(result.mDiagnostics.size(), 2U);

    EXPECT_EQ(result.mDiagnostics[0].mSources.at(0).mSourceIndex, 0U);
    EXPECT_EQ(result.mDiagnostics[1].mSources.at(0).mSourceIndex, 1U);
}

TEST(ApplicationResultOrderTest, TheEarlierRowOfOneFileComesFirst) {
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mMergeDiagnostics = {
        SourcedParseDiagnostic{
            .mSourceIndex = 0,
            .mBroker = Broker::TradeRepublic,
            .mDiagnostic =
                ParseDiagnostic{
                    .mSeverity = DiagnosticSeverity::Error,
                    .mCode = DiagnosticCode::MissingField,
                    .mSourceFile = "export.csv",
                    .mRowIndex = 7,
                    .mMessage = "Synthetic parser message.",
                    .mRowKind = ParsedRowKind::Trade,
                },
        },
        SourcedParseDiagnostic{
            .mSourceIndex = 0,
            .mBroker = Broker::TradeRepublic,
            .mDiagnostic =
                ParseDiagnostic{
                    .mSeverity = DiagnosticSeverity::Error,
                    .mCode = DiagnosticCode::MissingField,
                    .mSourceFile = "export.csv",
                    .mRowIndex = 3,
                    .mMessage = "Synthetic parser message.",
                    .mRowKind = ParsedRowKind::Trade,
                },
        },
    };

    const auto result = ApplicationResultBuilder::build(std::move(input));

    ASSERT_EQ(result.mDiagnostics.size(), 2U);

    EXPECT_EQ(result.mDiagnostics[0].mSources.at(0).mRow, 3U);
    EXPECT_EQ(result.mDiagnostics[1].mSources.at(0).mRow, 7U);
}

TEST(ApplicationResultOrderTest, ADiagnosticOnTheHeaderRowComesAfterTheDataRowsOfItsFile) {
    // Row 1 is the header, so it has no event; a source without an event sorts last in its file.
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mMergeDiagnostics = {
        SourcedParseDiagnostic{
            .mSourceIndex = 0,
            .mBroker = Broker::TradeRepublic,
            .mDiagnostic =
                ParseDiagnostic{
                    .mSeverity = DiagnosticSeverity::Error,
                    .mCode = DiagnosticCode::InvalidHeader,
                    .mSourceFile = "export.csv",
                    .mRowIndex = 1,
                    .mMessage = "Synthetic parser message.",
                    .mRowKind = std::nullopt,
                },
        },
        SourcedParseDiagnostic{
            .mSourceIndex = 0,
            .mBroker = Broker::TradeRepublic,
            .mDiagnostic =
                ParseDiagnostic{
                    .mSeverity = DiagnosticSeverity::Error,
                    .mCode = DiagnosticCode::MissingField,
                    .mSourceFile = "export.csv",
                    .mRowIndex = 2,
                    .mMessage = "Synthetic parser message.",
                    .mRowKind = ParsedRowKind::Trade,
                },
        },
    };

    const auto result = ApplicationResultBuilder::build(std::move(input));

    ASSERT_EQ(result.mDiagnostics.size(), 2U);

    EXPECT_EQ(result.mDiagnostics[0].mSources.at(0).mRow, 2U);
    EXPECT_EQ(result.mDiagnostics[0].mSources.at(0).mEventIndex, 0U); // row 2 is event 0
    EXPECT_EQ(result.mDiagnostics[1].mSources.at(0).mRow, 1U);
    EXPECT_FALSE(result.mDiagnostics[1].mSources.at(0).mEventIndex.has_value());
}

TEST(ApplicationResultOrderTest, TheTenThousandthDiagnosticGetsAFiveDigitId) {
    // IDs have at least four digits; a fifth digit is added, never cut off.
    const ProcessingDiagnostic diagnostic{
        .mSeverity = DiagnosticSeverity::Warning,
        .mCode = ProcessingDiagnosticCode::UnrepresentableValue,
        .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kCapital}},
        .mMessage = "Synthetic processing message.",
    };
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mProcessingDiagnostics = std::vector<ProcessingDiagnostic>(10'000, diagnostic);

    const auto result = ApplicationResultBuilder::build(std::move(input));

    ASSERT_EQ(result.mDiagnostics.size(), 10'000U);

    EXPECT_EQ(result.mDiagnostics[9'998].mId, "diagnostic-9999");
    EXPECT_EQ(result.mDiagnostics[9'999].mId, "diagnostic-10000");
}

TEST(ApplicationResultOrderTest, ADiagnosticWithoutASourceComesAfterOneWithASource) {
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mProcessingDiagnostics = {
        ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
            .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kCapital}},
            .mMessage = "Synthetic processing message.",
        },
        ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::UnrepresentableValue,
            .mContext = {.mStage = PipelineStage::FifoTax,
                         .mAffectedReports = {kCapital},
                         .mSources = {SourceReference{
                             .mBroker = Broker::TradeRepublic,
                             .mFilename = SourceFilename::fromPath("a.csv"),
                             .mSourceRow = 4,
                             .mTransactionId = std::nullopt,
                             .mInputSequence = {5, 2},
                         }}},
            .mMessage = "Synthetic processing message.",
        },
    };

    const auto result = ApplicationResultBuilder::build(std::move(input));

    ASSERT_EQ(result.mDiagnostics.size(), 2U);

    EXPECT_EQ(std::get<ProcessingDiagnosticCode>(result.mDiagnostics[0].mCode),
              ProcessingDiagnosticCode::UnrepresentableValue);
    EXPECT_EQ(std::get<ProcessingDiagnosticCode>(result.mDiagnostics[1].mCode),
              ProcessingDiagnosticCode::ArithmeticOverflow);
}

TEST(ApplicationResultOrderTest, TheEarlierTaxDateComesFirstAndNoDateLast) {
    const ProcessingDiagnostic none{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
        .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kCapital}},
        .mMessage = "Synthetic processing message.",
    };
    const ProcessingDiagnostic march{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
        .mContext = {.mStage = PipelineStage::FifoTax,
                     .mAffectedReports = {kCapital},
                     .mTaxDate = day(2024, 3, 1)},
        .mMessage = "Synthetic processing message.",
    };
    const ProcessingDiagnostic january{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
        .mContext = {.mStage = PipelineStage::FifoTax,
                     .mAffectedReports = {kCapital},
                     .mTaxDate = day(2024, 1, 15)},
        .mMessage = "Synthetic processing message.",
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mProcessingDiagnostics = {none, march, january}});

    ASSERT_EQ(result.mDiagnostics.size(), 3U);

    EXPECT_EQ(result.mDiagnostics[0].mTaxDate, day(2024, 1, 15));
    EXPECT_EQ(result.mDiagnostics[1].mTaxDate, day(2024, 3, 1));
    EXPECT_FALSE(result.mDiagnostics[2].mTaxDate.has_value());
}

TEST(ApplicationResultOrderTest, ReportOrderIsCapitalDividendsBankOtherThenNone) {
    const ProcessingDiagnostic none{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
        .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {}},
        .mMessage = "Synthetic processing message.",
    };
    const ProcessingDiagnostic other{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
        .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kOther}},
        .mMessage = "Synthetic processing message.",
    };
    const ProcessingDiagnostic bank{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
        .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kBank}},
        .mMessage = "Synthetic processing message.",
    };
    const ProcessingDiagnostic dividends{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
        .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kDividends}},
        .mMessage = "Synthetic processing message.",
    };
    const ProcessingDiagnostic capital{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
        .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kCapital}},
        .mMessage = "Synthetic processing message.",
    };

    const auto result = ApplicationResultBuilder::build(ApplicationResultInput{
        .mProcessingDiagnostics = {none, other, bank, dividends, capital},
    });

    ASSERT_EQ(result.mDiagnostics.size(), 5U);

    EXPECT_EQ(result.mDiagnostics[0].mAffectedReports, (std::vector{kCapital}));
    EXPECT_EQ(result.mDiagnostics[1].mAffectedReports, (std::vector{kDividends}));
    EXPECT_EQ(result.mDiagnostics[2].mAffectedReports, (std::vector{kBank}));
    EXPECT_EQ(result.mDiagnostics[3].mAffectedReports, (std::vector{kOther}));
    EXPECT_TRUE(result.mDiagnostics[4].mAffectedReports.empty());
}

TEST(ApplicationResultOrderTest, TheIsinComesNextAndNoIsinLast) {
    const ProcessingDiagnostic none{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
        .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kCapital}},
        .mMessage = "Synthetic processing message.",
    };
    const ProcessingDiagnostic second{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
        .mContext = {.mStage = PipelineStage::FifoTax,
                     .mAffectedReports = {kCapital},
                     .mIsin = "XX0000000002"},
        .mMessage = "Synthetic processing message.",
    };
    const ProcessingDiagnostic first{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
        .mContext = {.mStage = PipelineStage::FifoTax,
                     .mAffectedReports = {kCapital},
                     .mIsin = "XX0000000001"},
        .mMessage = "Synthetic processing message.",
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mProcessingDiagnostics = {none, second, first}});

    ASSERT_EQ(result.mDiagnostics.size(), 3U);

    EXPECT_EQ(result.mDiagnostics[0].mIsin, "XX0000000001");
    EXPECT_EQ(result.mDiagnostics[1].mIsin, "XX0000000002");
    EXPECT_FALSE(result.mDiagnostics[2].mIsin.has_value());
}

TEST(ApplicationResultOrderTest, AnErrorComesBeforeAWarning) {
    const ProcessingDiagnostic warning{
        .mSeverity = DiagnosticSeverity::Warning,
        .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
        .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kCapital}},
        .mMessage = "Synthetic processing message.",
    };
    const ProcessingDiagnostic error{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
        .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kCapital}},
        .mMessage = "Synthetic processing message.",
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mProcessingDiagnostics = {warning, error}});

    ASSERT_EQ(result.mDiagnostics.size(), 2U);

    EXPECT_EQ(result.mDiagnostics[0].mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(result.mDiagnostics[1].mSeverity, DiagnosticSeverity::Warning);
}

TEST(ApplicationResultOrderTest, TheCodeOrdersDiagnosticsOfTheSamePlace) {
    // ArithmeticOverflow is declared before IncompleteHistory.
    const ProcessingDiagnostic incomplete{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::IncompleteHistory,
        .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kCapital}},
        .mMessage = "Synthetic processing message.",
    };
    const ProcessingDiagnostic overflow{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
        .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kCapital}},
        .mMessage = "Synthetic processing message.",
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mProcessingDiagnostics = {incomplete, overflow}});

    ASSERT_EQ(result.mDiagnostics.size(), 2U);

    EXPECT_EQ(std::get<ProcessingDiagnosticCode>(result.mDiagnostics[0].mCode),
              ProcessingDiagnosticCode::ArithmeticOverflow);
    EXPECT_EQ(std::get<ProcessingDiagnosticCode>(result.mDiagnostics[1].mCode),
              ProcessingDiagnosticCode::IncompleteHistory);
}

TEST(ApplicationResultOrderTest, EqualDiagnosticsKeepTheirCreationOrder) {
    const ProcessingDiagnostic first{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
        .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kCapital}},
        .mMessage = "First created.",
    };
    const ProcessingDiagnostic second{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
        .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kCapital}},
        .mMessage = "Second created.",
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mProcessingDiagnostics = {first, second}});

    ASSERT_EQ(result.mDiagnostics.size(), 2U);

    EXPECT_EQ(result.mDiagnostics[0].mMessage, "First created.");
    EXPECT_EQ(result.mDiagnostics[1].mMessage, "Second created.");
}

TEST(ApplicationResultOrderTest, ParserDiagnosticsOfDifferentFilesGiveTheSameIdsInAnyInputOrder) {
    const SourcedParseDiagnostic fromFirstFile{
        .mSourceIndex = 0,
        .mBroker = Broker::TradeRepublic,
        .mDiagnostic =
            ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = DiagnosticCode::MissingField,
                .mSourceFile = "first.csv",
                .mRowIndex = 4,
                .mMessage = "Synthetic parser message.",
                .mRowKind = ParsedRowKind::Trade,
            },
    };
    const SourcedParseDiagnostic fromSecondFile{
        .mSourceIndex = 1,
        .mBroker = Broker::TradeRepublic,
        .mDiagnostic =
            ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = DiagnosticCode::MissingField,
                .mSourceFile = "second.csv",
                .mRowIndex = 4,
                .mMessage = "Synthetic parser message.",
                .mRowKind = ParsedRowKind::Dividend,
            },
    };

    const auto forward = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024,
                               .mMergeDiagnostics = {fromFirstFile, fromSecondFile}});
    const auto backward = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024,
                               .mMergeDiagnostics = {fromSecondFile, fromFirstFile}});

    ASSERT_EQ(forward.mDiagnostics.size(), 2U);

    EXPECT_EQ(forward.mDiagnostics[0].mId, "diagnostic-0001");
    EXPECT_EQ(forward.mDiagnostics[0].mSources.at(0).mFilename->value(), "first.csv");
    EXPECT_EQ(forward.mDiagnostics[0].mAffectedReports, (std::vector{kCapital}));
    EXPECT_EQ(forward.mDiagnostics[1].mId, "diagnostic-0002");
    EXPECT_EQ(forward.mDiagnostics[1].mSources.at(0).mFilename->value(), "second.csv");
    EXPECT_EQ(forward.mDiagnostics[1].mAffectedReports, (std::vector{kDividends}));

    ASSERT_EQ(backward.mDiagnostics.size(), 2U);

    EXPECT_EQ(backward.mDiagnostics[0].mId, "diagnostic-0001");
    EXPECT_EQ(backward.mDiagnostics[0].mSources.at(0).mFilename->value(), "first.csv");
    EXPECT_EQ(backward.mDiagnostics[0].mAffectedReports, (std::vector{kCapital}));
    EXPECT_EQ(backward.mDiagnostics[1].mId, "diagnostic-0002");
    EXPECT_EQ(backward.mDiagnostics[1].mSources.at(0).mFilename->value(), "second.csv");
    EXPECT_EQ(backward.mDiagnostics[1].mAffectedReports, (std::vector{kDividends}));
}

TEST(ApplicationResultOrderTest, RequestsAreOrderedAndNumberedAfterTheSort) {
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mRequests = {
        RequestInput{
            .mQuestion =
                {
                    .mType = RequestType::IncomeDetailsRequired,
                    .mAffectedReports = {kOther, kBank},
                    .mIsin = "XX0000000002",
                },
        },
        RequestInput{
            .mQuestion =
                {
                    .mType = RequestType::TaxClassificationRequired,
                    .mAffectedReports = {kCapital},
                    .mIsin = "XX0000000001",
                },
        },
    };

    const auto result = ApplicationResultBuilder::build(std::move(input));

    ASSERT_EQ(result.mRequests.size(), 2U);

    EXPECT_EQ(result.mRequests[0].mId, "request-0001");
    EXPECT_EQ(result.mRequests[0].mType, RequestType::TaxClassificationRequired);
    EXPECT_EQ(result.mRequests[1].mId, "request-0002");
    EXPECT_EQ(result.mRequests[1].mType, RequestType::IncomeDetailsRequired);
    // The reports of a request are in the fixed order too.
    EXPECT_EQ(result.mRequests[1].mAffectedReports, (std::vector{kBank, kOther}));
    EXPECT_EQ(result.report(kCapital).mRequestIds, (std::vector<std::string>{"request-0001"}));
    EXPECT_EQ(result.report(kBank).mRequestIds, (std::vector<std::string>{"request-0002"}));
}

// Safe sources.

TEST(ApplicationResultSourceTest, ASourceHasOnlyTheFilenameAndNeverAPath) {
    const SourcedParseDiagnostic posix{
        .mSourceIndex = 0,
        .mBroker = Broker::TradeRepublic,
        .mDiagnostic =
            ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = DiagnosticCode::MissingField,
                .mSourceFile = "/home/user/2023/taxreport.csv",
                .mRowIndex = 4,
                .mMessage = "Synthetic parser message.",
                .mRowKind = ParsedRowKind::Trade,
            },
    };
    const SourcedParseDiagnostic windows{
        .mSourceIndex = 1,
        .mBroker = Broker::TradeRepublic,
        .mDiagnostic =
            ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = DiagnosticCode::MissingField,
                .mSourceFile = R"(C:\Users\user\2022\taxreport.csv)",
                .mRowIndex = 4,
                .mMessage = "Synthetic parser message.",
                .mRowKind = ParsedRowKind::Trade,
            },
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = {posix, windows}});

    ASSERT_EQ(result.mDiagnostics.size(), 2U);

    EXPECT_EQ(result.mDiagnostics[0].mSources.at(0).mFilename->value(), "taxreport.csv");
    EXPECT_EQ(result.mDiagnostics[1].mSources.at(0).mFilename->value(), "taxreport.csv");
}

TEST(ApplicationResultSourceTest, ANameWithoutAFilenameIsLeftOutAndTheSourceIndexRemains) {
    const SourcedParseDiagnostic diagnostic{
        .mSourceIndex = 2,
        .mBroker = Broker::TradeRepublic,
        .mDiagnostic =
            ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = DiagnosticCode::MissingField,
                .mSourceFile = "exports/",
                .mRowIndex = 4,
                .mMessage = "Synthetic parser message.",
                .mRowKind = ParsedRowKind::Trade,
            },
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = {diagnostic}});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    ASSERT_EQ(result.mDiagnostics[0].mSources.size(), 1U);

    const auto& source = result.mDiagnostics[0].mSources[0];

    EXPECT_FALSE(source.mFilename.has_value());
    EXPECT_EQ(source.mSourceIndex, 2U);
    EXPECT_EQ(source.mRow, 4U);
}

TEST(ApplicationResultSourceTest, TheSourcesOfAConflictAreSortedByRequestIndexThenEventIndex) {
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mMergeDiagnostics = {MergeDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = MergeDiagnosticCode::ConflictingDuplicate,
        .mMessage = "Synthetic merge message.",
        .mSources =
            {
                SourceReference{
                    .mBroker = Broker::TradeRepublic,
                    .mFilename = SourceFilename::fromPath("b.csv"),
                    .mSourceRow = 9,
                    .mTransactionId = std::nullopt,
                    .mInputSequence = {2, 7},
                },
                SourceReference{
                    .mBroker = Broker::TradeRepublic,
                    .mFilename = SourceFilename::fromPath("a.csv"),
                    .mSourceRow = 8,
                    .mTransactionId = std::nullopt,
                    .mInputSequence = {0, 6},
                },
                SourceReference{
                    .mBroker = Broker::TradeRepublic,
                    .mFilename = SourceFilename::fromPath("a.csv"),
                    .mSourceRow = 4,
                    .mTransactionId = std::nullopt,
                    .mInputSequence = {0, 2},
                },
            },
        .mEventKinds = {StatementEventKind::Trade},
    }};

    const auto result = ApplicationResultBuilder::build(std::move(input));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& sources = result.mDiagnostics[0].mSources;

    ASSERT_EQ(sources.size(), 3U);

    EXPECT_EQ(sources[0].mSourceIndex, 0U);
    EXPECT_EQ(sources[0].mRow, 4U);
    EXPECT_EQ(sources[1].mSourceIndex, 0U);
    EXPECT_EQ(sources[1].mRow, 8U);
    EXPECT_EQ(sources[2].mSourceIndex, 2U);
    EXPECT_EQ(sources[2].mRow, 9U);
}

TEST(ApplicationResultSourceTest, ARepeatedSourceIsDroppedEvenWithAnotherSourceAtTheSamePlace) {
    // The first and third sources are the same; the second one differs only in its transaction
    // ID, so all three have the same file, event and row.
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mMergeDiagnostics = {MergeDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = MergeDiagnosticCode::ConflictingDuplicate,
        .mMessage = "Synthetic merge message.",
        .mSources =
            {
                SourceReference{
                    .mBroker = Broker::TradeRepublic,
                    .mFilename = SourceFilename::fromPath("a.csv"),
                    .mSourceRow = 4,
                    .mTransactionId = "synthetic-b",
                    .mInputSequence = {0, 2},
                },
                SourceReference{
                    .mBroker = Broker::TradeRepublic,
                    .mFilename = SourceFilename::fromPath("a.csv"),
                    .mSourceRow = 4,
                    .mTransactionId = "synthetic-a",
                    .mInputSequence = {0, 2},
                },
                SourceReference{
                    .mBroker = Broker::TradeRepublic,
                    .mFilename = SourceFilename::fromPath("a.csv"),
                    .mSourceRow = 4,
                    .mTransactionId = "synthetic-b",
                    .mInputSequence = {0, 2},
                },
            },
        .mEventKinds = {StatementEventKind::Trade},
    }};

    const auto result = ApplicationResultBuilder::build(std::move(input));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& sources = result.mDiagnostics[0].mSources;

    ASSERT_EQ(sources.size(), 2U);

    EXPECT_EQ(sources[0].mTransactionId, "synthetic-a");
    EXPECT_EQ(sources[1].mTransactionId, "synthetic-b");
}

TEST(ApplicationResultSourceTest, AtTheSamePlaceNoFilenameComesFirstAndNoTransactionIdLast) {
    // All three sources are row 4 (event 2) of the first file. The sort key compares the filename
    // as text, an absent one as empty text, and puts an absent transaction ID last.
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mMergeDiagnostics = {MergeDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = MergeDiagnosticCode::ConflictingDuplicate,
        .mMessage = "Synthetic merge message.",
        .mSources =
            {
                SourceReference{
                    .mBroker = Broker::TradeRepublic,
                    .mFilename = SourceFilename::fromPath("a.csv"),
                    .mSourceRow = 4,
                    .mTransactionId = std::nullopt,
                    .mInputSequence = {0, 2},
                },
                SourceReference{
                    .mBroker = Broker::TradeRepublic,
                    .mFilename = SourceFilename::fromPath("a.csv"),
                    .mSourceRow = 4,
                    .mTransactionId = "synthetic-a",
                    .mInputSequence = {0, 2},
                },
                SourceReference{
                    .mBroker = Broker::TradeRepublic,
                    .mFilename = SourceFilename{}, // empty: not a plain filename
                    .mSourceRow = 4,
                    .mTransactionId = "synthetic-a",
                    .mInputSequence = {0, 2},
                },
            },
        .mEventKinds = {StatementEventKind::Trade},
    }};

    const auto result = ApplicationResultBuilder::build(std::move(input));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& sources = result.mDiagnostics[0].mSources;

    ASSERT_EQ(sources.size(), 3U);

    EXPECT_FALSE(sources[0].mFilename.has_value());
    EXPECT_EQ(sources[0].mTransactionId, "synthetic-a");
    EXPECT_EQ(sources[1].mFilename->value(), "a.csv");
    EXPECT_EQ(sources[1].mTransactionId, "synthetic-a");
    EXPECT_EQ(sources[2].mFilename->value(), "a.csv");
    EXPECT_FALSE(sources[2].mTransactionId.has_value());
}

TEST(ApplicationResultSourceTest, AParserDiagnosticKeepsItsCodeSeverityKeyAndMessage) {
    const SourcedParseDiagnostic diagnostic{
        .mSourceIndex = 0,
        .mBroker = Broker::TradeRepublic,
        .mDiagnostic =
            ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Warning,
                .mCode = DiagnosticCode::MissingField,
                .mSourceFile = "export.csv",
                .mRowIndex = 12,
                .mTransactionId = "synthetic-id",
                .mField = "name",
                .mMessage = "The name is blank.",
                .mMessageKey = MessageKey::MissingFieldName,
                .mRowKind = ParsedRowKind::Trade,
            },
    };

    const auto result = ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = {diagnostic}});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& adapted = result.mDiagnostics[0];

    EXPECT_EQ(adapted.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(std::get<DiagnosticCode>(adapted.mCode), DiagnosticCode::MissingField);
    EXPECT_EQ(adapted.mStage, PipelineStage::Parsing);
    EXPECT_EQ(adapted.mMessage, "The name is blank.");
    EXPECT_EQ(adapted.mMessageKey, MessageKey::MissingFieldName);
    EXPECT_EQ(adapted.mField, "name");
    EXPECT_FALSE(adapted.mBlocking);

    ASSERT_EQ(adapted.mSources.size(), 1U);

    EXPECT_EQ(adapted.mSources[0].mRow, 12U);
    EXPECT_EQ(adapted.mSources[0].mTransactionId, "synthetic-id");
}

// Question data of decision requests.

TEST(ApplicationResultRequestTest, AFailureRequestListsItsInvestmentsSorted) {
    // Two incomplete-history errors; the stage lists the investments out of ISIN order and with a
    // repeated reason code.
    ApplicationResultInput input{.mReportingYear = 2024, .mRunRevision = 7};
    input.mProcessingDiagnostics = {
        ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::IncompleteHistory,
            .mContext = {.mStage = PipelineStage::FifoTax,
                         .mAffectedReports = {kCapital},
                         .mIsin = "XX0000000002"},
            .mMessage = "Synthetic processing message.",
        },
        ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::IncompleteHistory,
            .mContext = {.mStage = PipelineStage::FifoTax,
                         .mAffectedReports = {kCapital},
                         .mIsin = "XX0000000001"},
            .mMessage = "Synthetic processing message.",
        },
    };
    input.mRequests = {RequestInput{
        .mQuestion =
            {
                .mType = RequestType::CapitalGainsFailureResolutionRequired,
                .mAffectedReports = {kCapital},
                .mData =
                    FailureResolutionQuestion{
                        .mInvestments =
                            {
                                FailedInvestment{
                                    .mIsin = "XX0000000002",
                                    .mName = "Synthetic Fund",
                                    .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
                                },
                                FailedInvestment{
                                    .mIsin = "XX0000000001",
                                    .mName = "Synthetic Share",
                                    .mReasonCodes =
                                        {ProcessingDiagnosticCode::LossEligibilityUnresolved,
                                         ProcessingDiagnosticCode::IncompleteHistory,
                                         ProcessingDiagnosticCode::IncompleteHistory},
                                },
                            },
                    },
            },
        .mProcessingDiagnostics = {0, 1},
    }};

    const auto result = ApplicationResultBuilder::build(std::move(input));

    EXPECT_EQ(result.mRunRevision, 7U);
    ASSERT_EQ(result.mRequests.size(), 1U);

    const auto& request = result.mRequests[0];

    EXPECT_EQ(request.mRunRevision, 7U);
    ASSERT_EQ(request.mInvestments.size(), 2U);

    // The diagnostics sort by ISIN too: diagnostic-0001 is XX0000000001.
    EXPECT_EQ(request.mInvestments[0].mIsin, "XX0000000001");
    EXPECT_EQ(request.mInvestments[0].mName, "Synthetic Share");
    EXPECT_EQ(request.mInvestments[0].mReasonCodes,
              (std::vector<ApplicationDiagnosticCode>{
                  ProcessingDiagnosticCode::IncompleteHistory,
                  ProcessingDiagnosticCode::LossEligibilityUnresolved}));
    EXPECT_EQ(request.mInvestments[0].mDiagnosticIds,
              (std::vector<std::string>{"diagnostic-0001"}));

    EXPECT_EQ(request.mInvestments[1].mIsin, "XX0000000002");
    EXPECT_EQ(request.mInvestments[1].mName, "Synthetic Fund");
    EXPECT_EQ(request.mInvestments[1].mDiagnosticIds,
              (std::vector<std::string>{"diagnostic-0002"}));
}

TEST(ApplicationResultRequestTest, QuestionDataMustFitTheRequestType) {
    // A failure choice without its investment list.
    ApplicationResultInput withoutList{.mReportingYear = 2024};
    withoutList.mRequests = {
        RequestInput{.mQuestion = {
                         .mType = RequestType::CapitalGainsFailureResolutionRequired,
                         .mAffectedReports = {kCapital},
                     }}};

    EXPECT_THROW((void)ApplicationResultBuilder::build(std::move(withoutList)),
                 std::invalid_argument);

    // An investment list on a type that has no question data yet.
    ApplicationResultInput withList{.mReportingYear = 2024};
    withList.mRequests = {
        RequestInput{.mQuestion = {
                         .mType = RequestType::TaxClassificationRequired,
                         .mAffectedReports = {kCapital},
                         .mData =
                             FailureResolutionQuestion{
                                 .mInvestments = {FailedInvestment{
                                     .mIsin = "XX0000000001",
                                     .mName = "Synthetic Share",
                                     .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
                                 }}},
                     }}};

    EXPECT_THROW((void)ApplicationResultBuilder::build(std::move(withList)), std::invalid_argument);

    // A failure choice with an empty investment list: an answer would choose for nothing.
    ApplicationResultInput emptyList{.mReportingYear = 2024};
    emptyList.mRequests = {
        RequestInput{.mQuestion = {
                         .mType = RequestType::OtherInterestFailureResolutionRequired,
                         .mAffectedReports = {kOther},
                         .mData = FailureResolutionQuestion{.mInvestments = {}},
                     }}};

    EXPECT_THROW((void)ApplicationResultBuilder::build(std::move(emptyList)),
                 std::invalid_argument);
}

TEST(ApplicationResultRequestTest, ALossQuestionNeedsSalesWithOpenUnits) {
    // Loss question data on another request type.
    ApplicationResultInput otherType{.mReportingYear = 2024};
    otherType.mRequests = {
        RequestInput{.mQuestion = {
                         .mType = RequestType::CapitalGainsFailureResolutionRequired,
                         .mAffectedReports = {kCapital},
                         .mData =
                             LossEligibilityQuestion{
                                 .mIsin = "XX0000000001",
                                 .mName = "Synthetic Share",
                                 .mSales = {LossSaleFacts{.mSaleDate = day(2024, 3, 10),
                                                          .mSaleUnits = 100'000'000, // 1 unit
                                                          .mLossUnits = 100'000'000,
                                                          .mWindowStart = day(2024, 2, 9),
                                                          .mWindowEnd = day(2024, 4, 9),
                                                          .mOpenUnits = 100'000'000}},
                             },
                     }}};

    EXPECT_THROW((void)ApplicationResultBuilder::build(std::move(otherType)),
                 std::invalid_argument);

    // A loss question without question data.
    ApplicationResultInput withoutData{.mReportingYear = 2024};
    withoutData.mRequests = {
        RequestInput{.mQuestion = {
                         .mType = RequestType::LossEligibilityConfirmationRequired,
                         .mAffectedReports = {kCapital},
                     }}};

    EXPECT_THROW((void)ApplicationResultBuilder::build(std::move(withoutData)),
                 std::invalid_argument);

    // A loss question without a sale.
    ApplicationResultInput withoutSale{.mReportingYear = 2024};
    withoutSale.mRequests = {RequestInput{
        .mQuestion = {
            .mType = RequestType::LossEligibilityConfirmationRequired,
            .mAffectedReports = {kCapital},
            .mData = LossEligibilityQuestion{.mIsin = "XX0000000001", .mName = "Synthetic Share"},
        }}};

    EXPECT_THROW((void)ApplicationResultBuilder::build(std::move(withoutSale)),
                 std::invalid_argument);

    // A sale without open units: nothing to answer.
    ApplicationResultInput noOpenUnits{.mReportingYear = 2024};
    noOpenUnits.mRequests = {
        RequestInput{.mQuestion = {
                         .mType = RequestType::LossEligibilityConfirmationRequired,
                         .mAffectedReports = {kCapital},
                         .mData =
                             LossEligibilityQuestion{
                                 .mIsin = "XX0000000001",
                                 .mName = "Synthetic Share",
                                 .mSales = {LossSaleFacts{.mSaleDate = day(2024, 3, 10),
                                                          .mSaleUnits = 100'000'000, // 1 unit
                                                          .mLossUnits = 100'000'000,
                                                          .mWindowStart = day(2024, 2, 9),
                                                          .mWindowEnd = day(2024, 4, 9),
                                                          .mKnownIneligibleUnits = 100'000'000,
                                                          .mOpenUnits = 0}},
                             },
                     }}};

    EXPECT_THROW((void)ApplicationResultBuilder::build(std::move(noOpenUnits)),
                 std::invalid_argument);

    // More open units than loss units.
    ApplicationResultInput tooManyOpenUnits{.mReportingYear = 2024};
    tooManyOpenUnits.mRequests = {
        RequestInput{.mQuestion = {
                         .mType = RequestType::LossEligibilityConfirmationRequired,
                         .mAffectedReports = {kCapital},
                         .mData =
                             LossEligibilityQuestion{
                                 .mIsin = "XX0000000001",
                                 .mName = "Synthetic Share",
                                 .mSales = {LossSaleFacts{.mSaleDate = day(2024, 3, 10),
                                                          .mSaleUnits = 100'000'000, // 1 unit
                                                          .mLossUnits = 100'000'000,
                                                          .mWindowStart = day(2024, 2, 9),
                                                          .mWindowEnd = day(2024, 4, 9),
                                                          .mOpenUnits = 200'000'000}},
                             },
                     }}};

    EXPECT_THROW((void)ApplicationResultBuilder::build(std::move(tooManyOpenUnits)),
                 std::invalid_argument);
}

TEST(ApplicationResultRequestTest, ARatioQuestionNeedsASourceAndOpenUnits) {
    // Ratio question data on another request type.
    ApplicationResultInput otherType{.mReportingYear = 2024};
    otherType.mRequests = {
        RequestInput{.mQuestion = {
                         .mType = RequestType::CorporateActionIdentityRequired,
                         .mAffectedReports = {kCapital},
                         .mData =
                             CorporateActionRatioQuestion{
                                 .mIsin = "XX0000000001",
                                 .mName = "Synthetic Share",
                                 .mEffectiveDate = day(2024, 6, 1),
                                 .mSources = {{.mBroker = Broker::TradeRepublic,
                                               .mTransactionId = "synthetic-split",
                                               .mSourceValue = "3"}},
                                 .mPositionBefore = 300'000'000, // 3 units
                                 .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                                                  .mLatestDate = day(2024, 9, 1),
                                                  .mPositionBefore = 300'000'000}},
                             },
                     }}};

    EXPECT_THROW((void)ApplicationResultBuilder::build(std::move(otherType)),
                 std::invalid_argument);

    // A ratio question without question data.
    ApplicationResultInput withoutData{.mReportingYear = 2024};
    withoutData.mRequests = {RequestInput{.mQuestion = {
                                              .mType = RequestType::CorporateActionRatioRequired,
                                              .mAffectedReports = {kCapital},
                                          }}};

    EXPECT_THROW((void)ApplicationResultBuilder::build(std::move(withoutData)),
                 std::invalid_argument);

    // A ratio question without a source record.
    ApplicationResultInput withoutSource{.mReportingYear = 2024};
    withoutSource.mRequests = {
        RequestInput{.mQuestion = {
                         .mType = RequestType::CorporateActionRatioRequired,
                         .mAffectedReports = {kCapital},
                         .mData =
                             CorporateActionRatioQuestion{
                                 .mIsin = "XX0000000001",
                                 .mName = "Synthetic Share",
                                 .mEffectiveDate = day(2024, 6, 1),
                                 .mPositionBefore = 300'000'000, // 3 units
                                 .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                                                  .mLatestDate = day(2024, 9, 1),
                                                  .mPositionBefore = 300'000'000}},
                             },
                     }}};

    EXPECT_THROW((void)ApplicationResultBuilder::build(std::move(withoutSource)),
                 std::invalid_argument);

    // No units before the action: nothing to preview.
    ApplicationResultInput noPosition{.mReportingYear = 2024};
    noPosition.mRequests = {
        RequestInput{.mQuestion = {
                         .mType = RequestType::CorporateActionRatioRequired,
                         .mAffectedReports = {kCapital},
                         .mData =
                             CorporateActionRatioQuestion{
                                 .mIsin = "XX0000000001",
                                 .mName = "Synthetic Share",
                                 .mEffectiveDate = day(2024, 6, 1),
                                 .mSources = {{.mBroker = Broker::TradeRepublic,
                                               .mTransactionId = "synthetic-split",
                                               .mSourceValue = "3"}},
                                 .mPositionBefore = 0,
                                 .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                                                  .mLatestDate = day(2024, 9, 1),
                                                  .mPositionBefore = 0}},
                             },
                     }}};

    EXPECT_THROW((void)ApplicationResultBuilder::build(std::move(noPosition)),
                 std::invalid_argument);
}

TEST(ApplicationResultRequestTest, ARatioQuestionNeedsADateRangeAroundItsDate) {
    // The range starts after the row date of 2024-06-01.
    ApplicationResultInput lateStart{.mReportingYear = 2024};
    lateStart.mRequests = {
        RequestInput{.mQuestion = {
                         .mType = RequestType::CorporateActionRatioRequired,
                         .mAffectedReports = {kCapital},
                         .mData =
                             CorporateActionRatioQuestion{
                                 .mIsin = "XX0000000001",
                                 .mName = "Synthetic Share",
                                 .mEffectiveDate = day(2024, 6, 1),
                                 .mSources = {{.mBroker = Broker::TradeRepublic,
                                               .mTransactionId = "synthetic-split",
                                               .mSourceValue = "3"}},
                                 .mPositionBefore = 300'000'000, // 3 units
                                 .mDateRanges = {{.mEarliestDate = day(2024, 6, 2),
                                                  .mLatestDate = day(2024, 9, 1),
                                                  .mPositionBefore = 300'000'000}},
                             },
                     }}};

    EXPECT_THROW((void)ApplicationResultBuilder::build(std::move(lateStart)),
                 std::invalid_argument);

    // The range ends before the row date.
    ApplicationResultInput earlyEnd{.mReportingYear = 2024};
    earlyEnd.mRequests = {
        RequestInput{.mQuestion = {
                         .mType = RequestType::CorporateActionRatioRequired,
                         .mAffectedReports = {kCapital},
                         .mData =
                             CorporateActionRatioQuestion{
                                 .mIsin = "XX0000000001",
                                 .mName = "Synthetic Share",
                                 .mEffectiveDate = day(2024, 6, 1),
                                 .mSources = {{.mBroker = Broker::TradeRepublic,
                                               .mTransactionId = "synthetic-split",
                                               .mSourceValue = "3"}},
                                 .mPositionBefore = 300'000'000, // 3 units
                                 .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                                                  .mLatestDate = day(2024, 5, 31),
                                                  .mPositionBefore = 300'000'000}},
                             },
                     }}};

    EXPECT_THROW((void)ApplicationResultBuilder::build(std::move(earlyEnd)), std::invalid_argument);

    // The range of the row date has 4 units, the question says 3 units just before the row date.
    ApplicationResultInput otherUnits{.mReportingYear = 2024};
    otherUnits.mRequests = {
        RequestInput{.mQuestion = {
                         .mType = RequestType::CorporateActionRatioRequired,
                         .mAffectedReports = {kCapital},
                         .mData =
                             CorporateActionRatioQuestion{
                                 .mIsin = "XX0000000001",
                                 .mName = "Synthetic Share",
                                 .mEffectiveDate = day(2024, 6, 1),
                                 .mSources = {{.mBroker = Broker::TradeRepublic,
                                               .mTransactionId = "synthetic-split",
                                               .mSourceValue = "3"}},
                                 .mPositionBefore = 300'000'000, // 3 units
                                 .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                                                  .mLatestDate = day(2024, 9, 1),
                                                  .mPositionBefore = 400'000'000}},
                             },
                     }}};

    EXPECT_THROW((void)ApplicationResultBuilder::build(std::move(otherUnits)),
                 std::invalid_argument);

    // A range of one day, the row date itself: trades on the day before and on that day.
    ApplicationResultInput oneDay{.mReportingYear = 2024};
    oneDay.mRequests = {RequestInput{.mQuestion = {
                                         .mType = RequestType::CorporateActionRatioRequired,
                                         .mAffectedReports = {kCapital},
                                         .mData =
                                             CorporateActionRatioQuestion{
                                                 .mIsin = "XX0000000001",
                                                 .mName = "Synthetic Share",
                                                 .mEffectiveDate = day(2024, 6, 1),
                                                 .mSources = {{.mBroker = Broker::TradeRepublic,
                                                               .mTransactionId = "synthetic-split",
                                                               .mSourceValue = "3"}},
                                                 .mPositionBefore = 300'000'000, // 3 units
                                                 .mDateRanges = {{.mEarliestDate = day(2024, 6, 1),
                                                                  .mLatestDate = day(2024, 6, 1),
                                                                  .mPositionBefore = 300'000'000}},
                                             },
                                     }}};

    const auto result = ApplicationResultBuilder::build(std::move(oneDay));

    EXPECT_EQ(result.mRequests.size(), 1U);
}

TEST(ApplicationResultRequestTest, ARatioQuestionBeforeASaleNeedsTheSaleDateInsteadOfTheUnits) {
    // The sale of 2024-06-01 needs the split before it; the row is dated 2024-06-03. Valid: no
    // units at the row date, the sale date, and a range up to the sale.
    ApplicationResultInput beforeTheSale{.mReportingYear = 2024};
    beforeTheSale.mRequests = {
        RequestInput{.mQuestion = {
                         .mType = RequestType::CorporateActionRatioRequired,
                         .mAffectedReports = {kCapital},
                         .mData =
                             CorporateActionRatioQuestion{
                                 .mIsin = "XX0000000001",
                                 .mName = "Synthetic Share",
                                 .mEffectiveDate = day(2024, 6, 3),
                                 .mSources = {{.mBroker = Broker::TradeRepublic,
                                               .mTransactionId = "synthetic-split",
                                               .mSourceValue = "10"}},
                                 .mPositionBefore = std::nullopt,
                                 .mEarlierSaleDate = day(2024, 6, 1),
                                 .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                                                  .mLatestDate = day(2024, 6, 1),
                                                  .mPositionBefore = 1'000'000'000}},
                             },
                     }}};

    EXPECT_EQ(ApplicationResultBuilder::build(std::move(beforeTheSale)).mRequests.size(), 1U);

    // Both the units at the row date and a sale date.
    ApplicationResultInput both{.mReportingYear = 2024};
    both.mRequests = {RequestInput{.mQuestion = {
                                       .mType = RequestType::CorporateActionRatioRequired,
                                       .mAffectedReports = {kCapital},
                                       .mData =
                                           CorporateActionRatioQuestion{
                                               .mIsin = "XX0000000001",
                                               .mName = "Synthetic Share",
                                               .mEffectiveDate = day(2024, 6, 1),
                                               .mSources = {{.mBroker = Broker::TradeRepublic,
                                                             .mTransactionId = "synthetic-split",
                                                             .mSourceValue = "10"}},
                                               .mPositionBefore = 1'000'000'000, // 10 units
                                               .mEarlierSaleDate = day(2024, 5, 30),
                                               .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                                                                .mLatestDate = day(2024, 6, 1),
                                                                .mPositionBefore = 1'000'000'000}},
                                           },
                                   }}};

    EXPECT_THROW((void)ApplicationResultBuilder::build(std::move(both)), std::invalid_argument);

    // Neither of them.
    ApplicationResultInput neither{.mReportingYear = 2024};
    neither.mRequests = {
        RequestInput{.mQuestion = {
                         .mType = RequestType::CorporateActionRatioRequired,
                         .mAffectedReports = {kCapital},
                         .mData =
                             CorporateActionRatioQuestion{
                                 .mIsin = "XX0000000001",
                                 .mName = "Synthetic Share",
                                 .mEffectiveDate = day(2024, 6, 3),
                                 .mSources = {{.mBroker = Broker::TradeRepublic,
                                               .mTransactionId = "synthetic-split",
                                               .mSourceValue = "10"}},
                                 .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                                                  .mLatestDate = day(2024, 6, 1),
                                                  .mPositionBefore = 1'000'000'000}},
                             },
                     }}};

    EXPECT_THROW((void)ApplicationResultBuilder::build(std::move(neither)), std::invalid_argument);

    // A range that goes past the sale.
    ApplicationResultInput pastTheSale{.mReportingYear = 2024};
    pastTheSale.mRequests = {
        RequestInput{.mQuestion = {
                         .mType = RequestType::CorporateActionRatioRequired,
                         .mAffectedReports = {kCapital},
                         .mData =
                             CorporateActionRatioQuestion{
                                 .mIsin = "XX0000000001",
                                 .mName = "Synthetic Share",
                                 .mEffectiveDate = day(2024, 6, 3),
                                 .mSources = {{.mBroker = Broker::TradeRepublic,
                                               .mTransactionId = "synthetic-split",
                                               .mSourceValue = "10"}},
                                 .mEarlierSaleDate = day(2024, 6, 1),
                                 .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                                                  .mLatestDate = day(2024, 6, 2),
                                                  .mPositionBefore = 1'000'000'000}},
                             },
                     }}};

    EXPECT_THROW((void)ApplicationResultBuilder::build(std::move(pastTheSale)),
                 std::invalid_argument);

    // No range at all.
    ApplicationResultInput noRange{.mReportingYear = 2024};
    noRange.mRequests = {RequestInput{.mQuestion = {
                                          .mType = RequestType::CorporateActionRatioRequired,
                                          .mAffectedReports = {kCapital},
                                          .mData =
                                              CorporateActionRatioQuestion{
                                                  .mIsin = "XX0000000001",
                                                  .mName = "Synthetic Share",
                                                  .mEffectiveDate = day(2024, 6, 3),
                                                  .mSources = {{.mBroker = Broker::TradeRepublic,
                                                                .mTransactionId = "synthetic-split",
                                                                .mSourceValue = "10"}},
                                                  .mEarlierSaleDate = day(2024, 6, 1),
                                              },
                                      }}};

    EXPECT_THROW((void)ApplicationResultBuilder::build(std::move(noRange)), std::invalid_argument);
}

TEST(ApplicationResultRequestTest, OnlyARatioQuestionWithRowsOnTwoDatesAsksForOneEvent) {
    // Rows on 2024-06-01 and 2024-06-05 without the question whether they are one event.
    ApplicationResultInput twoDates{.mReportingYear = 2024};
    twoDates.mRequests = {
        RequestInput{
            .mQuestion = {
                .mType = RequestType::CorporateActionRatioRequired,
                .mAffectedReports = {kCapital},
                .mData =
                    CorporateActionRatioQuestion{
                        .mIsin = "XX0000000001",
                        .mName = "Synthetic Share",
                        .mEffectiveDate = day(2024, 6, 1),
                        .mSources = {{.mBroker = Broker::TradeRepublic, .mDate = day(2024, 6, 1)},
                                     {.mBroker = Broker::InteractiveBrokers,
                                      .mDate = day(2024, 6, 5)}},
                        .mPositionBefore = 300'000'000, // 3 units
                        .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                                         .mLatestDate = day(2024, 12, 31),
                                         .mPositionBefore = 300'000'000}},
                        .mAskSameEvent = false,
                    },
            }}};

    EXPECT_THROW((void)ApplicationResultBuilder::build(std::move(twoDates)), std::invalid_argument);

    // One row with the question: there is nothing to tell apart.
    ApplicationResultInput oneDate{.mReportingYear = 2024};
    oneDate.mRequests = {RequestInput{
        .mQuestion = {
            .mType = RequestType::CorporateActionRatioRequired,
            .mAffectedReports = {kCapital},
            .mData =
                CorporateActionRatioQuestion{
                    .mIsin = "XX0000000001",
                    .mName = "Synthetic Share",
                    .mEffectiveDate = day(2024, 6, 1),
                    .mSources = {{.mBroker = Broker::TradeRepublic, .mDate = day(2024, 6, 1)}},
                    .mPositionBefore = 300'000'000, // 3 units
                    .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                                     .mLatestDate = day(2024, 12, 31),
                                     .mPositionBefore = 300'000'000}},
                    .mAskSameEvent = true,
                },
        }}};

    EXPECT_THROW((void)ApplicationResultBuilder::build(std::move(oneDate)), std::invalid_argument);
}

TEST(ApplicationResultRequestTest, TheOrderOfSourcesDoesNotChangeTheRatioQuestion) {
    // The same action from Trade Republic and Interactive Brokers, listed in two orders.
    const QuestionData ibkrFirst = CorporateActionRatioQuestion{
        .mIsin = "XX0000000001",
        .mName = "Synthetic Share",
        .mEffectiveDate = day(2024, 6, 1),
        .mSources = {{.mBroker = Broker::InteractiveBrokers, .mTransactionId = "synthetic-ibkr"},
                     {.mBroker = Broker::TradeRepublic,
                      .mTransactionId = "synthetic-tr",
                      .mSourceValue = "3"}},
        .mPositionBefore = 300'000'000, // 3 units
        .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                         .mLatestDate = day(2024, 9, 1),
                         .mPositionBefore = 300'000'000}},
    };
    const QuestionData trFirst = CorporateActionRatioQuestion{
        .mIsin = "XX0000000001",
        .mName = "Synthetic Share",
        .mEffectiveDate = day(2024, 6, 1),
        .mSources = {{.mBroker = Broker::TradeRepublic,
                      .mTransactionId = "synthetic-tr",
                      .mSourceValue = "3"},
                     {.mBroker = Broker::InteractiveBrokers, .mTransactionId = "synthetic-ibkr"}},
        .mPositionBefore = 300'000'000, // 3 units
        .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                         .mLatestDate = day(2024, 9, 1),
                         .mPositionBefore = 300'000'000}},
    };

    EXPECT_NE(ibkrFirst, trFirst);
    EXPECT_EQ(normalizedData(ibkrFirst), normalizedData(trFirst));

    // Trade Republic comes first in the Broker enum.
    const auto normalized = normalizedData(ibkrFirst);
    const auto& sources = std::get<CorporateActionRatioQuestion>(normalized).mSources;

    ASSERT_EQ(sources.size(), 2U);
    EXPECT_EQ(sources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(sources[1].mBroker, Broker::InteractiveBrokers);
}

TEST(ApplicationResultRequestTest, AStageOutputKeepsItsLinksWhenAppended) {
    // The input already holds one diagnostic, so the stage's diagnostics 0 and 1 become 1 and 2.
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mProcessingDiagnostics = {ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::IncompleteHistory,
        .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kCapital}},
        .mMessage = "Synthetic first error.",
    }};
    std::vector<ProcessingDiagnostic> stageDiagnostics{
        ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::LossEligibilityUnresolved,
            .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kCapital}},
            .mMessage = "Synthetic second error.",
        },
        ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::LossEligibilityUnresolved,
            .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kCapital}},
            .mMessage = "Synthetic third error.",
        },
    };
    std::vector<RequestInput> stageRequests{RequestInput{
        .mQuestion = {.mType = RequestType::TaxClassificationRequired,
                      .mAffectedReports = {kCapital}},
        .mProcessingDiagnostics = {0, 1},
    }};

    appendStageOutput(input, std::move(stageDiagnostics), std::move(stageRequests));

    ASSERT_EQ(input.mProcessingDiagnostics.size(), 3U);
    EXPECT_EQ(input.mProcessingDiagnostics[1].mMessage, "Synthetic second error.");
    EXPECT_EQ(input.mProcessingDiagnostics[2].mMessage, "Synthetic third error.");
    ASSERT_EQ(input.mRequests.size(), 1U);
    EXPECT_EQ(input.mRequests[0].mProcessingDiagnostics, (std::vector<std::size_t>{1, 2}));
}

} // namespace
