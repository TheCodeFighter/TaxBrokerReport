#include "taxbroker/api/application_result_json.hpp"
#include "taxbroker/api/diagnostics_json.hpp"
#include "taxbroker/application_result.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace taxbroker;

constexpr auto kCapital = ReportType::CapitalGains;

Date day(std::string_view aDate) {
    return *parseCalendarDate(aDate);
}

nlohmann::json serialize(const ApplicationResult& aResult) {
    return nlohmann::json::parse(api::serializeApplicationResultJson(aResult));
}

TEST(ApplicationResultJsonTest, WritesTheDocumentedPartialResultExample) {
    // The partial-result example of diagnostics.md without the fields of the decision
    // lifecycle (run revision, options and investments).
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mProcessingDiagnostics = {ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::IncompleteHistory,
        .mContext =
            {
                .mStage = PipelineStage::HistoricalValidation,
                .mAffectedReports = {kCapital},
                .mSources = {SourceReference{
                    .mBroker = Broker::TradeRepublic,
                    .mFilename = SourceFilename::fromPath("TransactionExport.csv"),
                    .mSourceRow = 42,
                    .mInputSequence = {1, 40},
                }},
                .mInstrumentName = "Example Investment",
                .mIsin = "XX0000000001",
            },
        .mMessage = "Earlier purchase history is missing.",
        .mMessageKey = MessageKey::IncompleteHistory,
    }};
    input.mRequests = {RequestInput{
        .mQuestion =
            {
                .mType = RequestType::CapitalGainsFailureResolutionRequired,
                .mAffectedReports = {kCapital},
                .mSources = {SourceReference{
                    .mBroker = Broker::TradeRepublic,
                    .mFilename = SourceFilename::fromPath("TransactionExport.csv"),
                    .mSourceRow = 42,
                    .mInputSequence = {1, 40},
                }},
                .mData =
                    FailureResolutionQuestion{
                        .mInvestments = {FailedInvestment{
                            .mIsin = "XX0000000001",
                            .mName = "Example Investment",
                            .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
                        }},
                    },
            },
        .mProcessingDiagnostics = {0},
    }};
    input.mReports[1].mXml = "<Envelope>dividends</Envelope>";

    const auto text =
        api::serializeApplicationResultJson(ApplicationResultBuilder::build(std::move(input)));

    // ordered_json keeps the key order of the literal, which is the order of the doc example.
    const auto expected = nlohmann::ordered_json::parse(R"({
      "schemaVersion": 2,
      "status": "needs_input",
      "runRevision": 1,
      "reports": {
        "capitalGains": {
          "status": "needs_input",
          "diagnosticIds": ["diagnostic-0001"],
          "requestIds": ["request-0001"],
          "excludedInvestments": [],
          "unsafe": false
        },
        "dividends": {
          "status": "generated",
          "diagnosticIds": [],
          "requestIds": [],
          "unsafe": false,
          "downloadAvailable": true
        },
        "bankInterest": {
          "status": "no_data",
          "diagnosticIds": [],
          "requestIds": [],
          "unsafe": false
        },
        "otherInterest": {
          "status": "no_data",
          "diagnosticIds": [],
          "requestIds": [],
          "excludedInvestments": [],
          "unsafe": false
        }
      },
      "diagnostics": [
        {
          "id": "diagnostic-0001",
          "severity": "error",
          "stage": "historical_validation",
          "code": "incomplete_history",
          "message": "Earlier purchase history is missing.",
          "messageKey": "incomplete_history",
          "messageParams": {},
          "affectedReports": ["capital_gains"],
          "blocking": true,
          "sources": [
            {
              "broker": "trade_republic",
              "sourceId": "source-0002",
              "file": "TransactionExport.csv",
              "row": 42
            }
          ],
          "instrument": {
            "name": "Example Investment",
            "isin": "XX0000000001"
          },
          "requestId": "request-0001"
        }
      ],
      "requests": [
        {
          "id": "request-0001",
          "type": "capital_gains_failure_resolution_required",
          "runRevision": 1,
          "affectedReports": ["capital_gains"],
          "sources": [
            {
              "broker": "trade_republic",
              "sourceId": "source-0002",
              "file": "TransactionExport.csv",
              "row": 42
            }
          ],
          "options": ["exclude_failed_instruments", "fail_report"],
          "investments": [
            {
              "name": "Example Investment",
              "isin": "XX0000000001",
              "reasonCodes": ["incomplete_history"],
              "diagnosticIds": ["diagnostic-0001"]
            }
          ]
        }
      ],
      "decisions": []
    })");

    EXPECT_EQ(text, expected.dump());
}

TEST(ApplicationResultJsonTest, NeverContainsTheGeneratedXml) {
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mReports[0].mXml = "<Envelope>secret-content</Envelope>";

    const auto text =
        api::serializeApplicationResultJson(ApplicationResultBuilder::build(std::move(input)));

    EXPECT_EQ(text.find("secret-content"), std::string::npos);
    EXPECT_EQ(text.find("Envelope"), std::string::npos);
    EXPECT_EQ(nlohmann::json::parse(text).at("reports").at("capitalGains").at("downloadAvailable"),
              true);
}

TEST(ApplicationResultJsonTest, WritesTheReasonOfANoDataReportAndNoDownload) {
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mReports[0].mNoDataReason = NoDataReason::NoSelectedYearDisposals;
    input.mReports[2].mNoDataReason = NoDataReason::BelowDepositInterestThreshold;

    const auto json = serialize(ApplicationResultBuilder::build(std::move(input)));

    const auto& reports = json.at("reports");

    EXPECT_EQ(reports.at("capitalGains").at("reason"), "no_selected_year_disposals");
    EXPECT_EQ(reports.at("bankInterest").at("reason"), "below_deposit_interest_threshold");
    EXPECT_FALSE(reports.at("capitalGains").contains("downloadAvailable"));
    EXPECT_FALSE(reports.at("dividends").contains("reason"));
    EXPECT_EQ(json.at("status"), "success");
}

TEST(ApplicationResultJsonTest, OmitsOptionalFieldsThatAreNotAvailable) {
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mProcessingDiagnostics = {ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
        .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kCapital}},
        .mMessage = "The calculation exceeds the supported integer range.",
    }};

    const auto json = serialize(ApplicationResultBuilder::build(std::move(input)));

    const auto& diagnostic = json.at("diagnostics").at(0);

    EXPECT_FALSE(diagnostic.contains("messageKey"));
    EXPECT_FALSE(diagnostic.contains("messageParams"));
    EXPECT_FALSE(diagnostic.contains("taxDate"));
    EXPECT_FALSE(diagnostic.contains("field"));
    EXPECT_FALSE(diagnostic.contains("instrument"));
    EXPECT_FALSE(diagnostic.contains("requestId"));
    EXPECT_TRUE(diagnostic.at("sources").empty());
}

TEST(ApplicationResultJsonTest, WritesTheTaxDateAsAnIsoDate) {
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mProcessingDiagnostics = {ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
        .mContext = {.mStage = PipelineStage::FifoTax,
                     .mAffectedReports = {kCapital},
                     .mTaxDate = Date{std::chrono::sys_days{std::chrono::year{2024} / 3 / 5}
                                          .time_since_epoch()},
                     .mField = "price"},
        .mMessage = "Synthetic processing message.",
    }};

    const auto json = serialize(ApplicationResultBuilder::build(std::move(input)));

    EXPECT_EQ(json.at("diagnostics").at(0).at("taxDate"), "2024-03-05");
    EXPECT_EQ(json.at("diagnostics").at(0).at("field"), "price");
}

