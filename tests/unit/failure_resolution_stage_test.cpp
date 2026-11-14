#include "processors/failure_resolution_stage.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace taxbroker;

constexpr auto kCapital = ReportType::CapitalGains;
constexpr auto kDividends = ReportType::Dividends;
constexpr auto kOtherInterest = ReportType::OtherInterest;

const AnswerValue kExclude = FailureResolutionAnswer{FailureResolution::ExcludeFailedInstruments};
const AnswerValue kFailReport = FailureResolutionAnswer{FailureResolution::FailReport};
const AnswerValue kCancel = CancelAnswer{};

Date day(int aYear, unsigned aMonth, unsigned aDay) {
    return Date{std::chrono::sys_days{std::chrono::year{aYear} / aMonth / aDay}.time_since_epoch()};
}

/// A loss question of one sale with 1 open unit.
RequestInput
lossRequest(const Isin& aIsin, const std::string& aName, std::vector<std::size_t> aDiagnostics) {
    return RequestInput{
        .mQuestion =
            DecisionQuestion{
                .mType = RequestType::LossEligibilityConfirmationRequired,
                .mAffectedReports = {kCapital},
                .mIsin = aIsin,
                .mData =
                    LossEligibilityQuestion{
                        .mIsin = aIsin,
                        .mName = aName,
                        .mSales = {{.mSaleDate = day(2024, 3, 10),
                                    .mSaleUnits = 100'000'000, // 1 unit
                                    .mLossUnits = 100'000'000, // 1 unit
                                    .mWindowStart = day(2024, 2, 9),
                                    .mWindowEnd = day(2024, 4, 9),
                                    .mOpenUnits = 100'000'000}}, // 1 unit
                    },
            },
        .mProcessingDiagnostics = std::move(aDiagnostics),
    };
}

/// A parse error of a trade row, which affects capital gains but names no investment.
SourcedParseDiagnostic rejectedTradeRow() {
    return SourcedParseDiagnostic{
        .mSourceIndex = 0,
        .mBroker = Broker::TradeRepublic,
        .mDiagnostic = {.mSeverity = DiagnosticSeverity::Error,
                        .mCode = DiagnosticCode::InvalidValue,
                        .mSourceFile = "history.csv",
                        .mRowIndex = 3,
                        .mMessage = "Synthetic parse error.",
                        .mRowKind = ParsedRowKind::Trade},
    };
}

using InputFactory = std::function<ApplicationResultInput()>;

/// Starts a session whose calculation runs the stage on a new input. aExcluded gets what the stage
/// returned in the latest calculation.
void start(DecisionSession& aSession,
           InputFactory aInput,
           std::vector<Isin>& aExcluded,
           FailureForm aForm = kCapitalFailure) {
    // The session keeps the calculation, so the lambda owns the factory.
    aSession.start([factory = std::move(aInput), &aExcluded, aForm](RunDecisions& aDecisions) {
        auto input = factory();

        aExcluded = FailureResolutionStage::resolve(input, aDecisions, aForm);

        return input;
    });
}

TEST(FailureResolutionStageTest, TwoFailedInvestmentsAreOneQuestionSortedByIsin) {
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2024};

        // XX0000000002 has incomplete history; XX0000000005 has a missing exchange rate and an
        // unsupported asset class.
        input.mProcessingDiagnostics = {
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::IncompleteHistory,
                .mContext = {.mStage = PipelineStage::HistoricalValidation,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Other",
                             .mIsin = "XX0000000002"},
                .mMessage = "Synthetic capital error.",
            },
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::MissingExchangeRate,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Coin",
                             .mIsin = "XX0000000005"},
                .mMessage = "Synthetic capital error.",
            },
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::UnsupportedTaxTreatment,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Coin",
                             .mIsin = "XX0000000005"},
                .mMessage = "Synthetic capital error.",
            },
        };
        input.mReports[0].mXml = "<capital/>";

        return input;
    };
    DecisionSession session;
    std::vector<Isin> excluded;

    start(session, input, excluded);
    const auto& result = session.result();

    EXPECT_TRUE(excluded.empty());
    ASSERT_EQ(result.mRequests.size(), 1U);

    const auto& request = result.mRequests.at(0);

    EXPECT_EQ(request.mType, RequestType::CapitalGainsFailureResolutionRequired);
    EXPECT_EQ(request.mAffectedReports, std::vector<ReportType>{kCapital});
    EXPECT_FALSE(request.mIsin.has_value());

    // Diagnostic order: XX0000000002 first; for XX0000000005 the unsupported asset class (code
    // 7) comes before the missing rate (code 10).
    ASSERT_EQ(request.mInvestments.size(), 2U);
    EXPECT_EQ(request.mInvestments.at(0).mIsin, "XX0000000002");
    EXPECT_EQ(request.mInvestments.at(0).mName, "Synthetic Other");
    EXPECT_EQ(
        request.mInvestments.at(0).mReasonCodes,
        (std::vector<ApplicationDiagnosticCode>{ProcessingDiagnosticCode::IncompleteHistory}));
    EXPECT_EQ(request.mInvestments.at(0).mDiagnosticIds,
              std::vector<std::string>{"diagnostic-0001"});
    EXPECT_EQ(request.mInvestments.at(1).mIsin, "XX0000000005");
    EXPECT_EQ(request.mInvestments.at(1).mName, "Synthetic Coin");
    EXPECT_EQ(
        request.mInvestments.at(1).mReasonCodes,
        (std::vector<ApplicationDiagnosticCode>{ProcessingDiagnosticCode::UnsupportedTaxTreatment,
                                                ProcessingDiagnosticCode::MissingExchangeRate}));
    EXPECT_EQ(request.mInvestments.at(1).mDiagnosticIds,
              (std::vector<std::string>{"diagnostic-0002", "diagnostic-0003"}));

    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::NeedsInput);
    EXPECT_FALSE(result.report(kCapital).mFailureResolution.has_value());
    EXPECT_TRUE(result.report(kCapital).mExcludedInvestments.empty());
    EXPECT_FALSE(result.report(kCapital).mXml.has_value());
}

TEST(FailureResolutionStageTest, AnInvestmentNameMissingFromAnErrorIsTakenFromAnother) {
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2024};

        input.mProcessingDiagnostics = {
            // No instrument name.
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::IncompleteHistory,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital},
                             .mIsin = "XX0000000002"},
                .mMessage = "Synthetic capital error.",
            },
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::MissingExchangeRate,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Other",
                             .mIsin = "XX0000000002"},
                .mMessage = "Synthetic capital error.",
            },
        };

        return input;
    };
    DecisionSession session;
    std::vector<Isin> excluded;

    start(session, input, excluded);
    const auto& result = session.result();

    ASSERT_EQ(result.mRequests.size(), 1U);
    ASSERT_EQ(result.mRequests.at(0).mInvestments.size(), 1U);
    EXPECT_EQ(result.mRequests.at(0).mInvestments.at(0).mName, "Synthetic Other");
}

// Errors that no investment owns.

