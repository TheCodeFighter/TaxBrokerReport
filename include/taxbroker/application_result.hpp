#pragma once

#include "taxbroker/processing_diagnostics.hpp"
#include "taxbroker/statement_merger.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace taxbroker {

enum class ReportStatus {
    Generated,
    NoData,
    NeedsInput,
    Failed,
};

enum class OverallStatus {
    Success,
    CompletedWithWarnings,
    PartialSuccess,
    NeedsInput,
    Failed,
};

enum class NoDataReason {
    BelowDepositInterestThreshold,
    NoSelectedYearDisposals,
};

/// The documented user-decision types. The core only lists the request; its question data and
/// answers belong to the decision lifecycle (#215).
enum class RequestType {
    ExchangeRatesRequired,
    CorporateActionIdentityRequired,
    CorporateActionRatioRequired,
    QuantityCorrectionRequired,
    LossEligibilityConfirmationRequired,
    TaxClassificationRequired,
    IncomeDetailsRequired,
    CapitalGainsFailureResolutionRequired,
    OtherInterestFailureResolutionRequired,
};

/// The code of the stage that found the problem; the three enums have no value in common.
using ApplicationDiagnosticCode =
    std::variant<DiagnosticCode, MergeDiagnosticCode, ProcessingDiagnosticCode>;

/// A safe location. The source index follows the request order of the files and is exposed only
/// as the run-local source ID (`source-0001` for index 0).
struct ApplicationSource {
    std::size_t mSourceIndex{};
    Broker mBroker{Broker::Unknown};
    /// Absent when the name could not be reduced to a plain filename.
    std::optional<SourceFilename> mFilename;
    /// One-based logical row; the CSV header is row 1.
    std::optional<std::size_t> mRow;
    std::optional<std::string> mTransactionId;
    /// Orders sources inside a file; never exposed.
    std::optional<std::size_t> mEventIndex;

    bool operator==(const ApplicationSource&) const = default;
};

struct ApplicationDiagnostic {
    /// `diagnostic-0001` and so on, assigned after the final sort.
    std::string mId;
    DiagnosticSeverity mSeverity{DiagnosticSeverity::Error};
    PipelineStage mStage{PipelineStage::Parsing};
    ApplicationDiagnosticCode mCode{DiagnosticCode::ParseError};
    /// English sentence for logs, the CLI and developer tools; the frontend uses the key.
    std::string mMessage;
    std::optional<MessageKey> mMessageKey;
    std::vector<MessageParam> mMessageParams;
    /// In the fixed report order. Empty for a row that no report of this application uses.
    std::vector<ReportType> mAffectedReports;
    /// True for an error that affects at least one report. Resolutions come with later issues.
    bool mBlocking{};
    std::vector<ApplicationSource> mSources;
    std::optional<Date> mTaxDate;
    std::optional<std::string> mField;
    std::optional<std::string> mInstrumentName;
    std::optional<Isin> mIsin;
    /// The open request that can resolve this diagnostic.
    std::optional<std::string> mRequestId;
};

/// What a stage hands over about an open question to the user.
struct RequestInput {
    RequestType mType{};
    std::vector<ReportType> mAffectedReports;
    std::vector<SourceReference> mSources;
    std::optional<Date> mTaxDate;
    std::optional<Isin> mIsin;
    /// Positions in ApplicationResultInput::mProcessingDiagnostics that this request resolves for
    /// the reports in mAffectedReports. Every position must exist there; the builder throws
    /// std::out_of_range otherwise.
    std::vector<std::size_t> mProcessingDiagnostics;
};

struct DecisionRequest {
    /// `request-0001` and so on, assigned after the final sort.
    std::string mId;
    RequestType mType{};
    std::vector<ReportType> mAffectedReports;
    std::vector<ApplicationSource> mSources;
    std::optional<Date> mTaxDate;
    std::optional<Isin> mIsin;
};

/// What the report stage produced for one form, before the status is derived.
struct ReportInput {
    /// The generated XML document, when the report stage produced one.
    std::optional<std::string> mXml;
    /// The reason when the form has no rows and no XML.
    std::optional<NoDataReason> mNoDataReason;
};

struct ReportResult {
    ReportStatus mStatus{ReportStatus::NoData};
    std::vector<std::string> mDiagnosticIds;
    std::vector<std::string> mRequestIds;
    /// Present only for Generated. The builder moves it in from ReportInput; read it by reference.
    std::optional<std::string> mXml;
    /// Present only for NoData, and only when the report stage gave a reason.
    std::optional<NoDataReason> mNoDataReason;
    /// Always false until the developer-mode flow exists
    bool mUnsafe{};
};

/// All results of one run, in the fixed report order of ReportType.
struct ApplicationResult {
    OverallStatus mStatus{OverallStatus::Success};
    std::vector<ApplicationDiagnostic> mDiagnostics;
    std::vector<DecisionRequest> mRequests;
    std::array<ReportResult, 4> mReports;

    [[nodiscard]] const ReportResult& report(ReportType aType) const {
        return mReports[static_cast<std::size_t>(aType)];
    }
};

struct ApplicationResultInput {
    /// Selects the form of Trade Republic interest: before 6 December 2023 it is Doh-Obr income.
    int mReportingYear{};
    /// Parser and merger diagnostics, as the merger returns them.
    std::vector<StatementMergeDiagnostic> mMergeDiagnostics;
    std::vector<ProcessingDiagnostic> mProcessingDiagnostics;
    std::vector<RequestInput> mRequests;
    /// Indexed by ReportType.
    std::array<ReportInput, 4> mReports;
};

class ApplicationResultBuilder final {
  public:
    /// Combines the diagnostics of every stage into the application result.
    ///
    /// Each diagnostic gets its report scope, a blocking flag, sorted safe sources, and an ID.
    /// Parser diagnostics keep their code,
    /// severity, key, parameters and message. A report is Failed when a blocking diagnostic
    /// affects it that no open request of that report resolves. XML is kept only for
    /// Generated.
    [[nodiscard]] static ApplicationResult build(ApplicationResultInput&& aInput);
};

} // namespace taxbroker