TEST(ApplicationResultJsonTest, FilesWithTheSameNameGetDifferentSourceIds) {
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mMergeDiagnostics = {
        SourcedParseDiagnostic{
            .mSourceIndex = 1,
            .mBroker = Broker::TradeRepublic,
            .mDiagnostic =
                ParseDiagnostic{
                    .mSeverity = DiagnosticSeverity::Error,
                    .mCode = DiagnosticCode::MissingField,
                    .mSourceFile = "/home/user/2023/taxreport.csv",
                    .mRowIndex = 42,
                    .mField = "amount",
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
                    .mSourceFile = R"(C:\Users\user\2022\taxreport.csv)",
                    .mRowIndex = 42,
                    .mField = "amount",
                    .mMessage = "Synthetic parser message.",
                    .mRowKind = ParsedRowKind::Trade,
                },
        },
    };

    const auto text =
        api::serializeApplicationResultJson(ApplicationResultBuilder::build(std::move(input)));
    const auto json = nlohmann::json::parse(text);

    const auto& diagnostics = json.at("diagnostics");

    ASSERT_EQ(diagnostics.size(), 2U);

    EXPECT_EQ(diagnostics[0].at("sources").at(0).at("sourceId"), "source-0001");
    EXPECT_EQ(diagnostics[0].at("sources").at(0).at("file"), "taxreport.csv");
    EXPECT_EQ(diagnostics[1].at("sources").at(0).at("sourceId"), "source-0002");
    EXPECT_EQ(diagnostics[1].at("sources").at(0).at("file"), "taxreport.csv");

    // No directory or parent-folder name distinguishes the two files.
    EXPECT_EQ(text.find("home"), std::string::npos);
    EXPECT_EQ(text.find("Users"), std::string::npos);
    EXPECT_EQ(text.find("2023"), std::string::npos);
    EXPECT_EQ(text.find("2022"), std::string::npos);
}

TEST(ApplicationResultJsonTest, ReorderingTheFilesChangesTheirSourceIds) {
    const SourcedParseDiagnostic first{
        .mSourceIndex = 0,
        .mBroker = Broker::TradeRepublic,
        .mDiagnostic =
            ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = DiagnosticCode::MissingField,
                .mSourceFile = "a.csv",
                .mRowIndex = 5,
                .mField = "amount",
                .mMessage = "Synthetic parser message.",
                .mRowKind = ParsedRowKind::Trade,
            },
    };
    const SourcedParseDiagnostic second{
        .mSourceIndex = 1,
        .mBroker = Broker::TradeRepublic,
        .mDiagnostic =
            ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = DiagnosticCode::MissingField,
                .mSourceFile = "b.csv",
                .mRowIndex = 5,
                .mField = "amount",
                .mMessage = "Synthetic parser message.",
                .mRowKind = ParsedRowKind::Trade,
            },
    };
    const SourcedParseDiagnostic firstAfterSwap{
        .mSourceIndex = 1,
        .mBroker = Broker::TradeRepublic,
        .mDiagnostic =
            ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = DiagnosticCode::MissingField,
                .mSourceFile = "a.csv",
                .mRowIndex = 5,
                .mField = "amount",
                .mMessage = "Synthetic parser message.",
                .mRowKind = ParsedRowKind::Trade,
            },
    };
    const SourcedParseDiagnostic secondAfterSwap{
        .mSourceIndex = 0,
        .mBroker = Broker::TradeRepublic,
        .mDiagnostic =
            ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = DiagnosticCode::MissingField,
                .mSourceFile = "b.csv",
                .mRowIndex = 5,
                .mField = "amount",
                .mMessage = "Synthetic parser message.",
                .mRowKind = ParsedRowKind::Trade,
            },
    };

    const auto original = serialize(ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024, .mMergeDiagnostics = {first, second}}));
    const auto swapped = serialize(ApplicationResultBuilder::build(
        ApplicationResultInput{.mReportingYear = 2024,
                               .mMergeDiagnostics = {firstAfterSwap, secondAfterSwap}}));

    // Diagnostics are sorted by file position, so the first one belongs to the first file.
    const auto& originalDiagnostics = original.at("diagnostics");

    ASSERT_EQ(originalDiagnostics.size(), 2U);

    EXPECT_EQ(originalDiagnostics[0].at("sources").at(0).at("file"), "a.csv");
    EXPECT_EQ(originalDiagnostics[0].at("sources").at(0).at("sourceId"), "source-0001");
    EXPECT_EQ(originalDiagnostics[1].at("sources").at(0).at("file"), "b.csv");
    EXPECT_EQ(originalDiagnostics[1].at("sources").at(0).at("sourceId"), "source-0002");

    const auto& swappedDiagnostics = swapped.at("diagnostics");

    ASSERT_EQ(swappedDiagnostics.size(), 2U);

    EXPECT_EQ(swappedDiagnostics[0].at("sources").at(0).at("file"), "b.csv");
    EXPECT_EQ(swappedDiagnostics[0].at("sources").at(0).at("sourceId"), "source-0001");
    EXPECT_EQ(swappedDiagnostics[1].at("sources").at(0).at("file"), "a.csv");
    EXPECT_EQ(swappedDiagnostics[1].at("sources").at(0).at("sourceId"), "source-0002");
}

TEST(ApplicationResultJsonTest, AnUnusableFilenameIsOmittedButTheSourceIdAndRowRemain) {
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mMergeDiagnostics = {
        SourcedParseDiagnostic{
            .mSourceIndex = 2,
            .mBroker = Broker::TradeRepublic,
            .mDiagnostic =
                ParseDiagnostic{
                    .mSeverity = DiagnosticSeverity::Error,
                    .mCode = DiagnosticCode::MissingField,
                    .mSourceFile = "exports/",
                    .mRowIndex = 42,
                    .mField = "amount",
                    .mMessage = "Synthetic parser message.",
                    .mRowKind = ParsedRowKind::Trade,
                },
        },
    };

    const auto json = serialize(ApplicationResultBuilder::build(std::move(input)));

    const auto& source = json.at("diagnostics").at(0).at("sources").at(0);

    EXPECT_FALSE(source.contains("file"));
    EXPECT_EQ(source.at("sourceId"), "source-0003");
    EXPECT_EQ(source.at("row"), 42);
}

TEST(ApplicationResultJsonTest, TheTenThousandthFileGetsAFiveDigitSourceId) {
    // Request index 9999 is the 10,000th file.
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mMergeDiagnostics = {
        SourcedParseDiagnostic{
            .mSourceIndex = 9'999,
            .mBroker = Broker::TradeRepublic,
            .mDiagnostic =
                ParseDiagnostic{
                    .mSeverity = DiagnosticSeverity::Error,
                    .mCode = DiagnosticCode::MissingField,
                    .mSourceFile = "export.csv",
                    .mRowIndex = 2,
                    .mField = "amount",
                    .mMessage = "Synthetic parser message.",
                    .mRowKind = ParsedRowKind::Trade,
                },
        },
    };

    const auto json = serialize(ApplicationResultBuilder::build(std::move(input)));

    EXPECT_EQ(json.at("diagnostics").at(0).at("sources").at(0).at("sourceId"), "source-10000");
}

TEST(ApplicationResultJsonTest, AnInstrumentWithOnlyAnIsinHasNoName) {
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mProcessingDiagnostics = {ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::IncompleteHistory,
        .mContext = {.mStage = PipelineStage::FifoTax,
                     .mAffectedReports = {kCapital},
                     .mInstrumentName = std::nullopt,
                     .mIsin = "XX0000000001"},
        .mMessage = "Synthetic processing message.",
    }};

    const auto json = serialize(ApplicationResultBuilder::build(std::move(input)));

    const auto& instrument = json.at("diagnostics").at(0).at("instrument");

    EXPECT_EQ(instrument, nlohmann::json::parse(R"({"isin": "XX0000000001"})"));
}