TEST(FailureResolutionStageTest, ARejectedTradeRowSuppressesTheQuestion) {
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2024};

        // XX0000000002 has incomplete history; XX0000000005 has a missing exchange rate and an
        // unsupported asset class.
        input.mProcessingDiagnostics = {
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::IncompleteHistory,
                .mContext = {.mStage = PipelineStage::HistoricalValidation,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Other",
                             .mIsin = "XX0000000002"},
                .mMessage = "Synthetic capital error.",
            },
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::MissingExchangeRate,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Coin",
                             .mIsin = "XX0000000005"},
                .mMessage = "Synthetic capital error.",
            },
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::UnsupportedTaxTreatment,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Coin",
                             .mIsin = "XX0000000005"},
                .mMessage = "Synthetic capital error.",
            },
        };
        input.mReports[0].mXml = "<capital/>";

        input.mMergeDiagnostics = {rejectedTradeRow()};

        return input;
    };
    DecisionSession session;
    std::vector<Isin> excluded;

    start(session, input, excluded);
    const auto& result = session.result();

    EXPECT_TRUE(excluded.empty());
    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Failed);
}

TEST(FailureResolutionStageTest, AnErrorWithoutAnIsinSuppressesTheQuestion) {
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2024};

        // XX0000000002 has incomplete history; XX0000000005 has a missing exchange rate and an
        // unsupported asset class.
        input.mProcessingDiagnostics = {
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::IncompleteHistory,
                .mContext = {.mStage = PipelineStage::HistoricalValidation,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Other",
                             .mIsin = "XX0000000002"},
                .mMessage = "Synthetic capital error.",
            },
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::MissingExchangeRate,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Coin",
                             .mIsin = "XX0000000005"},
                .mMessage = "Synthetic capital error.",
            },
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::UnsupportedTaxTreatment,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Coin",
                             .mIsin = "XX0000000005"},
                .mMessage = "Synthetic capital error.",
            },
        };
        input.mReports[0].mXml = "<capital/>";

        // No ISIN.
        input.mProcessingDiagnostics.push_back(ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
            .mContext = {.mStage = PipelineStage::FifoTax,
                         .mAffectedReports = {kCapital},
                         .mInstrumentName = "Synthetic Other"},
            .mMessage = "Synthetic capital error.",
        });

        return input;
    };
    DecisionSession session;
    std::vector<Isin> excluded;

    start(session, input, excluded);
    const auto& result = session.result();

    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Failed);
}

TEST(FailureResolutionStageTest, AnXmlErrorSuppressesTheQuestion) {
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2024};

        // XX0000000002 has incomplete history; XX0000000005 has a missing exchange rate and an
        // unsupported asset class.
        input.mProcessingDiagnostics = {
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::IncompleteHistory,
                .mContext = {.mStage = PipelineStage::HistoricalValidation,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Other",
                             .mIsin = "XX0000000002"},
                .mMessage = "Synthetic capital error.",
            },
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::MissingExchangeRate,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Coin",
                             .mIsin = "XX0000000005"},
                .mMessage = "Synthetic capital error.",
            },
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::UnsupportedTaxTreatment,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Coin",
                             .mIsin = "XX0000000005"},
                .mMessage = "Synthetic capital error.",
            },
        };
        input.mReports[0].mXml = "<capital/>";

        input.mProcessingDiagnostics.push_back(ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::XmlValueOutOfRange,
            .mContext = {.mStage = PipelineStage::XmlGeneration,
                         .mAffectedReports = {kCapital},
                         .mInstrumentName = "Synthetic Share",
                         .mIsin = "XX0000000001"},
            .mMessage = "Synthetic capital error.",
        });

        return input;
    };
    DecisionSession session;
    std::vector<Isin> excluded;

    start(session, input, excluded);
    const auto& result = session.result();

    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Failed);
}

TEST(FailureResolutionStageTest, AnErrorOfTheYearSuppressesTheQuestion) {
    // An invalid reporting year affects every form and names no investment.
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2024};

        // XX0000000002 has incomplete history; XX0000000005 has a missing exchange rate and an
        // unsupported asset class.
        input.mProcessingDiagnostics = {
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::IncompleteHistory,
                .mContext = {.mStage = PipelineStage::HistoricalValidation,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Other",
                             .mIsin = "XX0000000002"},
                .mMessage = "Synthetic capital error.",
            },
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::MissingExchangeRate,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Coin",
                             .mIsin = "XX0000000005"},
                .mMessage = "Synthetic capital error.",
            },
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::UnsupportedTaxTreatment,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Coin",
                             .mIsin = "XX0000000005"},
                .mMessage = "Synthetic capital error.",
            },
        };
        input.mReports[0].mXml = "<capital/>";

        input.mProcessingDiagnostics.push_back(ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::InvalidReportingYear,
            .mContext = {.mStage = PipelineStage::HistoricalValidation,
                         .mAffectedReports = {kCapital,
                                              kDividends,
                                              ReportType::BankDepositInterest,
                                              ReportType::OtherInterest},
                         .mField = "reporting_year"},
            .mMessage = "Synthetic year error.",
        });

        return input;
    };
    DecisionSession session;
    std::vector<Isin> excluded;

    start(session, input, excluded);
    const auto& result = session.result();

    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Failed);
}

TEST(FailureResolutionStageTest, AnErrorThatAlsoAffectsAnotherFormSuppressesTheQuestion) {
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2024};

        // XX0000000002 has incomplete history; XX0000000005 has a missing exchange rate and an
        // unsupported asset class.
        input.mProcessingDiagnostics = {
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::IncompleteHistory,
                .mContext = {.mStage = PipelineStage::HistoricalValidation,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Other",
                             .mIsin = "XX0000000002"},
                .mMessage = "Synthetic capital error.",
            },
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::MissingExchangeRate,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Coin",
                             .mIsin = "XX0000000005"},
                .mMessage = "Synthetic capital error.",
            },
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::UnsupportedTaxTreatment,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Coin",
                             .mIsin = "XX0000000005"},
                .mMessage = "Synthetic capital error.",
            },
        };
        input.mReports[0].mXml = "<capital/>";

        // Affects capital gains and dividends.
        input.mProcessingDiagnostics.push_back(ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::IncompleteHistory,
            .mContext = {.mStage = PipelineStage::FifoTax,
                         .mAffectedReports = {kCapital, kDividends},
                         .mInstrumentName = "Synthetic Share",
                         .mIsin = "XX0000000001"},
            .mMessage = "Synthetic shared error.",
        });

        return input;
    };
    DecisionSession session;
    std::vector<Isin> excluded;

    start(session, input, excluded);
    const auto& result = session.result();

    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Failed);
}

TEST(FailureResolutionStageTest, AnErrorOfAnotherFormIsNotListed) {
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2024};

        // Affects only the dividend form.
        input.mProcessingDiagnostics = {ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::IncompleteHistory,
            .mContext = {.mStage = PipelineStage::FifoTax,
                         .mAffectedReports = {kDividends},
                         .mInstrumentName = "Synthetic Share",
                         .mIsin = "XX0000000001"},
            .mMessage = "Synthetic dividend error.",
        }};

        return input;
    };
    DecisionSession session;
    std::vector<Isin> excluded;

    start(session, input, excluded);
    const auto& result = session.result();

    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.report(ReportType::Dividends).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::NoData);
}

TEST(FailureResolutionStageTest, AWarningIsNotListed) {
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2024};

        input.mProcessingDiagnostics = {ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Warning,
            .mCode = ProcessingDiagnosticCode::CorporateActionApplied,
            .mContext = {.mStage = PipelineStage::CorporateAction,
                         .mAffectedReports = {kCapital},
                         .mInstrumentName = "Synthetic Share",
                         .mIsin = "XX0000000001"},
            .mMessage = "Synthetic capital warning.",
        }};

        return input;
    };
    DecisionSession session;
    std::vector<Isin> excluded;

    start(session, input, excluded);
    const auto& result = session.result();

    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::NoData);
}

