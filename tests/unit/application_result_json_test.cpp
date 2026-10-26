#include "taxbroker/api/application_result_json.hpp"
#include "taxbroker/api/diagnostics_json.hpp"
#include "taxbroker/application_result.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace {

using namespace taxbroker;

constexpr auto kCapital = ReportType::CapitalGains;

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
        .mType = RequestType::CapitalGainsFailureResolutionRequired,
        .mAffectedReports = {kCapital},
        .mSources = {SourceReference{
            .mBroker = Broker::TradeRepublic,
            .mFilename = SourceFilename::fromPath("TransactionExport.csv"),
            .mSourceRow = 42,
            .mInputSequence = {1, 40},
        }},
        .mProcessingDiagnostics = {0},
    }};
    input.mReports[1].mXml = "<Envelope>dividends</Envelope>";

    const auto text =
        api::serializeApplicationResultJson(ApplicationResultBuilder::build(std::move(input)));

    // ordered_json keeps the key order of the literal, which is the order of the doc example.
    const auto expected = nlohmann::ordered_json::parse(R"({
      "schemaVersion": 2,
      "status": "needs_input",
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
          "affectedReports": ["capital_gains"],
          "sources": [
            {
              "broker": "trade_republic",
              "sourceId": "source-0002",
              "file": "TransactionExport.csv",
              "row": 42
            }
          ]
        }
      ]
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
            .mType = RequestType::OtherInterestFailureResolutionRequired,
            .mAffectedReports = {kCapital},
            .mTaxDate =
                Date{std::chrono::sys_days{std::chrono::year{2024} / 11 / 30}.time_since_epoch()},
            .mIsin = "XX0000000001",
        },
        RequestInput{
            .mType = RequestType::CapitalGainsFailureResolutionRequired,
            .mAffectedReports = {kCapital},
            .mTaxDate =
                Date{std::chrono::sys_days{std::chrono::year{2024} / 11 / 30}.time_since_epoch()},
            .mIsin = "XX0000000001",
        },
        RequestInput{
            .mType = RequestType::IncomeDetailsRequired,
            .mAffectedReports = {kCapital},
            .mTaxDate =
                Date{std::chrono::sys_days{std::chrono::year{2024} / 11 / 30}.time_since_epoch()},
            .mIsin = "XX0000000001",
        },
        RequestInput{
            .mType = RequestType::TaxClassificationRequired,
            .mAffectedReports = {kCapital},
            .mTaxDate =
                Date{std::chrono::sys_days{std::chrono::year{2024} / 11 / 30}.time_since_epoch()},
            .mIsin = "XX0000000001",
        },
        RequestInput{
            .mType = RequestType::LossEligibilityConfirmationRequired,
            .mAffectedReports = {kCapital},
            .mTaxDate =
                Date{std::chrono::sys_days{std::chrono::year{2024} / 11 / 30}.time_since_epoch()},
            .mIsin = "XX0000000001",
        },
        RequestInput{
            .mType = RequestType::QuantityCorrectionRequired,
            .mAffectedReports = {kCapital},
            .mTaxDate =
                Date{std::chrono::sys_days{std::chrono::year{2024} / 11 / 30}.time_since_epoch()},
            .mIsin = "XX0000000001",
        },
        RequestInput{
            .mType = RequestType::CorporateActionRatioRequired,
            .mAffectedReports = {kCapital},
            .mTaxDate =
                Date{std::chrono::sys_days{std::chrono::year{2024} / 11 / 30}.time_since_epoch()},
            .mIsin = "XX0000000001",
        },
        RequestInput{
            .mType = RequestType::CorporateActionIdentityRequired,
            .mAffectedReports = {kCapital},
            .mTaxDate =
                Date{std::chrono::sys_days{std::chrono::year{2024} / 11 / 30}.time_since_epoch()},
            .mIsin = "XX0000000001",
        },
        RequestInput{
            .mType = RequestType::ExchangeRatesRequired,
            .mAffectedReports = {kCapital},
            .mTaxDate =
                Date{std::chrono::sys_days{std::chrono::year{2024} / 11 / 30}.time_since_epoch()},
            .mIsin = "XX0000000001",
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

} // namespace