TEST(ApplicationResultJsonTest, AnInstrumentWithOnlyANameHasNoIsin) {
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mProcessingDiagnostics = {ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = ProcessingDiagnosticCode::IncompleteHistory,
        .mContext = {.mStage = PipelineStage::FifoTax,
                     .mAffectedReports = {kCapital},
                     .mInstrumentName = "Example Investment",
                     .mIsin = std::nullopt},
        .mMessage = "Synthetic processing message.",
    }};

    const auto json = serialize(ApplicationResultBuilder::build(std::move(input)));

    const auto& instrument = json.at("diagnostics").at(0).at("instrument");

    EXPECT_EQ(instrument, nlohmann::json::parse(R"({"name": "Example Investment"})"));
}

TEST(ApplicationResultJsonTest, WritesEveryPipelineStageAndProcessingCode) {
    // The diagnostics are sorted by stage, so the output follows the stage order.
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mProcessingDiagnostics = {
        ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::XmlValueOutOfRange,
            .mContext = {.mStage = PipelineStage::XmlGeneration, .mAffectedReports = {kCapital}},
            .mMessage = "Synthetic processing message.",
        },
        ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::IncompleteHistory,
            .mContext = {.mStage = PipelineStage::XmlGeneration, .mAffectedReports = {kCapital}},
            .mMessage = "Synthetic processing message.",
        },
        ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::InvalidReportingYear,
            .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kCapital}},
            .mMessage = "Synthetic processing message.",
        },
        ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::CorporateActionUnrepresentable,
            .mContext = {.mStage = PipelineStage::CorporateAction, .mAffectedReports = {kCapital}},
            .mMessage = "Synthetic processing message.",
        },
        ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::InvalidExchangeRate,
            .mContext = {.mStage = PipelineStage::HistoricalValidation,
                         .mAffectedReports = {kCapital}},
            .mMessage = "Synthetic processing message.",
        },
        ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::ArithmeticOverflow,
            .mContext = {.mStage = PipelineStage::Merging, .mAffectedReports = {kCapital}},
            .mMessage = "Synthetic processing message.",
        },
        ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::UnrepresentableValue,
            .mContext = {.mStage = PipelineStage::Parsing, .mAffectedReports = {kCapital}},
            .mMessage = "Synthetic processing message.",
        },
    };

    const auto json = serialize(ApplicationResultBuilder::build(std::move(input)));

    const auto& diagnostics = json.at("diagnostics");

    ASSERT_EQ(diagnostics.size(), 7U);

    EXPECT_EQ(diagnostics[0].at("stage"), "parsing");
    EXPECT_EQ(diagnostics[0].at("code"), "unrepresentable_value");
    EXPECT_EQ(diagnostics[1].at("stage"), "merging");
    EXPECT_EQ(diagnostics[1].at("code"), "arithmetic_overflow");
    EXPECT_EQ(diagnostics[2].at("stage"), "historical_validation");
    EXPECT_EQ(diagnostics[2].at("code"), "invalid_exchange_rate");
    EXPECT_EQ(diagnostics[3].at("stage"), "corporate_action");
    EXPECT_EQ(diagnostics[3].at("code"), "corporate_action_unrepresentable");
    EXPECT_EQ(diagnostics[4].at("stage"), "fifo_tax");
    EXPECT_EQ(diagnostics[4].at("code"), "invalid_reporting_year");
    EXPECT_EQ(diagnostics[5].at("stage"), "xml_generation");
    EXPECT_EQ(diagnostics[5].at("code"), "incomplete_history");
    EXPECT_EQ(diagnostics[6].at("stage"), "xml_generation");
    EXPECT_EQ(diagnostics[6].at("code"), "xml_value_out_of_range");
}

TEST(ApplicationResultJsonTest, WritesEveryCapitalGateCode) {
    // The capital report gates, all for capital gains. corporate_action_not_applied is in the
    // earlier corporate-action stage, so it comes first; the other four share the FIFO and tax
    // stage, so their code order decides.
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mProcessingDiagnostics = {
        ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::LossEligibilityUnresolved,
            .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kCapital}},
            .mMessage = "Synthetic processing message.",
        },
        ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::MissingExchangeRate,
            .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kCapital}},
            .mMessage = "Synthetic processing message.",
        },
        ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::CorporateActionNotApplied,
            .mContext = {.mStage = PipelineStage::CorporateAction, .mAffectedReports = {kCapital}},
            .mMessage = "Synthetic processing message.",
        },
        ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::TaxClassificationUnknown,
            .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kCapital}},
            .mMessage = "Synthetic processing message.",
        },
        ProcessingDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = ProcessingDiagnosticCode::UnsupportedTaxTreatment,
            .mContext = {.mStage = PipelineStage::FifoTax, .mAffectedReports = {kCapital}},
            .mMessage = "Synthetic processing message.",
        },
    };

    const auto json = serialize(ApplicationResultBuilder::build(std::move(input)));

    const auto& diagnostics = json.at("diagnostics");

    ASSERT_EQ(diagnostics.size(), 5U);

    EXPECT_EQ(diagnostics[0].at("code"), "corporate_action_not_applied");
    EXPECT_EQ(diagnostics[1].at("code"), "unsupported_tax_treatment");
    EXPECT_EQ(diagnostics[2].at("code"), "tax_classification_unknown");
    EXPECT_EQ(diagnostics[3].at("code"), "missing_exchange_rate");
    EXPECT_EQ(diagnostics[4].at("code"), "loss_eligibility_unresolved");
}

TEST(ApplicationResultJsonTest, WritesEveryMergeCode) {
    // All six codes affect capital gains first, so the code order decides.
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mMergeDiagnostics = {
        MergeDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = MergeDiagnosticCode::DuplicateTransactionInFile,
            .mMessage = "Synthetic merge message.",
            .mEventKinds = {StatementEventKind::Trade},
        },
        MergeDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = MergeDiagnosticCode::InstrumentAssetClassConflict,
            .mMessage = "Synthetic merge message.",
            .mEventKinds = {StatementEventKind::Trade},
        },
        MergeDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = MergeDiagnosticCode::InstrumentNameConflict,
            .mMessage = "Synthetic merge message.",
            .mEventKinds = {StatementEventKind::Trade},
        },
        MergeDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = MergeDiagnosticCode::ConflictingDuplicate,
            .mMessage = "Synthetic merge message.",
            .mEventKinds = {StatementEventKind::Trade},
        },
        MergeDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = MergeDiagnosticCode::InconsistentSourceIndex,
            .mMessage = "Synthetic merge message.",
            .mEventKinds = {StatementEventKind::Trade},
        },
        MergeDiagnostic{
            .mSeverity = DiagnosticSeverity::Error,
            .mCode = MergeDiagnosticCode::DuplicateSourceIndex,
            .mMessage = "Synthetic merge message.",
            .mEventKinds = {StatementEventKind::Trade},
        },
    };

    const auto json = serialize(ApplicationResultBuilder::build(std::move(input)));

    const auto& diagnostics = json.at("diagnostics");

    ASSERT_EQ(diagnostics.size(), 6U);

    EXPECT_EQ(diagnostics[0].at("code"), "duplicate_source_index");
    EXPECT_EQ(diagnostics[1].at("code"), "inconsistent_source_index");
    EXPECT_EQ(diagnostics[2].at("code"), "conflicting_duplicate");
    EXPECT_EQ(diagnostics[3].at("code"), "instrument_name_conflict");
    EXPECT_EQ(diagnostics[4].at("code"), "instrument_asset_class_conflict");
    EXPECT_EQ(diagnostics[5].at("code"), "duplicate_transaction_in_file");
    EXPECT_EQ(diagnostics[0].at("stage"), "merging");
}