TEST(FailureResolutionStageTest, AnErrorOfAnOpenQuestionIsNotListed) {
    // XX0000000001 waits for its loss answer (diagnostic 0 is linked to it), so only that
    // question is open.
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2024};

        input.mProcessingDiagnostics = {ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::LossEligibilityUnresolved,
            .mContext = {.mStage = PipelineStage::FifoTax,
                         .mAffectedReports = {kCapital},
                         .mInstrumentName = "Synthetic Share",
                         .mIsin = "XX0000000001"},
            .mMessage = "Synthetic capital error.",
        }};
        input.mRequests = {lossRequest("XX0000000001", "Synthetic Share", {0})};

        return input;
    };
    DecisionSession session;
    std::vector<Isin> excluded;

    start(session, input, excluded);
    const auto& result = session.result();

    ASSERT_EQ(result.mRequests.size(), 1U);
    EXPECT_EQ(result.mRequests.at(0).mType, RequestType::LossEligibilityConfirmationRequired);
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::NeedsInput);
}

/// A capital error `incomplete_history` of one investment.
ProcessingDiagnostic missingHistoryError(const Isin& aIsin, const std::string& aName) {
    return ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::IncompleteHistory,
        .mContext = {.mStage = PipelineStage::HistoricalValidation,
                     .mAffectedReports = {kCapital},
                     .mInstrumentName = aName,
                     .mIsin = aIsin},
        .mMessage = "Synthetic capital error.",
    };
}

/// Starts a session whose calculation runs the capital stage with a developer offer. aOutcome
/// gets what the stage returned in the latest calculation.
void startWithDeveloper(DecisionSession& aSession,
                        InputFactory aInput,
                        DeveloperOffer aOffer,
                        ExcludedUnits& aOutcome) {
    aSession.start([factory = std::move(aInput), offer = std::move(aOffer), &aOutcome](
                       RunDecisions& aDecisions) {
        auto input = factory();

        aOutcome = FailureResolutionStage::resolveUnits(input, aDecisions, kCapitalFailure, offer);

        return input;
    });
}

bool isOffered(const DecisionSession& aSession) {
    const auto& request = aSession.result().mRequests.at(0);

    return std::get<FailureResolutionQuestion>(request.mData).mDeveloperExportOffered;
}

TEST(FailureResolutionStageTest, MissingHistoryOfEligibleIsinsOffersTheDeveloperExport) {
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2024};

        input.mProcessingDiagnostics = {missingHistoryError("XX0000000001", "Synthetic Share"),
                                        missingHistoryError("XX0000000002", "Synthetic Other")};

        return input;
    };
    DecisionSession session;
    ExcludedUnits outcome;

    startWithDeveloper(session,
                       input,
                       {.mEnabled = true, .mEligibleIsins = {"XX0000000001", "XX0000000002"}},
                       outcome);

    ASSERT_EQ(session.result().mRequests.size(), 1U);
    EXPECT_TRUE(isOffered(session));
}

TEST(FailureResolutionStageTest, WithoutDeveloperModeTheDeveloperExportIsNotOffered) {
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2024};

        input.mProcessingDiagnostics = {missingHistoryError("XX0000000001", "Synthetic Share")};

        return input;
    };
    DecisionSession session;
    ExcludedUnits outcome;

    startWithDeveloper(session,
                       input,
                       {.mEnabled = false, .mEligibleIsins = {"XX0000000001"}},
                       outcome);

    ASSERT_EQ(session.result().mRequests.size(), 1U);
    EXPECT_FALSE(isOffered(session));
}

TEST(FailureResolutionStageTest, AnIsinOfAnotherErrorKindTakesTheDeveloperExportAwayFromTheList) {
    // XX0000000002 has a missing exchange rate besides: one listed error is not missing history.
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2024};

        input.mProcessingDiagnostics = {missingHistoryError("XX0000000001", "Synthetic Share"),
                                        ProcessingDiagnostic{
                                            .mSeverity = DiagnosticSeverity::Error,
                                            .mCode = ProcessingDiagnosticCode::MissingExchangeRate,
                                            .mContext = {.mStage = PipelineStage::FifoTax,
                                                         .mAffectedReports = {kCapital},
                                                         .mInstrumentName = "Synthetic Other",
                                                         .mIsin = "XX0000000002"},
                                            .mMessage = "Synthetic capital error.",
                                        }};

        return input;
    };
    DecisionSession session;
    ExcludedUnits outcome;

    startWithDeveloper(session,
                       input,
                       {.mEnabled = true, .mEligibleIsins = {"XX0000000001", "XX0000000002"}},
                       outcome);

    ASSERT_EQ(session.result().mRequests.size(), 1U);
    EXPECT_FALSE(isOffered(session));
}

TEST(FailureResolutionStageTest, AnIsinWhoseFifoPoolStoppedForAnotherReasonIsNotEligible) {
    // An `incomplete_history` error of an ISIN that is not in the eligible list: its pool stopped
    // at a corporate action, so the developer export cannot pass it.
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2024};

        input.mProcessingDiagnostics = {missingHistoryError("XX0000000001", "Synthetic Share"),
                                        missingHistoryError("XX0000000002", "Synthetic Other")};

        return input;
    };
    DecisionSession session;
    ExcludedUnits outcome;

    startWithDeveloper(session,
                       input,
                       {.mEnabled = true, .mEligibleIsins = {"XX0000000001"}},
                       outcome);

    ASSERT_EQ(session.result().mRequests.size(), 1U);
    EXPECT_FALSE(isOffered(session));
}

TEST(FailureResolutionStageTest, AnOpenQuestionOfTheIsinTakesTheDeveloperExportAway) {
    // XX0000000001 also waits for a loss answer (diagnostic 1 is linked to it).
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2024};

        input.mProcessingDiagnostics = {
            missingHistoryError("XX0000000001", "Synthetic Share"),
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::LossEligibilityUnresolved,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Share",
                             .mIsin = "XX0000000001"},
                .mMessage = "Synthetic capital error.",
            }};
        input.mRequests = {lossRequest("XX0000000001", "Synthetic Share", {1})};

        return input;
    };
    DecisionSession session;
    ExcludedUnits outcome;

    startWithDeveloper(session,
                       input,
                       {.mEnabled = true, .mEligibleIsins = {"XX0000000001"}},
                       outcome);

    // The failure question is the second request, after the loss question of the same ISIN.
    ASSERT_EQ(session.result().mRequests.size(), 2U);

    const auto failure = std::find_if(session.result().mRequests.begin(),
                                      session.result().mRequests.end(),
                                      [](const DecisionRequest& aRequest) {
                                          return aRequest.mType ==
                                                 RequestType::CapitalGainsFailureResolutionRequired;
                                      });

    ASSERT_NE(failure, session.result().mRequests.end());
    EXPECT_FALSE(std::get<FailureResolutionQuestion>(failure->mData).mDeveloperExportOffered);
}