TEST(ApplicationResultJsonTest, WritesEveryParserCode) {
    // The unsupported row type affects no report, so it comes last.
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mMergeDiagnostics = {
        SourcedParseDiagnostic{
            .mSourceIndex = 0,
            .mBroker = Broker::TradeRepublic,
            .mDiagnostic =
                ParseDiagnostic{
                    .mSeverity = DiagnosticSeverity::Error,
                    .mCode = DiagnosticCode::ParseError,
                    .mSourceFile = "export.csv",
                    .mRowIndex = 5,
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
                    .mCode = DiagnosticCode::InvalidHeader,
                    .mSourceFile = "export.csv",
                    .mRowIndex = 5,
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
                    .mCode = DiagnosticCode::LocalizedNumberFormat,
                    .mSourceFile = "export.csv",
                    .mRowIndex = 5,
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
                    .mCode = DiagnosticCode::FieldCountMismatch,
                    .mSourceFile = "export.csv",
                    .mRowIndex = 5,
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
                    .mCode = DiagnosticCode::InconsistentValue,
                    .mSourceFile = "export.csv",
                    .mRowIndex = 5,
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
                    .mCode = DiagnosticCode::InvalidValue,
                    .mSourceFile = "export.csv",
                    .mRowIndex = 5,
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
                    .mRowIndex = 5,
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
                    .mCode = DiagnosticCode::UnsupportedAssetClass,
                    .mSourceFile = "export.csv",
                    .mRowIndex = 5,
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
                    .mCode = DiagnosticCode::UnsupportedRowType,
                    .mSourceFile = "export.csv",
                    .mRowIndex = 5,
                    .mMessage = "Synthetic parser message.",
                    .mRowKind = std::nullopt, // the parser gives an unsupported row type no kind
                },
        },
        SourcedParseDiagnostic{
            .mSourceIndex = 0,
            .mBroker = Broker::TradeRepublic,
            .mDiagnostic =
                ParseDiagnostic{
                    .mSeverity = DiagnosticSeverity::Error,
                    .mCode = DiagnosticCode::UnknownRowType,
                    .mSourceFile = "export.csv",
                    .mRowIndex = 5,
                    .mMessage = "Synthetic parser message.",
                    .mRowKind = ParsedRowKind::Trade,
                },
        },
    };

    const auto json = serialize(ApplicationResultBuilder::build(std::move(input)));

    const auto& diagnostics = json.at("diagnostics");

    ASSERT_EQ(diagnostics.size(), 10U);

    EXPECT_EQ(diagnostics[0].at("code"), "unknown_row_type");
    EXPECT_EQ(diagnostics[1].at("code"), "unsupported_asset_class");
    EXPECT_EQ(diagnostics[2].at("code"), "missing_field");
    EXPECT_EQ(diagnostics[3].at("code"), "invalid_value");
    EXPECT_EQ(diagnostics[4].at("code"), "inconsistent_value");
    EXPECT_EQ(diagnostics[5].at("code"), "field_count_mismatch");
    EXPECT_EQ(diagnostics[6].at("code"), "localized_number_format");
    EXPECT_EQ(diagnostics[7].at("code"), "invalid_header");
    EXPECT_EQ(diagnostics[8].at("code"), "parse_error");
    EXPECT_EQ(diagnostics[9].at("code"), "unsupported_row_type");
    EXPECT_EQ(diagnostics[0].at("stage"), "parsing");
}

TEST(ApplicationResultJsonTest, WritesEveryRequestTypeWithItsDateAndIsin) {
    // The requests share their scope, so the type order decides.
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mRequests = {
        RequestInput{
            .mQuestion =
                {
                    .mType = RequestType::OtherInterestFailureResolutionRequired,
                    .mAffectedReports = {kCapital},
                    .mTaxDate = Date{std::chrono::sys_days{std::chrono::year{2024} / 11 / 30}
                                         .time_since_epoch()},
                    .mIsin = "XX0000000001",
                    .mData =
                        FailureResolutionQuestion{
                            .mInvestments = {FailedInvestment{
                                .mIsin = "XX0000000001",
                                .mName = "Synthetic Bond",
                                .mReasonCodes = {ProcessingDiagnosticCode::UnrepresentableValue},
                            }}},
                },
        },
        RequestInput{
            .mQuestion =
                {
                    .mType = RequestType::CapitalGainsFailureResolutionRequired,
                    .mAffectedReports = {kCapital},
                    .mTaxDate = Date{std::chrono::sys_days{std::chrono::year{2024} / 11 / 30}
                                         .time_since_epoch()},
                    .mIsin = "XX0000000001",
                    .mData =
                        FailureResolutionQuestion{
                            .mInvestments = {FailedInvestment{
                                .mIsin = "XX0000000001",
                                .mName = "Synthetic Share",
                                .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
                            }}},
                },
        },
        RequestInput{
            .mQuestion =
                {
                    .mType = RequestType::IncomeDetailsRequired,
                    .mAffectedReports = {kCapital},
                    .mTaxDate = Date{std::chrono::sys_days{std::chrono::year{2024} / 11 / 30}
                                         .time_since_epoch()},
                    .mIsin = "XX0000000001",
                },
        },
        RequestInput{
            .mQuestion =
                {
                    .mType = RequestType::TaxClassificationRequired,
                    .mAffectedReports = {kCapital},
                    .mTaxDate = Date{std::chrono::sys_days{std::chrono::year{2024} / 11 / 30}
                                         .time_since_epoch()},
                    .mIsin = "XX0000000001",
                },
        },
        RequestInput{
            .mQuestion =
                {
                    .mType = RequestType::LossEligibilityConfirmationRequired,
                    .mAffectedReports = {kCapital},
                    .mTaxDate = Date{std::chrono::sys_days{std::chrono::year{2024} / 11 / 30}
                                         .time_since_epoch()},
                    .mIsin = "XX0000000001",
                    .mData =
                        LossEligibilityQuestion{
                            .mIsin = "XX0000000001",
                            .mName = "Synthetic Share",
                            .mSales =
                                {
                                    LossSaleFacts{
                                        .mSaleDate = day("2024-11-30"),
                                        .mSaleUnits = 100'000'000, // 1 unit
                                        .mLossUnits = 100'000'000,
                                        .mWindowStart = day("2024-10-31"),
                                        .mWindowEnd = day("2024-12-30"),
                                        .mOpenUnits = 100'000'000,
                                    }},
                        },
                },
        },
        RequestInput{
            .mQuestion =
                {
                    .mType = RequestType::QuantityCorrectionRequired,
                    .mAffectedReports = {kCapital},
                    .mTaxDate = Date{std::chrono::sys_days{std::chrono::year{2024} / 11 / 30}
                                         .time_since_epoch()},
                    .mIsin = "XX0000000001",
                },
        },
        RequestInput{
            .mQuestion =
                {
                    .mType = RequestType::CorporateActionRatioRequired,
                    .mAffectedReports = {kCapital},
                    .mTaxDate = Date{std::chrono::sys_days{std::chrono::year{2024} / 11 / 30}
                                         .time_since_epoch()},
                    .mIsin = "XX0000000001",
                    .mData =
                        CorporateActionRatioQuestion{
                            .mIsin = "XX0000000001",
                            .mName = "Synthetic Share",
                            .mEffectiveDate = day("2024-11-30"),
                            .mSources = {CorporateActionSourceFacts{
                                .mBroker = Broker::TradeRepublic,
                                .mTransactionId = "synthetic-split",
                                .mDate = day("2024-11-30"),
                                .mSourceValue = "3",
                            }},
                            .mPositionBefore = 300'000'000, // 3 units
                            .mDateRanges = {{.mEarliestDate = day("2024-01-11"),
                                              .mLatestDate = day("2024-12-31"),
                                              .mPositionBefore = 300'000'000}},
                        },
                },
        },
        RequestInput{
            .mQuestion =
                {
                    .mType = RequestType::CorporateActionIdentityRequired,
                    .mAffectedReports = {kCapital},
                    .mTaxDate = Date{std::chrono::sys_days{std::chrono::year{2024} / 11 / 30}
                                         .time_since_epoch()},
                    .mIsin = "XX0000000001",
                },
        },
        RequestInput{
            .mQuestion =
                {
                    .mType = RequestType::ExchangeRatesRequired,
                    .mAffectedReports = {kCapital},
                    .mTaxDate = Date{std::chrono::sys_days{std::chrono::year{2024} / 11 / 30}
                                         .time_since_epoch()},
                    .mIsin = "XX0000000001",
                },
        },
    };

    const auto json = serialize(ApplicationResultBuilder::build(std::move(input)));

    const auto& requests = json.at("requests");

    ASSERT_EQ(requests.size(), 9U);

    EXPECT_EQ(requests[0].at("type"), "exchange_rates_required");
    EXPECT_EQ(requests[1].at("type"), "corporate_action_identity_required");
    EXPECT_EQ(requests[2].at("type"), "corporate_action_ratio_required");
    EXPECT_EQ(requests[3].at("type"), "quantity_correction_required");
    EXPECT_EQ(requests[4].at("type"), "loss_eligibility_confirmation_required");
    EXPECT_EQ(requests[5].at("type"), "tax_classification_required");
    EXPECT_EQ(requests[6].at("type"), "income_details_required");
    EXPECT_EQ(requests[7].at("type"), "capital_gains_failure_resolution_required");
    EXPECT_EQ(requests[8].at("type"), "other_interest_failure_resolution_required");
    EXPECT_EQ(requests[0].at("id"), "request-0001");
    EXPECT_EQ(requests[8].at("id"), "request-0009");
    EXPECT_EQ(requests[0].at("taxDate"), "2024-11-30");
    EXPECT_EQ(requests[0].at("isin"), "XX0000000001");
}