TEST(FailureResolutionStageTest, TheDeveloperExportIsNeverOfferedForAnotherForm) {
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2024};

        input.mProcessingDiagnostics = {ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::IncompleteHistory,
            .mContext = {.mStage = PipelineStage::FifoTax,
                         .mAffectedReports = {kDividends},
                         .mInstrumentName = "Synthetic Share",
                         .mIsin = "XX0000000001"},
            .mMessage = "Synthetic dividend error.",
        }};

        return input;
    };
    DecisionSession session;
    ExcludedUnits outcome;

    session.start([&input, &outcome](RunDecisions& aDecisions) {
        auto result = input();

        outcome = FailureResolutionStage::resolveUnits(
            result,
            aDecisions,
            kDividendsFailure,
            {.mEnabled = true, .mEligibleIsins = {"XX0000000001"}});

        return result;
    });

    ASSERT_EQ(session.result().mRequests.size(), 1U);
    EXPECT_FALSE(isOffered(session));
}

TEST(FailureResolutionStageTest, TheDeveloperChoiceWaitsForTheThreeConfirmations) {
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2024};

        input.mProcessingDiagnostics = {missingHistoryError("XX0000000001", "Synthetic Share")};

        return input;
    };
    DecisionSession session;
    ExcludedUnits outcome;
    const AnswerValue developerExport = FailureResolutionAnswer{FailureResolution::DeveloperExport};
    const AnswerValue confirm = DeveloperExportConfirmAnswer{};

    startWithDeveloper(session,
                       input,
                       {.mEnabled = true, .mEligibleIsins = {"XX0000000001"}},
                       outcome);

    ASSERT_TRUE(session.answer({{"request-0001", 1, developerExport}}).empty());

    // Step 1: nothing is overridden yet and the form needs the confirmation.
    EXPECT_TRUE(outcome.mDeveloperIsins.empty());
    EXPECT_EQ(session.result().report(kCapital).mStatus, ReportStatus::NeedsInput);
    EXPECT_EQ(session.result().mRequests.at(0).mType,
              RequestType::DeveloperExportConfirmationRequired);

    ASSERT_TRUE(session.answer({{"request-0001", 2, confirm}}).empty());
    EXPECT_TRUE(outcome.mDeveloperIsins.empty());

    ASSERT_TRUE(session.answer({{"request-0001", 3, confirm}}).empty());
    EXPECT_TRUE(outcome.mDeveloperIsins.empty());
    EXPECT_EQ(std::get<DeveloperExportQuestion>(session.result().mRequests.at(0).mData).mStep, 3);

    // Step 3 confirmed: the ISIN is overridden, not excluded, and its error no longer blocks.
    ASSERT_TRUE(session.answer({{"request-0001", 4, confirm}}).empty());
    EXPECT_EQ(outcome.mDeveloperIsins, std::vector<Isin>{"XX0000000001"});
    EXPECT_TRUE(outcome.mIsins.empty());
    EXPECT_TRUE(session.result().mRequests.empty());
    EXPECT_EQ(session.result().report(kCapital).mFailureResolution,
              FailureResolution::DeveloperExport);
    ASSERT_EQ(session.result().report(kCapital).mUnsafeInvestments.size(), 1U);
    EXPECT_FALSE(session.result().mDiagnostics.at(0).mBlocking);
}

TEST(FailureResolutionStageTest, AChangedListAsksTheFailureQuestionAgainBeforeAnyStep) {
    // Only XX0000000001 has missing history at first; later XX0000000002 has it too.
    bool secondIsin = false;
    const auto input = [&secondIsin]() {
        ApplicationResultInput input{.mReportingYear = 2024};

        input.mProcessingDiagnostics = {missingHistoryError("XX0000000001", "Synthetic Share")};

        if (secondIsin)
        {
            input.mProcessingDiagnostics.push_back(
                missingHistoryError("XX0000000002", "Synthetic Other"));
        }

        return input;
    };
    DecisionSession session;
    ExcludedUnits outcome;
    const AnswerValue developerExport = FailureResolutionAnswer{FailureResolution::DeveloperExport};
    const AnswerValue confirm = DeveloperExportConfirmAnswer{};

    startWithDeveloper(session,
                       input,
                       {.mEnabled = true, .mEligibleIsins = {"XX0000000001", "XX0000000002"}},
                       outcome);

    ASSERT_TRUE(session.answer({{"request-0001", 1, developerExport}}).empty());
    ASSERT_EQ(session.result().mRequests.at(0).mType,
              RequestType::DeveloperExportConfirmationRequired);

    // The list now has two investments: the choice and the step were given for one only.
    secondIsin = true;

    ASSERT_TRUE(session.answer({{"request-0001", 2, confirm}}).empty());

    ASSERT_EQ(session.result().mRequests.size(), 1U);
    EXPECT_EQ(session.result().mRequests.at(0).mType,
              RequestType::CapitalGainsFailureResolutionRequired);
    EXPECT_EQ(session.result().mRequests.at(0).mInvestments.size(), 2U);
    EXPECT_TRUE(outcome.mDeveloperIsins.empty());
    EXPECT_TRUE(outcome.mIsins.empty());
}

// The answers.

TEST(FailureResolutionStageTest, ExcludingLeavesOutEveryListedIsinAndResolvesItsErrors) {
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2024};

        // XX0000000002 has incomplete history; XX0000000005 has a missing exchange rate and an
        // unsupported asset class.
        input.mProcessingDiagnostics = {
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::IncompleteHistory,
                .mContext = {.mStage = PipelineStage::HistoricalValidation,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Other",
                             .mIsin = "XX0000000002"},
                .mMessage = "Synthetic capital error.",
            },
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::MissingExchangeRate,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Coin",
                             .mIsin = "XX0000000005"},
                .mMessage = "Synthetic capital error.",
            },
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::UnsupportedTaxTreatment,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Coin",
                             .mIsin = "XX0000000005"},
                .mMessage = "Synthetic capital error.",
            },
        };
        input.mReports[0].mXml = "<capital/>";

        return input;
    };
    DecisionSession session;
    std::vector<Isin> excluded;

    start(session, input, excluded);

    const auto request = session.result().mRequests.at(0).mId;
    const auto rejections =
        session.answer({{.mRequestId = request, .mRunRevision = 1, .mValue = kExclude}});
    // The latest calculation of the stage is the one of the answer.
    const auto& result = session.result();

    EXPECT_TRUE(rejections.empty());
    EXPECT_EQ(excluded, (std::vector<Isin>{"XX0000000002", "XX0000000005"}));
    EXPECT_TRUE(result.mRequests.empty());

    // The errors stay in the result and no longer block.
    ASSERT_EQ(result.mDiagnostics.size(), 3U);
    EXPECT_FALSE(result.mDiagnostics.at(0).mBlocking);
    EXPECT_FALSE(result.mDiagnostics.at(1).mBlocking);
    EXPECT_FALSE(result.mDiagnostics.at(2).mBlocking);

    // The test input has an XML, so the form is generated; the CLI leaves it out of the input
    // when the rows are not built.
    const auto& capital = result.report(kCapital);

    EXPECT_EQ(capital.mStatus, ReportStatus::Generated);
    EXPECT_EQ(capital.mFailureResolution, FailureResolution::ExcludeFailedInstruments);
    ASSERT_EQ(capital.mExcludedInvestments.size(), 2U);
    EXPECT_EQ(capital.mExcludedInvestments.at(0).mIsin, "XX0000000002");
    EXPECT_EQ(capital.mExcludedInvestments.at(0).mDiagnosticIds,
              std::vector<std::string>{"diagnostic-0001"});
    EXPECT_EQ(capital.mExcludedInvestments.at(1).mIsin, "XX0000000005");
    EXPECT_EQ(capital.mExcludedInvestments.at(1).mDiagnosticIds,
              (std::vector<std::string>{"diagnostic-0002", "diagnostic-0003"}));
    EXPECT_EQ(result.mStatus, OverallStatus::CompletedWithWarnings);
}