TEST(ApplicationResultJsonTest, AFileLevelFailureListsAllFourReportsAndAnUnknownBroker) {
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mMergeDiagnostics = {SourcedParseDiagnostic{
        .mSourceIndex = 0,
        .mBroker = Broker::Unknown,
        .mDiagnostic =
            ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = DiagnosticCode::ParseError,
                .mSourceFile = "export.csv",
                .mMessage = "The file could not be read.",
            },
    }};

    const auto json = serialize(ApplicationResultBuilder::build(std::move(input)));

    EXPECT_EQ(json.at("status"), "failed");
    EXPECT_EQ(
        json.at("diagnostics").at(0).at("affectedReports"),
        (nlohmann::json{"capital_gains", "dividends", "bank_deposit_interest", "other_interest"}));
    EXPECT_EQ(json.at("diagnostics").at(0).at("sources").at(0).at("broker"), "unknown");
    EXPECT_FALSE(json.at("diagnostics").at(0).at("sources").at(0).contains("row"));
    EXPECT_EQ(json.at("reports").at("otherInterest").at("status"), "failed");
}

TEST(ApplicationResultJsonTest, WritesTheWarningStatusAndAnInteractiveBrokersSource) {
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mMergeDiagnostics = {MergeDiagnostic{
        .mSeverity = DiagnosticSeverity::Warning,
        .mCode = MergeDiagnosticCode::DuplicateTransactionInFile,
        .mMessage = "Synthetic merge message.",
        .mSources = {SourceReference{
            .mBroker = Broker::InteractiveBrokers,
            .mFilename = SourceFilename::fromPath("ibkr.csv"),
            .mSourceRow = 7,
            .mTransactionId = "synthetic-id",
            .mInputSequence = {0, 5},
        }},
        .mEventKinds = {StatementEventKind::Interest},
    }};

    const auto json = serialize(ApplicationResultBuilder::build(std::move(input)));

    EXPECT_EQ(json.at("status"), "completed_with_warnings");
    EXPECT_EQ(json.at("diagnostics").at(0).at("blocking"), false);
    EXPECT_EQ(json.at("diagnostics").at(0).at("affectedReports"),
              (nlohmann::json{"other_interest"}));
    EXPECT_EQ(json.at("diagnostics").at(0).at("sources").at(0).at("broker"), "interactive_brokers");
    EXPECT_EQ(json.at("diagnostics").at(0).at("sources").at(0).at("transactionId"), "synthetic-id");
}

TEST(ApplicationResultJsonTest, AFilenameThatFailsTheSanitizerIsNotWritten) {
    // The serializer checks every filename again, so a source that was filled in by hand with an
    // empty name does not reach the JSON.
    ApplicationResult result;
    result.mDiagnostics = {ApplicationDiagnostic{
        .mId = "diagnostic-0001",
        .mSeverity = DiagnosticSeverity::Error,
        .mStage = PipelineStage::Parsing,
        .mCode = DiagnosticCode::ParseError,
        .mMessage = "Synthetic parser message.",
        .mAffectedReports = {kCapital},
        .mBlocking = true,
        .mSources = {ApplicationSource{
            .mSourceIndex = 0,
            .mBroker = Broker::TradeRepublic,
            .mFilename = SourceFilename{},
            .mRow = 3,
        }},
    }};

    const auto json = serialize(result);

    const auto& source = json.at("diagnostics").at(0).at("sources").at(0);

    EXPECT_FALSE(source.contains("file"));
    EXPECT_EQ(source.at("sourceId"), "source-0001");
    EXPECT_EQ(source.at("row"), 3);
}

TEST(ApplicationResultJsonTest, ParserJsonVersionOneHasNoRowKindOrSourceId) {
    ParseResult parseResult{.mBroker = Broker::TradeRepublic};
    parseResult.mDiagnostics = {ParseDiagnostic{
        .mSeverity = DiagnosticSeverity::Error,
        .mCode = DiagnosticCode::MissingField,
        .mSourceFile = "export.csv",
        .mRowIndex = 42,
        .mField = "amount",
        .mMessage = "Synthetic parser message.",
        .mRowKind = ParsedRowKind::Trade,
    }};
    const auto before = api::serializeDiagnosticsJson(parseResult);

    ApplicationResultInput input{.mReportingYear = 2024};
    input.mMergeDiagnostics = {SourcedParseDiagnostic{
        .mSourceIndex = 0,
        .mBroker = Broker::TradeRepublic,
        .mDiagnostic = parseResult.mDiagnostics[0],
    }};
    const auto result = ApplicationResultBuilder::build(std::move(input));
    const auto after = api::serializeDiagnosticsJson(parseResult);

    EXPECT_EQ(before, after);
    EXPECT_EQ(nlohmann::json::parse(after).at("schemaVersion"), 1);
    EXPECT_EQ(after.find("rowKind"), std::string::npos);
    EXPECT_EQ(after.find("sourceId"), std::string::npos);
    EXPECT_EQ(result.mDiagnostics.size(), 1U);
}

TEST(ApplicationResultJsonTest, DiagnosticCodeNameOfAParserCodeIsItsJsonCode) {
    const ApplicationDiagnosticCode code = DiagnosticCode::InvalidValue;

    EXPECT_EQ(api::diagnosticCodeName(code), "invalid_value");
}

TEST(ApplicationResultJsonTest, DiagnosticCodeNameOfAMergeCodeIsItsJsonCode) {
    const ApplicationDiagnosticCode code = MergeDiagnosticCode::ConflictingDuplicate;

    EXPECT_EQ(api::diagnosticCodeName(code), "conflicting_duplicate");
}

TEST(ApplicationResultJsonTest, DiagnosticCodeNameOfAProcessingCodeIsItsJsonCode) {
    const ApplicationDiagnosticCode code = ProcessingDiagnosticCode::LossEligibilityUnresolved;

    EXPECT_EQ(api::diagnosticCodeName(code), "loss_eligibility_unresolved");
}

TEST(ApplicationResultJsonTest, TheCodesOfAppliedCorporateActionsHaveTheirDocumentedNames) {
    const ApplicationDiagnosticCode missing =
        ProcessingDiagnosticCode::CorporateActionMissingPosition;
    const ApplicationDiagnosticCode rounded =
        ProcessingDiagnosticCode::CorporateActionQuantityRounded;
    const ApplicationDiagnosticCode applied = ProcessingDiagnosticCode::CorporateActionApplied;
    const ApplicationDiagnosticCode unsupported =
        ProcessingDiagnosticCode::UnsupportedCorporateAction;

    EXPECT_EQ(api::diagnosticCodeName(missing), "corporate_action_missing_position");
    EXPECT_EQ(api::diagnosticCodeName(rounded), "corporate_action_quantity_rounded");
    EXPECT_EQ(api::diagnosticCodeName(applied), "corporate_action_applied");
    EXPECT_EQ(api::diagnosticCodeName(unsupported), "unsupported_corporate_action");
}

TEST(ApplicationResultJsonTest, ARecordedAnswerIsListedWithoutSources) {
    auto result = ApplicationResultBuilder::build(ApplicationResultInput{.mReportingYear = 2024});
    result.mDecisions = {RecordedDecision{
        .mId = "decision-0001",
        .mType = RequestType::CapitalGainsFailureResolutionRequired,
        .mData =
            FailureResolutionQuestion{
                .mInvestments = {FailedInvestment{
                    .mIsin = "XX0000000001",
                    .mName = "Synthetic Share",
                    .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
                }},
            },
        .mAnswer = FailureResolutionAnswer{FailureResolution::FailReport},
        .mFromEarlierRun = true,
        .mUsed = true,
    }};

    const auto json = nlohmann::ordered_json::parse(api::serializeApplicationResultJson(result));

    // The sources of a recorded answer belong to the run in which it was given, so the decision
    // has only its type, question data and answer.
    const auto expected = nlohmann::ordered_json::parse(R"([{
      "id": "decision-0001",
      "type": "capital_gains_failure_resolution_required",
      "fromEarlierRun": true,
      "used": true,
      "investments": [
        {"name": "Synthetic Share", "isin": "XX0000000001", "reasonCodes": ["incomplete_history"]}
      ],
      "answer": "fail_report"
    }])");

    EXPECT_EQ(json.at("decisions"), expected);
}

TEST(ApplicationResultJsonTest, WritesEveryAnswer) {
    auto result = ApplicationResultBuilder::build(ApplicationResultInput{.mReportingYear = 2024});
    result.mDecisions = {
        RecordedDecision{
            .mId = "decision-0001",
            .mType = RequestType::OtherInterestFailureResolutionRequired,
            .mData =
                FailureResolutionQuestion{
                    .mInvestments = {FailedInvestment{
                        .mIsin = "XX0000000002",
                        .mName = "Synthetic Bond",
                        .mReasonCodes = {ProcessingDiagnosticCode::UnrepresentableValue},
                    }}},
            .mAnswer = FailureResolutionAnswer{FailureResolution::ExcludeFailedInstruments},
        },
        RecordedDecision{
            .mId = "decision-0002",
            .mType = RequestType::CapitalGainsFailureResolutionRequired,
            .mData =
                FailureResolutionQuestion{
                    .mInvestments = {FailedInvestment{
                        .mIsin = "XX0000000001",
                        .mName = "Synthetic Share",
                        .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory},
                    }}},
            .mAnswer = CancelAnswer{},
        },
        // The session never records an answer to a question without question data; the writer
        // still writes such a decision without investments.
        RecordedDecision{
            .mId = "decision-0003",
            .mType = RequestType::TaxClassificationRequired,
            .mAnswer = CancelAnswer{},
        },
        RecordedDecision{
            .mId = "decision-0004",
            .mType = RequestType::LossEligibilityConfirmationRequired,
            .mData =
                LossEligibilityQuestion{
                    .mIsin = "XX0000000001",
                    .mName = "Synthetic Share",
                    .mSales = {LossSaleFacts{
                        .mSaleDate = day("2024-03-10"),
                        .mSaleUnits = 1'000'000'000, // 10 units
                        .mLossUnits = 1'000'000'000,
                        .mWindowStart = day("2024-02-09"),
                        .mWindowEnd = day("2024-04-09"),
                        .mOpenUnits = 1'000'000'000,
                    }},
                },
            .mAnswer = NoOtherActivityAnswer{},
        },
        RecordedDecision{
            .mId = "decision-0005",
            .mType = RequestType::LossEligibilityConfirmationRequired,
            .mData =
                LossEligibilityQuestion{
                    .mIsin = "XX0000000003",
                    .mName = "Synthetic Fund",
                    .mSales = {LossSaleFacts{
                        .mSaleDate = day("2024-05-10"),
                        .mSaleUnits = 200'000'000, // 2 units
                        .mLossUnits = 200'000'000,
                        .mWindowStart = day("2024-04-10"),
                        .mWindowEnd = day("2024-06-09"),
                        .mOpenUnits = 200'000'000,
                    }},
                },
            .mAnswer = OtherActivityAnswer{},
        },
    };

    const auto json = nlohmann::json::parse(api::serializeApplicationResultJson(result));
    const auto& decisions = json.at("decisions");

    ASSERT_EQ(decisions.size(), 5U);

    EXPECT_EQ(decisions[0].at("answer"), "exclude_failed_instruments");
    EXPECT_EQ(decisions[0].at("investments").at(0).at("isin"), "XX0000000002");
    EXPECT_EQ(decisions[0].at("fromEarlierRun"), false);
    EXPECT_EQ(decisions[0].at("used"), false);

    EXPECT_EQ(decisions[1].at("answer"), "cancel");
    EXPECT_EQ(decisions[1].at("investments").at(0).at("isin"), "XX0000000001");

    EXPECT_EQ(decisions[2].at("type"), "tax_classification_required");
    EXPECT_FALSE(decisions[2].contains("investments"));
    EXPECT_FALSE(decisions[2].contains("lossCheck"));

    EXPECT_EQ(decisions[3].at("answer"), "no_other_activity");
    EXPECT_EQ(decisions[3].at("lossCheck").at("isin"), "XX0000000001");
    EXPECT_FALSE(decisions[3].contains("investments"));

    EXPECT_EQ(decisions[4].at("answer"), "other_activity");
    EXPECT_EQ(decisions[4].at("lossCheck").at("isin"), "XX0000000003");
}

TEST(ApplicationResultJsonTest, RowsOnTwoDatesAnsweredAsTwoSplitsAreListedWithTheirDates) {
    auto result = ApplicationResultBuilder::build(ApplicationResultInput{.mReportingYear = 2024});
    result.mDecisions = {
        RecordedDecision{
            .mId = "decision-0001",
            .mType = RequestType::CorporateActionRatioRequired,
            .mData =
                CorporateActionRatioQuestion{
                    .mIsin = "XX0000000001",
                    .mName = "Synthetic Share",
                    .mEffectiveDate = day("2024-06-01"),
                    .mSources = {{.mBroker = Broker::TradeRepublic, .mDate = day("2024-06-01")},
                                 {.mBroker = Broker::InteractiveBrokers,
                                  .mDate = day("2024-06-05")}},
                    .mPositionBefore = 300'000'000, // 3 units
                    .mDateRanges = {{.mEarliestDate = day("2024-01-11"),
                                     .mLatestDate = day("2024-12-31"),
                                     .mPositionBefore = 300'000'000}},
                    .mAskSameEvent = true,
                },
            .mAnswer = SeparateEventsAnswer{},
        },
    };

    const auto json = nlohmann::json::parse(api::serializeApplicationResultJson(result));
    const auto& decision = json.at("decisions").at(0);

    EXPECT_EQ(decision.at("answer"), "separate_events");
    EXPECT_EQ(decision.at("ratioCheck").at("askSameEvent"), true);
    EXPECT_EQ(decision.at("ratioCheck").at("sources").at(0).at("date"), "2024-06-01");
    EXPECT_EQ(decision.at("ratioCheck").at("sources").at(1).at("date"), "2024-06-05");
}