TEST(FailureResolutionStageTest, FailingTheReportKeepsTheErrorsBlocking) {
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2024};

        // XX0000000002 has incomplete history; XX0000000005 has a missing exchange rate and an
        // unsupported asset class.
        input.mProcessingDiagnostics = {
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::IncompleteHistory,
                .mContext = {.mStage = PipelineStage::HistoricalValidation,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Other",
                             .mIsin = "XX0000000002"},
                .mMessage = "Synthetic capital error.",
            },
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::MissingExchangeRate,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Coin",
                             .mIsin = "XX0000000005"},
                .mMessage = "Synthetic capital error.",
            },
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::UnsupportedTaxTreatment,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Coin",
                             .mIsin = "XX0000000005"},
                .mMessage = "Synthetic capital error.",
            },
        };
        input.mReports[0].mXml = "<capital/>";

        return input;
    };
    DecisionSession session;
    std::vector<Isin> excluded;

    start(session, input, excluded);

    const auto request = session.result().mRequests.at(0).mId;
    const auto rejections =
        session.answer({{.mRequestId = request, .mRunRevision = 1, .mValue = kFailReport}});
    const auto& result = session.result();

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(excluded.empty());
    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_TRUE(result.mDiagnostics.at(0).mBlocking);
    EXPECT_TRUE(result.mDiagnostics.at(1).mBlocking);
    EXPECT_TRUE(result.mDiagnostics.at(2).mBlocking);

    const auto& capital = result.report(kCapital);

    EXPECT_EQ(capital.mStatus, ReportStatus::Failed);
    EXPECT_FALSE(capital.mXml.has_value());
    EXPECT_EQ(capital.mFailureResolution, FailureResolution::FailReport);
    EXPECT_TRUE(capital.mExcludedInvestments.empty());
    // The other three forms have no data and no error.
    EXPECT_EQ(result.mStatus, OverallStatus::PartialSuccess);
}

TEST(FailureResolutionStageTest, CancellingClosesTheQuestionWithoutAChoice) {
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2024};

        // XX0000000002 has incomplete history; XX0000000005 has a missing exchange rate and an
        // unsupported asset class.
        input.mProcessingDiagnostics = {
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::IncompleteHistory,
                .mContext = {.mStage = PipelineStage::HistoricalValidation,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Other",
                             .mIsin = "XX0000000002"},
                .mMessage = "Synthetic capital error.",
            },
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::MissingExchangeRate,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Coin",
                             .mIsin = "XX0000000005"},
                .mMessage = "Synthetic capital error.",
            },
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::UnsupportedTaxTreatment,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Coin",
                             .mIsin = "XX0000000005"},
                .mMessage = "Synthetic capital error.",
            },
        };
        input.mReports[0].mXml = "<capital/>";

        return input;
    };
    DecisionSession session;
    std::vector<Isin> excluded;

    start(session, input, excluded);

    const auto request = session.result().mRequests.at(0).mId;
    const auto rejections =
        session.answer({{.mRequestId = request, .mRunRevision = 1, .mValue = kCancel}});
    const auto& result = session.result();

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(excluded.empty());
    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_TRUE(result.mDiagnostics.at(0).mBlocking);

    const auto& capital = result.report(kCapital);

    EXPECT_EQ(capital.mStatus, ReportStatus::Failed);
    EXPECT_FALSE(capital.mFailureResolution.has_value());
    EXPECT_TRUE(capital.mExcludedInvestments.empty());

    // The cancel stays visible as an answer of this run.
    ASSERT_EQ(result.mDecisions.size(), 1U);
    EXPECT_TRUE(result.mDecisions.at(0).mUsed);
    EXPECT_EQ(result.mDecisions.at(0).mAnswer, kCancel);
}

TEST(FailureResolutionStageTest, ExcludingDropsTheOpenQuestionsOfTheExcludedIsin) {
    // XX0000000005 has an unsupported asset class and also waits for a loss answer (diagnostic 1
    // is linked to it).
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2024};

        input.mProcessingDiagnostics = {
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::UnsupportedTaxTreatment,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Coin",
                             .mIsin = "XX0000000005"},
                .mMessage = "Synthetic capital error.",
            },
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::LossEligibilityUnresolved,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Coin",
                             .mIsin = "XX0000000005"},
                .mMessage = "Synthetic capital error.",
            },
        };
        input.mRequests = {lossRequest("XX0000000005", "Synthetic Coin", {1})};

        return input;
    };
    DecisionSession session;
    std::vector<Isin> excluded;

    start(session, input, excluded);
    const auto& first = session.result();

    // The loss question and the exclusion question are open together.
    ASSERT_EQ(first.mRequests.size(), 2U);

    const auto failure = std::find_if(first.mRequests.begin(),
                                      first.mRequests.end(),
                                      [](const DecisionRequest& aRequest) {
                                          return aRequest.mType ==
                                                 RequestType::CapitalGainsFailureResolutionRequired;
                                      });

    ASSERT_NE(failure, first.mRequests.end());

    const auto rejections =
        session.answer({{.mRequestId = failure->mId, .mRunRevision = 1, .mValue = kExclude}});
    const auto& result = session.result();

    EXPECT_TRUE(rejections.empty());
    EXPECT_EQ(excluded, std::vector<Isin>{"XX0000000005"});
    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_FALSE(result.mDiagnostics.at(0).mBlocking);
    EXPECT_FALSE(result.mDiagnostics.at(1).mBlocking);
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::NoData);

    // The reason codes are those of the question; the diagnostic IDs are every error the
    // exclusion resolved, the loss error of the dropped question too.
    const auto& capital = result.report(kCapital);

    ASSERT_EQ(capital.mExcludedInvestments.size(), 1U);
    EXPECT_EQ(capital.mExcludedInvestments.at(0).mReasonCodes,
              (std::vector<ApplicationDiagnosticCode>{
                  ProcessingDiagnosticCode::UnsupportedTaxTreatment}));
    EXPECT_EQ(capital.mExcludedInvestments.at(0).mDiagnosticIds,
              (std::vector<std::string>{"diagnostic-0001", "diagnostic-0002"}));
}

TEST(FailureResolutionStageTest, ExcludingKeepsAQuestionWhoseErrorIsNotResolved) {
    // XX0000000005 has an unsupported asset class (diagnostic 0). It also waits for a loss answer
    // whose error (diagnostic 1) affects capital gains and dividends, so no exclusion of an
    // investment can resolve it: the question must stay open.
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2024};

        input.mProcessingDiagnostics = {
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::UnsupportedTaxTreatment,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Coin",
                             .mIsin = "XX0000000005"},
                .mMessage = "Synthetic capital error.",
            },
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::LossEligibilityUnresolved,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital, kDividends},
                             .mInstrumentName = "Synthetic Coin",
                             .mIsin = "XX0000000005"},
                .mMessage = "Synthetic error of two forms.",
            },
        };
        input.mRequests = {lossRequest("XX0000000005", "Synthetic Coin", {1})};

        return input;
    };
    DecisionSession session;
    std::vector<Isin> excluded;

    start(session, input, excluded);
    const auto& first = session.result();

    // The loss question and the exclusion question are open together.
    ASSERT_EQ(first.mRequests.size(), 2U);

    const auto failure = std::find_if(first.mRequests.begin(),
                                      first.mRequests.end(),
                                      [](const DecisionRequest& aRequest) {
                                          return aRequest.mType ==
                                                 RequestType::CapitalGainsFailureResolutionRequired;
                                      });

    ASSERT_NE(failure, first.mRequests.end());

    const auto rejections =
        session.answer({{.mRequestId = failure->mId, .mRunRevision = 1, .mValue = kExclude}});
    const auto& result = session.result();

    EXPECT_TRUE(rejections.empty());
    EXPECT_EQ(excluded, std::vector<Isin>{"XX0000000005"});

    // Only the error of the investment alone is resolved; the loss question stays open.
    EXPECT_FALSE(result.mDiagnostics.at(0).mBlocking);
    EXPECT_TRUE(result.mDiagnostics.at(1).mBlocking);
    ASSERT_EQ(result.mRequests.size(), 1U);
    EXPECT_EQ(result.mRequests.at(0).mType, RequestType::LossEligibilityConfirmationRequired);
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::NeedsInput);
}