TEST(ApplicationResultJsonTest, ALossRequestHasItsOptionsAndItsSales) {
    // Two sales of one ISIN whose loss the files cannot decide. The first has a transaction ID
    // and two replacements (2 + 3 = 5 of its 10 loss units); the second has neither.
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mRequests = {RequestInput{
        .mQuestion =
            {
                .mType = RequestType::LossEligibilityConfirmationRequired,
                .mAffectedReports = {kCapital},
                .mTaxDate = day("2024-03-10"),
                .mIsin = "XX0000000001",
                .mData =
                    LossEligibilityQuestion{
                        .mIsin = "XX0000000001",
                        .mName = "Synthetic Share",
                        .mSales =
                            {
                                LossSaleFacts{
                                    .mSaleDate = day("2024-05-10"),
                                    .mSaleUnits = 150'000'000, // 1.5 units
                                    .mLossUnits = 150'000'000,
                                    .mWindowStart = day("2024-04-10"),
                                    .mWindowEnd = day("2024-06-09"),
                                    .mOpenUnits = 150'000'000,
                                },
                                LossSaleFacts{
                                    .mSaleDate = day("2024-03-10"),
                                    .mSaleTransaction =
                                        TransactionIdentity{
                                            .mBroker = Broker::TradeRepublic,
                                            .mTransactionId = "synthetic-sell-1",
                                        },
                                    .mSaleUnits = 1'000'000'000, // 10 units
                                    .mLossUnits = 1'000'000'000,
                                    .mWindowStart = day("2024-02-09"),
                                    .mWindowEnd = day("2024-04-09"),
                                    // Listed out of date order; the result sorts them.
                                    .mKnownReplacements =
                                        {
                                            {.mDate = day("2024-04-09"),
                                             .mUnits = 300'000'000}, // 3 units
                                            {.mDate = day("2024-02-09"),
                                             .mUnits = 200'000'000}, // 2 units
                                        },
                                    .mKnownIneligibleUnits = 500'000'000, // 5 units
                                    .mOpenUnits = 500'000'000,
                                },
                            },
                    },
            },
    }};

    const auto json = nlohmann::ordered_json::parse(
        api::serializeApplicationResultJson(ApplicationResultBuilder::build(std::move(input))));
    const auto& request = json.at("requests").at(0);

    // The sales in date order, units with eight decimals.
    const auto expected = nlohmann::ordered_json::parse(R"({
      "name": "Synthetic Share",
      "isin": "XX0000000001",
      "sales": [
        {
          "saleDate": "2024-03-10",
          "transaction": {"broker": "trade_republic", "transactionId": "synthetic-sell-1"},
          "saleUnits": "10.00000000",
          "lossUnits": "10.00000000",
          "windowStart": "2024-02-09",
          "windowEnd": "2024-04-09",
          "knownReplacements": [
            {"date": "2024-02-09", "units": "2.00000000"},
            {"date": "2024-04-09", "units": "3.00000000"}
          ],
          "knownIneligibleUnits": "5.00000000",
          "openUnits": "5.00000000"
        },
        {
          "saleDate": "2024-05-10",
          "saleUnits": "1.50000000",
          "lossUnits": "1.50000000",
          "windowStart": "2024-04-10",
          "windowEnd": "2024-06-09",
          "knownReplacements": [],
          "knownIneligibleUnits": "0.00000000",
          "openUnits": "1.50000000"
        }
      ]
    })");

    EXPECT_EQ(request.at("type"), "loss_eligibility_confirmation_required");
    EXPECT_EQ(request.at("options"),
              (nlohmann::ordered_json{"no_other_activity", "other_activity"}));
    EXPECT_EQ(request.at("lossCheck"), expected);
    EXPECT_FALSE(request.contains("investments"));
}

TEST(ApplicationResultJsonTest, ARecordedLossAnswerIsListedWithItsQuestion) {
    auto result = ApplicationResultBuilder::build(ApplicationResultInput{.mReportingYear = 2024});
    result.mDecisions = {RecordedDecision{
        .mId = "decision-0001",
        .mType = RequestType::LossEligibilityConfirmationRequired,
        .mData =
            LossEligibilityQuestion{
                .mIsin = "XX0000000001",
                .mName = "Synthetic Share",
                .mSales = {LossSaleFacts{
                    .mSaleDate = day("2024-01-05"),
                    .mSaleUnits = 2'500'000'000, // 25 units
                    .mLossUnits = 2'500'000'000,
                    .mWindowStart = day("2023-12-06"),
                    .mWindowEnd = day("2024-02-04"),
                    .mOpenUnits = 2'500'000'000,
                }},
            },
        .mAnswer = OtherActivityAnswer{},
        .mFromEarlierRun = false,
        .mUsed = true,
    }};

    const auto json = nlohmann::ordered_json::parse(api::serializeApplicationResultJson(result));

    const auto expected = nlohmann::ordered_json::parse(R"([{
      "id": "decision-0001",
      "type": "loss_eligibility_confirmation_required",
      "fromEarlierRun": false,
      "used": true,
      "lossCheck": {
        "name": "Synthetic Share",
        "isin": "XX0000000001",
        "sales": [{
          "saleDate": "2024-01-05",
          "saleUnits": "25.00000000",
          "lossUnits": "25.00000000",
          "windowStart": "2023-12-06",
          "windowEnd": "2024-02-04",
          "knownReplacements": [],
          "knownIneligibleUnits": "0.00000000",
          "openUnits": "25.00000000"
        }]
      },
      "answer": "other_activity"
    }])");

    EXPECT_EQ(json.at("decisions"), expected);
}

TEST(ApplicationResultJsonTest, ARatioRequestListsTheActionItsSourcesAndThePosition) {
    // One action from two brokers; only Trade Republic has a quantity field.
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mRequests = {RequestInput{
        .mQuestion =
            {
                .mType = RequestType::CorporateActionRatioRequired,
                .mAffectedReports = {kCapital},
                .mTaxDate = day("2024-06-01"),
                .mIsin = "XX0000000001",
                .mData =
                    CorporateActionRatioQuestion{
                        .mIsin = "XX0000000001",
                        .mName = "Synthetic Share",
                        .mEffectiveDate = day("2024-06-01"),
                        .mSources = {{.mBroker = Broker::InteractiveBrokers,
                                      .mTransactionId = "synthetic-ibkr-split",
                                      .mDate = day("2024-06-01")},
                                     {.mBroker = Broker::TradeRepublic,
                                      .mTransactionId = "synthetic-tr-split",
                                      .mDate = day("2024-06-01"),
                                      .mSourceValue = "3"}},
                        .mPositionBefore = 300'000'000, // 3 units
                        // A purchase of 1 unit on 2024-06-01: from the next day 4 units. A
                        // sale of 0.5 units on 2024-09-01, the day before the next action, comes
                        // after every date: +1 - 0.5 never goes below 0 after the first range,
                        // -0.5 after the second.
                        .mDateRanges = {{.mEarliestDate = day("2024-01-11"),
                                         .mLatestDate = day("2024-06-01"),
                                         .mPositionBefore = 300'000'000},
                                        {.mEarliestDate = day("2024-06-02"),
                                         .mLatestDate = day("2024-09-01"),
                                         .mPositionBefore = 400'000'000,
                                         .mLowestChangeAfter = -50'000'000}},
                    },
            },
    }};

    const auto json = nlohmann::ordered_json::parse(
        api::serializeApplicationResultJson(ApplicationResultBuilder::build(std::move(input))));
    const auto& request = json.at("requests").at(0);

    // Sources by broker; the positions with eight decimals; the ranges in date order.
    const auto expected = nlohmann::ordered_json::parse(R"({
      "name": "Synthetic Share",
      "isin": "XX0000000001",
      "effectiveDate": "2024-06-01",
      "sources": [
        {"broker": "trade_republic", "date": "2024-06-01", "transactionId": "synthetic-tr-split",
         "sourceValue": "3"},
        {"broker": "interactive_brokers", "date": "2024-06-01",
         "transactionId": "synthetic-ibkr-split"}
      ],
      "positionBefore": "3.00000000",
      "dateRanges": [
        {"earliestDate": "2024-01-11", "latestDate": "2024-06-01", "positionBefore": "3.00000000",
         "lowestChangeAfter": "0.00000000"},
        {"earliestDate": "2024-06-02", "latestDate": "2024-09-01", "positionBefore": "4.00000000",
         "lowestChangeAfter": "-0.50000000"}
      ],
      "askSameEvent": false
    })");

    EXPECT_EQ(request.at("type"), "corporate_action_ratio_required");
    EXPECT_EQ(request.at("ratioCheck"), expected);
    EXPECT_FALSE(request.contains("options"));
    EXPECT_FALSE(request.contains("lossCheck"));
    EXPECT_FALSE(request.contains("investments"));
}