TEST(FailureResolutionStageTest, ExcludingKeepsTheOpenQuestionOfAnotherIsin) {
    // XX0000000002 has incomplete history. XX0000000001 waits for a loss answer (diagnostic 0 is
    // linked to it) and is not listed, so exclusion does not touch it.
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2024};

        input.mProcessingDiagnostics = {
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::LossEligibilityUnresolved,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Share",
                             .mIsin = "XX0000000001"},
                .mMessage = "Synthetic capital error.",
            },
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::IncompleteHistory,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Other",
                             .mIsin = "XX0000000002"},
                .mMessage = "Synthetic capital error.",
            },
        };
        input.mRequests = {lossRequest("XX0000000001", "Synthetic Share", {0})};

        return input;
    };
    DecisionSession session;
    std::vector<Isin> excluded;

    start(session, input, excluded);
    const auto& first = session.result();

    ASSERT_EQ(first.mRequests.size(), 2U);

    const auto failure = std::find_if(first.mRequests.begin(),
                                      first.mRequests.end(),
                                      [](const DecisionRequest& aRequest) {
                                          return aRequest.mType ==
                                                 RequestType::CapitalGainsFailureResolutionRequired;
                                      });

    ASSERT_NE(failure, first.mRequests.end());
    ASSERT_EQ(failure->mInvestments.size(), 1U);
    EXPECT_EQ(failure->mInvestments.at(0).mIsin, "XX0000000002");

    const auto rejections =
        session.answer({{.mRequestId = failure->mId, .mRunRevision = 1, .mValue = kExclude}});
    const auto& result = session.result();

    EXPECT_TRUE(rejections.empty());
    EXPECT_EQ(excluded, std::vector<Isin>{"XX0000000002"});
    ASSERT_EQ(result.mRequests.size(), 1U);
    EXPECT_EQ(result.mRequests.at(0).mType, RequestType::LossEligibilityConfirmationRequired);
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::NeedsInput);
    // The loss error of XX0000000001 still waits for its answer; the error of XX0000000002 is
    // resolved.
    EXPECT_TRUE(result.mDiagnostics.at(0).mBlocking);
    EXPECT_FALSE(result.mDiagnostics.at(1).mBlocking);
}

TEST(FailureResolutionStageTest, AnAnswerDoesNotCoverAChangedList) {
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2024};

        // XX0000000002 has incomplete history; XX0000000005 has a missing exchange rate and an
        // unsupported asset class.
        input.mProcessingDiagnostics = {
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::IncompleteHistory,
                .mContext = {.mStage = PipelineStage::HistoricalValidation,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Other",
                             .mIsin = "XX0000000002"},
                .mMessage = "Synthetic capital error.",
            },
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::MissingExchangeRate,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Coin",
                             .mIsin = "XX0000000005"},
                .mMessage = "Synthetic capital error.",
            },
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::UnsupportedTaxTreatment,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Coin",
                             .mIsin = "XX0000000005"},
                .mMessage = "Synthetic capital error.",
            },
        };
        input.mReports[0].mXml = "<capital/>";

        return input;
    };
    DecisionSession session;
    std::vector<Isin> excluded;

    start(session, input, excluded);

    const auto request = session.result().mRequests.at(0).mId;

    EXPECT_TRUE(
        session.answer({{.mRequestId = request, .mRunRevision = 1, .mValue = kExclude}}).empty());
    EXPECT_EQ(excluded.size(), 2U);

    // A new run with only one of the two investments is another question.
    const auto& second = session.start([&excluded](RunDecisions& aDecisions) {
        ApplicationResultInput input{.mReportingYear = 2024};

        input.mProcessingDiagnostics = {ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::IncompleteHistory,
            .mContext = {.mStage = PipelineStage::FifoTax,
                         .mAffectedReports = {kCapital},
                         .mInstrumentName = "Synthetic Other",
                         .mIsin = "XX0000000002"},
            .mMessage = "Synthetic capital error.",
        }};
        excluded = FailureResolutionStage::resolve(input, aDecisions, kCapitalFailure);

        return input;
    });

    EXPECT_TRUE(excluded.empty());
    ASSERT_EQ(second.mRequests.size(), 1U);
    EXPECT_EQ(second.report(kCapital).mStatus, ReportStatus::NeedsInput);

    // The old answer stays recorded, from an earlier run, and was not used.
    ASSERT_EQ(second.mDecisions.size(), 1U);
    EXPECT_TRUE(second.mDecisions.at(0).mFromEarlierRun);
    EXPECT_FALSE(second.mDecisions.at(0).mUsed);
}

TEST(FailureResolutionStageTest, TheSameListInALaterRunUsesTheRecordedAnswer) {
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2024};

        // XX0000000002 has incomplete history; XX0000000005 has a missing exchange rate and an
        // unsupported asset class.
        input.mProcessingDiagnostics = {
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::IncompleteHistory,
                .mContext = {.mStage = PipelineStage::HistoricalValidation,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Other",
                             .mIsin = "XX0000000002"},
                .mMessage = "Synthetic capital error.",
            },
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::MissingExchangeRate,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Coin",
                             .mIsin = "XX0000000005"},
                .mMessage = "Synthetic capital error.",
            },
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::UnsupportedTaxTreatment,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Coin",
                             .mIsin = "XX0000000005"},
                .mMessage = "Synthetic capital error.",
            },
        };
        input.mReports[0].mXml = "<capital/>";

        return input;
    };
    DecisionSession session;
    std::vector<Isin> excluded;

    start(session, input, excluded);

    const auto request = session.result().mRequests.at(0).mId;

    EXPECT_TRUE(session.answer({{.mRequestId = request, .mRunRevision = 1, .mValue = kFailReport}})
                    .empty());

    start(session, input, excluded);
    const auto& second = session.result();

    EXPECT_TRUE(second.mRequests.empty());
    EXPECT_EQ(second.report(kCapital).mFailureResolution, FailureResolution::FailReport);
    ASSERT_EQ(second.mDecisions.size(), 1U);
    EXPECT_TRUE(second.mDecisions.at(0).mFromEarlierRun);
    EXPECT_TRUE(second.mDecisions.at(0).mUsed);
}

// --- The Doh-Obr form ---

/// An error of one coupon ISIN that only Doh-Obr has: a missing exchange rate.
ProcessingDiagnostic couponError(const Isin& aIsin, const std::string& aName) {
    return ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::MissingExchangeRate,
        .mContext = {.mStage = PipelineStage::FifoTax,
                     .mAffectedReports = {kOtherInterest},
                     .mInstrumentName = aName,
                     .mIsin = aIsin},
        .mMessage = "Synthetic coupon error.",
    };
}

TEST(FailureResolutionStageTest, TheOtherInterestFormListsOnlyItsOwnErrorsInItsOwnRequestType) {
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2025};

        // XX0000000010: a Doh-Obr error and a Doh-KDVP error. XX0000000028: a Doh-Div error.
        input.mProcessingDiagnostics = {
            couponError("XX0000000010", "Synthetic Bond A"),
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::IncompleteHistory,
                .mContext = {.mStage = PipelineStage::HistoricalValidation,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Bond A",
                             .mIsin = "XX0000000010"},
                .mMessage = "Synthetic capital error.",
            },
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::MissingExchangeRate,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kDividends},
                             .mInstrumentName = "Synthetic Share",
                             .mIsin = "XX0000000028"},
                .mMessage = "Synthetic dividend error.",
            },
        };

        return input;
    };
    DecisionSession session;
    std::vector<Isin> excluded;

    start(session, input, excluded, kOtherInterestFailure);

    ASSERT_EQ(session.result().mRequests.size(), 1U);

    const auto& request = session.result().mRequests.at(0);

    EXPECT_EQ(request.mType, RequestType::OtherInterestFailureResolutionRequired);
    EXPECT_EQ(request.mAffectedReports, std::vector<ReportType>{kOtherInterest});
    ASSERT_EQ(request.mInvestments.size(), 1U);
    EXPECT_EQ(request.mInvestments.at(0).mIsin, "XX0000000010");
    EXPECT_EQ(
        request.mInvestments.at(0).mReasonCodes,
        std::vector<ApplicationDiagnosticCode>{ProcessingDiagnosticCode::MissingExchangeRate});
}

TEST(FailureResolutionStageTest, ExcludingFromOtherInterestResolvesOnlyItsErrorsOfTheIsin) {
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2025};

        // The same ISIN has an error in Doh-Obr and one in Doh-Div. The user excludes it from
        // Doh-Obr only.
        input.mProcessingDiagnostics = {
            couponError("XX0000000010", "Synthetic Bond A"),
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::MissingExchangeRate,
                .mContext = {.mStage = PipelineStage::FifoTax,
                             .mAffectedReports = {kDividends},
                             .mInstrumentName = "Synthetic Bond A",
                             .mIsin = "XX0000000010"},
                .mMessage = "Synthetic dividend error.",
            },
        };
        input.mReports[static_cast<std::size_t>(kOtherInterest)].mXml = "<obr/>";

        return input;
    };
    DecisionSession session;
    std::vector<Isin> excluded;

    start(session, input, excluded, kOtherInterestFailure);

    const auto rejections = session.answer({{.mRequestId = session.result().mRequests.at(0).mId,
                                             .mRunRevision = 1,
                                             .mValue = kExclude}});
    const auto& result = session.result();

    EXPECT_TRUE(rejections.empty());
    EXPECT_EQ(excluded, std::vector<Isin>{"XX0000000010"});

    // The result orders the diagnostics by report: the Doh-Div error is diagnostic 1 and stays
    // blocking, the Doh-Obr error is diagnostic 2 and is resolved.
    ASSERT_EQ(result.mDiagnostics.size(), 2U);

    const auto& obr = result.report(kOtherInterest);

    EXPECT_EQ(obr.mStatus, ReportStatus::Generated);
    ASSERT_EQ(obr.mExcludedInvestments.size(), 1U);
    EXPECT_EQ(obr.mExcludedInvestments.at(0).mDiagnosticIds,
              std::vector<std::string>{"diagnostic-0002"});

    EXPECT_EQ(result.mDiagnostics.at(0).mAffectedReports, std::vector<ReportType>{kDividends});
    EXPECT_TRUE(result.mDiagnostics.at(0).mBlocking);
    EXPECT_EQ(result.mDiagnostics.at(1).mAffectedReports, std::vector<ReportType>{kOtherInterest});
    EXPECT_FALSE(result.mDiagnostics.at(1).mBlocking);

    EXPECT_EQ(result.report(kDividends).mStatus, ReportStatus::Failed);
    EXPECT_TRUE(result.report(kDividends).mExcludedInvestments.empty());
}

TEST(FailureResolutionStageTest, ExcludingOneIsinFromTwoFormsListsOnlyEachFormsOwnErrors) {
    // One ISIN has an error in Doh-KDVP (diagnostic 1) and one in Doh-Obr (diagnostic 2).
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2025};

        input.mProcessingDiagnostics = {
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::IncompleteHistory,
                .mContext = {.mStage = PipelineStage::HistoricalValidation,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Bond A",
                             .mIsin = "XX0000000010"},
                .mMessage = "Synthetic capital error.",
            },
            couponError("XX0000000010", "Synthetic Bond A"),
        };
        input.mReports[static_cast<std::size_t>(kCapital)].mXml = "<capital/>";
        input.mReports[static_cast<std::size_t>(kOtherInterest)].mXml = "<obr/>";

        return input;
    };
    DecisionSession session;

    session.start([&input](RunDecisions& aDecisions) {
        auto result = input();

        static_cast<void>(FailureResolutionStage::resolve(result, aDecisions, kCapitalFailure));
        static_cast<void>(
            FailureResolutionStage::resolve(result, aDecisions, kOtherInterestFailure));

        return result;
    });

    ASSERT_EQ(session.result().mRequests.size(), 2U);

    const auto capitalRequest = session.result().mRequests.at(0).mId;
    const auto obrRequest = session.result().mRequests.at(1).mId;

    EXPECT_TRUE(session
                    .answer({{.mRequestId = capitalRequest, .mRunRevision = 1, .mValue = kExclude},
                             {.mRequestId = obrRequest, .mRunRevision = 1, .mValue = kExclude}})
                    .empty());

    const auto& result = session.result();

    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kOtherInterest).mStatus, ReportStatus::Generated);

    ASSERT_EQ(result.report(kCapital).mExcludedInvestments.size(), 1U);
    EXPECT_EQ(result.report(kCapital).mExcludedInvestments.at(0).mDiagnosticIds,
              std::vector<std::string>{"diagnostic-0001"});
    ASSERT_EQ(result.report(kOtherInterest).mExcludedInvestments.size(), 1U);
    EXPECT_EQ(result.report(kOtherInterest).mExcludedInvestments.at(0).mDiagnosticIds,
              std::vector<std::string>{"diagnostic-0002"});
}

TEST(FailureResolutionStageTest, ARejectedCouponRowSuppressesTheOtherInterestQuestion) {
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2025};

        input.mProcessingDiagnostics = {couponError("XX0000000010", "Synthetic Bond A")};
        input.mMergeDiagnostics = {SourcedParseDiagnostic{
            .mSourceIndex = 0,
            .mBroker = Broker::TradeRepublic,
            .mDiagnostic = {.mSeverity = DiagnosticSeverity::Error,
                            .mCode = DiagnosticCode::InvalidValue,
                            .mSourceFile = "history.csv",
                            .mRowIndex = 3,
                            .mMessage = "Synthetic parse error.",
                            .mRowKind = ParsedRowKind::BondInterest},
        }};

        return input;
    };
    DecisionSession session;
    std::vector<Isin> excluded;

    start(session, input, excluded, kOtherInterestFailure);

    EXPECT_TRUE(excluded.empty());
    EXPECT_TRUE(session.result().mRequests.empty());
    EXPECT_EQ(session.result().report(kOtherInterest).mStatus, ReportStatus::Failed);
}