TEST(ApplicationResultJsonTest, ARatioRequestBeforeASaleHasTheSaleDateInsteadOfThePosition) {
    // The sale of 20 units on 2024-06-01 needs the split before it; the row is dated 2024-06-03.
    ApplicationResultInput input{.mReportingYear = 2024};
    input.mRequests = {RequestInput{
        .mQuestion =
            {
                .mType = RequestType::CorporateActionRatioRequired,
                .mAffectedReports = {kCapital},
                .mTaxDate = day("2024-06-03"),
                .mIsin = "XX0000000001",
                .mData =
                    CorporateActionRatioQuestion{
                        .mIsin = "XX0000000001",
                        .mName = "Synthetic Share",
                        .mEffectiveDate = day("2024-06-03"),
                        .mSources = {{.mBroker = Broker::TradeRepublic,
                                      .mTransactionId = "synthetic-tr-split",
                                      .mDate = day("2024-06-03"),
                                      .mSourceValue = "10"}},
                        .mEarlierSaleDate = day("2024-06-01"),
                        .mDateRanges = {{.mEarliestDate = day("2024-01-11"),
                                         .mLatestDate = day("2024-06-01"),
                                         .mPositionBefore = 1'000'000'000,       // 10 units
                                         .mLowestChangeAfter = -2'000'000'000}}, // the sale of 20
                    },
            },
    }};

    const auto json = nlohmann::ordered_json::parse(
        api::serializeApplicationResultJson(ApplicationResultBuilder::build(std::move(input))));

    const auto expected = nlohmann::ordered_json::parse(R"({
      "name": "Synthetic Share",
      "isin": "XX0000000001",
      "effectiveDate": "2024-06-03",
      "sources": [
        {"broker": "trade_republic", "date": "2024-06-03", "transactionId": "synthetic-tr-split",
         "sourceValue": "10"}
      ],
      "earlierSaleDate": "2024-06-01",
      "dateRanges": [
        {"earliestDate": "2024-01-11", "latestDate": "2024-06-01", "positionBefore": "10.00000000",
         "lowestChangeAfter": "-20.00000000"}
      ],
      "askSameEvent": false
    })");

    EXPECT_EQ(json.at("requests").at(0).at("ratioCheck"), expected);
}

TEST(ApplicationResultJsonTest, ARecordedRatioIsListedWithItsQuestion) {
    auto result = ApplicationResultBuilder::build(ApplicationResultInput{.mReportingYear = 2024});
    result.mDecisions = {
        RecordedDecision{
            .mId = "decision-0001",
            .mType = RequestType::CorporateActionRatioRequired,
            .mData =
                CorporateActionRatioQuestion{
                    .mIsin = "XX0000000001",
                    .mName = "Synthetic Share",
                    .mEffectiveDate = day("2024-06-01"),
                    .mSources = {{.mBroker = Broker::TradeRepublic, .mDate = day("2024-06-01")}},
                    .mPositionBefore = 52'000'000, // 0.52 units
                    .mDateRanges = {{.mEarliestDate = day("2024-03-02"),
                                     .mLatestDate = day("2024-06-10"),
                                     .mPositionBefore = 52'000'000}},
                },
            // The announcement gives an earlier date than the broker's row.
            .mAnswer = SplitRatioAnswer{.mNewShares = 10,
                                        .mOldShares = 1,
                                        .mEffectiveDate = day("2024-05-28"),
                                        .mConfirmedUnits = 520'000'000},
            .mFromEarlierRun = true,
            .mUsed = true,
        },
        RecordedDecision{
            .mId = "decision-0002",
            .mType = RequestType::CorporateActionRatioRequired,
            .mData =
                CorporateActionRatioQuestion{
                    .mIsin = "XX0000000002",
                    .mName = "Synthetic Fund",
                    .mEffectiveDate = day("2024-09-01"),
                    .mSources = {{.mBroker = Broker::TradeRepublic,
                                  .mTransactionId = "synthetic-split-2",
                                  .mDate = day("2024-09-01"),
                                  .mSourceValue = "-1.5"}},
                    .mPositionBefore = 300'000'000, // 3 units
                    .mDateRanges = {{.mEarliestDate = day("2024-02-01"),
                                     .mLatestDate = day("2024-12-31"),
                                     .mPositionBefore = 300'000'000}},
                },
            .mAnswer = CancelAnswer{},
            .mFromEarlierRun = false,
            .mUsed = false,
        },
    };

    const auto json = nlohmann::ordered_json::parse(api::serializeApplicationResultJson(result));

    // A source without a transaction ID or a quantity field has only its broker and date.
    const auto expected = nlohmann::ordered_json::parse(R"([{
      "id": "decision-0001",
      "type": "corporate_action_ratio_required",
      "fromEarlierRun": true,
      "used": true,
      "ratioCheck": {
        "name": "Synthetic Share",
        "isin": "XX0000000001",
        "effectiveDate": "2024-06-01",
        "sources": [{"broker": "trade_republic", "date": "2024-06-01"}],
        "positionBefore": "0.52000000",
        "dateRanges": [
          {"earliestDate": "2024-03-02", "latestDate": "2024-06-10", "positionBefore": "0.52000000",
           "lowestChangeAfter": "0.00000000"}
        ],
        "askSameEvent": false
      },
      "answer": {
        "newShares": 10,
        "oldShares": 1,
        "effectiveDate": "2024-05-28",
        "confirmedUnits": "5.20000000"
      }
    }, {
      "id": "decision-0002",
      "type": "corporate_action_ratio_required",
      "fromEarlierRun": false,
      "used": false,
      "ratioCheck": {
        "name": "Synthetic Fund",
        "isin": "XX0000000002",
        "effectiveDate": "2024-09-01",
        "sources": [
          {"broker": "trade_republic", "date": "2024-09-01", "transactionId": "synthetic-split-2",
           "sourceValue": "-1.5"}
        ],
        "positionBefore": "3.00000000",
        "dateRanges": [
          {"earliestDate": "2024-02-01", "latestDate": "2024-12-31", "positionBefore": "3.00000000",
           "lowestChangeAfter": "0.00000000"}
        ],
        "askSameEvent": false
      },
      "answer": "cancel"
    }])");

    EXPECT_EQ(json.at("decisions"), expected);
}

TEST(ApplicationResultJsonTest, ARequestWithoutQuestionDataHasNoOptions) {
    ApplicationResultInput input{.mReportingYear = 2024, .mRunRevision = 3};
    input.mRequests = {RequestInput{.mQuestion = {
                                        .mType = RequestType::TaxClassificationRequired,
                                        .mAffectedReports = {kCapital},
                                    }}};

    const auto json = nlohmann::json::parse(
        api::serializeApplicationResultJson(ApplicationResultBuilder::build(std::move(input))));
    const auto& request = json.at("requests").at(0);

    EXPECT_EQ(json.at("runRevision"), 3);
    EXPECT_EQ(request.at("runRevision"), 3);
    EXPECT_FALSE(request.contains("options"));
    EXPECT_FALSE(request.contains("investments"));
}

} // namespace