/// An error of one dividend payment: a missing rate of the ISIN, Doh-Div only.
ProcessingDiagnostic dividendError(const Isin& aIsin, const std::string& aName) {
    return ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::MissingExchangeRate,
        .mContext = {.mStage = PipelineStage::FifoTax,
                     .mAffectedReports = {kDividends},
                     .mInstrumentName = aName,
                     .mIsin = aIsin},
        .mMessage = "Synthetic dividend error.",
    };
}

TEST(FailureResolutionStageTest, TheDividendFormListsOnlyItsOwnErrorsInItsOwnRequestType) {
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2025};

        input.mProcessingDiagnostics = {dividendError("XX0000000010", "Synthetic Payer A"),
                                        couponError("XX0000000028", "Synthetic Bond B")};

        return input;
    };
    DecisionSession session;
    std::vector<Isin> excluded;

    start(session, input, excluded, kDividendsFailure);

    ASSERT_EQ(session.result().mRequests.size(), 1U);
    EXPECT_EQ(session.result().mRequests.at(0).mType,
              RequestType::DividendsFailureResolutionRequired);
    EXPECT_EQ(session.result().mRequests.at(0).mAffectedReports,
              std::vector<ReportType>{kDividends});
    ASSERT_EQ(session.result().mRequests.at(0).mInvestments.size(), 1U);
    EXPECT_EQ(session.result().mRequests.at(0).mInvestments.at(0).mIsin, "XX0000000010");
}

TEST(FailureResolutionStageTest, ARejectedDividendRowSuppressesTheDividendQuestion) {
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2025};

        input.mProcessingDiagnostics = {dividendError("XX0000000010", "Synthetic Payer A")};
        input.mMergeDiagnostics = {SourcedParseDiagnostic{
            .mSourceIndex = 0,
            .mBroker = Broker::TradeRepublic,
            .mDiagnostic = {.mSeverity = DiagnosticSeverity::Error,
                            .mCode = DiagnosticCode::InvalidValue,
                            .mSourceFile = "history.csv",
                            .mRowIndex = 3,
                            .mMessage = "Synthetic parse error.",
                            .mRowKind = ParsedRowKind::Dividend},
        }};

        return input;
    };
    DecisionSession session;
    std::vector<Isin> excluded;

    start(session, input, excluded, kDividendsFailure);

    EXPECT_TRUE(excluded.empty());
    EXPECT_TRUE(session.result().mRequests.empty());
    EXPECT_EQ(session.result().report(kDividends).mStatus, ReportStatus::Failed);
}

TEST(FailureResolutionStageTest, ExcludingOneIsinFromDividendsAndCapitalResolvesEachFormsOwnError) {
    // One ISIN has an error in Doh-KDVP (diagnostic 1) and one in Doh-Div (diagnostic 2). Only the
    // capital choice is made: the dividend question stays open.
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2025};

        input.mProcessingDiagnostics = {
            ProcessingDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = ProcessingDiagnosticCode::IncompleteHistory,
                .mContext = {.mStage = PipelineStage::HistoricalValidation,
                             .mAffectedReports = {kCapital},
                             .mInstrumentName = "Synthetic Payer A",
                             .mIsin = "XX0000000010"},
                .mMessage = "Synthetic capital error.",
            },
            dividendError("XX0000000010", "Synthetic Payer A"),
        };
        input.mReports[static_cast<std::size_t>(kCapital)].mXml = "<capital/>";

        return input;
    };
    DecisionSession session;

    session.start([&input](RunDecisions& aDecisions) {
        auto result = input();

        static_cast<void>(FailureResolutionStage::resolve(result, aDecisions, kCapitalFailure));
        static_cast<void>(FailureResolutionStage::resolve(result, aDecisions, kDividendsFailure));

        return result;
    });

    ASSERT_EQ(session.result().mRequests.size(), 2U);

    const auto capitalRequest = session.result().mRequests.at(0).mId;

    EXPECT_TRUE(
        session.answer({{.mRequestId = capitalRequest, .mRunRevision = 1, .mValue = kExclude}})
            .empty());

    const auto& result = session.result();

    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Generated);
    EXPECT_EQ(result.report(kDividends).mStatus, ReportStatus::NeedsInput);
    ASSERT_EQ(result.mRequests.size(), 1U);
    EXPECT_EQ(result.mRequests.at(0).mType, RequestType::DividendsFailureResolutionRequired);
    EXPECT_TRUE(result.report(kDividends).mExcludedInvestments.empty());
}

TEST(FailureResolutionStageTest, AnXmlErrorOfADividendIsinAsksNoExclusion) {
    const auto input = []() {
        ApplicationResultInput input{.mReportingYear = 2025};

        // The Doh-Div writer found the error in the final document, so no ISIN can be left out.
        input.mProcessingDiagnostics = {ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::XmlValueOutOfRange,
            .mContext = {.mStage = PipelineStage::XmlGeneration,
                         .mAffectedReports = {kDividends},
                         .mInstrumentName = "Synthetic Payer A",
                         .mIsin = "XX0000000001"},
            .mMessage = "Synthetic XML error.",
        }};

        return input;
    };
    DecisionSession session;
    std::vector<Isin> excluded;

    start(session, input, excluded, kDividendsFailure);

    EXPECT_TRUE(excluded.empty());
    EXPECT_TRUE(session.result().mRequests.empty());
    EXPECT_EQ(session.result().report(kDividends).mStatus, ReportStatus::Failed);
}

TEST(FailureResolutionStageTest, AnXmlErrorOfOtherInterestAsksNoExclusionForAnIsinOrABroker) {
    const auto isinInput = []() {
        ApplicationResultInput input{.mReportingYear = 2025};

        input.mProcessingDiagnostics = {ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::XmlValueOutOfRange,
            .mContext = {.mStage = PipelineStage::XmlGeneration,
                         .mAffectedReports = {kOtherInterest},
                         .mInstrumentName = "Synthetic Bond A",
                         .mIsin = "XX0000000002"},
            .mMessage = "Synthetic XML error.",
        }};

        return input;
    };
    const auto brokerInput = []() {
        ApplicationResultInput input{.mReportingYear = 2025};

        // A cash-interest error of one broker, found in the final document.
        input.mProcessingDiagnostics = {ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::XmlValueOutOfRange,
            .mContext = {.mStage = PipelineStage::XmlGeneration,
                         .mAffectedReports = {kOtherInterest},
                         .mCashInterestBroker = Broker::TradeRepublic},
            .mMessage = "Synthetic XML error.",
        }};

        return input;
    };
    DecisionSession isinSession;
    DecisionSession brokerSession;
    std::vector<Isin> excluded;

    start(isinSession, isinInput, excluded, kOtherInterestFailure);

    EXPECT_TRUE(excluded.empty());
    EXPECT_TRUE(isinSession.result().mRequests.empty());
    EXPECT_EQ(isinSession.result().report(kOtherInterest).mStatus, ReportStatus::Failed);

    start(brokerSession, brokerInput, excluded, kOtherInterestFailure);

    EXPECT_TRUE(excluded.empty());
    EXPECT_TRUE(brokerSession.result().mRequests.empty());
    EXPECT_EQ(brokerSession.result().report(kOtherInterest).mStatus, ReportStatus::Failed);
}

} // namespace
