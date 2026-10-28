#pragma once

#include "taxbroker/processing_diagnostics.hpp"
#include "taxbroker/statement_merger.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
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

/// The documented user-decision types. Each processor adds the question data and answer of its
/// type; until then a request of that type has no question data and cannot be answered.
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

enum class FailureResolution {
    ExcludeFailedInstruments,
    FailReport,
};

/// One investment of a failure-resolution question.
struct FailedInvestment {
    Isin mIsin;
    std::string mName;
    std::vector<ApplicationDiagnosticCode> mReasonCodes;

    bool operator==(const FailedInvestment&) const = default;
};

/// The question data of `capital_gains_failure_resolution_required` and
/// `other_interest_failure_resolution_required`: the complete list of failed investments.
struct FailureResolutionQuestion {
    std::vector<FailedInvestment> mInvestments;

    bool operator==(const FailureResolutionQuestion&) const = default;
};

/// A purchase that the 30-day loss rule counts against a sale, with the units used for it.
struct KnownReplacement {
    Date mDate;
    Units mUnits{};

    bool operator==(const KnownReplacement&) const = default;
};

/// One sale of the year whose loss units the known replacements do not block in full.
struct LossSaleFacts {
    Date mSaleDate;
    /// Broker and transaction ID of the sale.
    std::optional<TransactionIdentity> mSaleTransaction;
    Units mSaleUnits{};
    Units mLossUnits{};
    Date mWindowStart;
    Date mWindowEnd;
    std::vector<KnownReplacement> mKnownReplacements;
    Units mKnownIneligibleUnits{};
    /// mLossUnits - mKnownIneligibleUnits, always more than 0.
    Units mOpenUnits{};

    bool operator==(const LossSaleFacts&) const = default;
};

/// The question data: one ISIN and every sale of the
/// year whose loss the files cannot decide. One answer covers all of them.
struct LossEligibilityQuestion {
    Isin mIsin;
    std::string mName;
    std::vector<LossSaleFacts> mSales;

    bool operator==(const LossEligibilityQuestion&) const = default;
};

/// The facts an answer depends on. std::monostate for a type whose processor does not define its
/// question data yet.
using QuestionData =
    std::variant<std::monostate, FailureResolutionQuestion, LossEligibilityQuestion>;

/// What a stage asks the user. Only mType and mData identify the question.
struct DecisionQuestion {
    RequestType mType{};
    std::vector<ReportType> mAffectedReports;
    std::vector<SourceReference> mSources;
    std::optional<Date> mTaxDate;
    std::optional<Isin> mIsin;
    QuestionData mData;
};

/// Sorts the investments by ISIN and name and their reason codes in code order without repeats,
/// the loss sales by date and transaction with their replacements by date and units, so the
/// order in which a stage lists them never changes the question.
[[nodiscard]] QuestionData normalizedData(QuestionData aData);

struct FailureResolutionAnswer {
    FailureResolution mChoice{};

    bool operator==(const FailureResolutionAnswer&) const = default;
};

/// The user closed the question without an answer.
struct CancelAnswer {
    bool operator==(const CancelAnswer&) const = default;
};

/// `no` to a loss question: within the window, the investment was not bought or sold anywhere
/// outside the files; open units keep
/// their loss.
struct NoOtherActivityAnswer {
    bool operator==(const NoOtherActivityAnswer&) const = default;
};

/// `yes` to a loss question: it was, so every open unit is treated as blocked by the 30-day rule
/// (the safe side).
struct OtherActivityAnswer {
    bool operator==(const OtherActivityAnswer&) const = default;
};

using AnswerValue =
    std::variant<FailureResolutionAnswer, CancelAnswer, NoOtherActivityAnswer, OtherActivityAnswer>;

/// What a stage hands over about an open question to the user.
struct RequestInput {
    DecisionQuestion mQuestion;
    std::vector<std::size_t> mProcessingDiagnostics;
};

/// A failed investment as the result shows it.
struct RequestInvestment {
    Isin mIsin;
    std::string mName;
    std::vector<ApplicationDiagnosticCode> mReasonCodes;
    /// The linked diagnostics with this ISIN, in diagnostic order.
    std::vector<std::string> mDiagnosticIds;
};

struct DecisionRequest {
    /// `request-0001` and so on, assigned after the final sort.
    std::string mId;
    RequestType mType{};
    std::uint64_t mRunRevision{};
    std::vector<ReportType> mAffectedReports;
    std::vector<ApplicationSource> mSources;
    std::optional<Date> mTaxDate;
    std::optional<Isin> mIsin;
    std::vector<RequestInvestment> mInvestments;
    /// The normalized question data. With mType it is the question an answer is bound to, never
    /// mId.
    QuestionData mData;
};

/// Listed in every result so it is never applied silently. It keeps only what identifies the
/// question: sources and rows are run-local.
struct RecordedDecision {
    /// `decision-0001` and so on, in recording order; valid for this revision only.
    std::string mId;
    RequestType mType{};
    /// The normalized question data.
    QuestionData mData;
    AnswerValue mAnswer;
    bool mFromEarlierRun{};
    /// A stage of this calculation asked this question.
    bool mUsed{};
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
    /// Every request carries the same number.
    std::uint64_t mRunRevision{};
    std::vector<ApplicationDiagnostic> mDiagnostics;
    std::vector<DecisionRequest> mRequests;
    std::array<ReportResult, 4> mReports;
    /// Builder leaves mDecisions empty.
    std::vector<RecordedDecision> mDecisions;

    [[nodiscard]] const ReportResult& report(ReportType aType) const {
        return mReports[static_cast<std::size_t>(aType)];
    }
};

struct ApplicationResultInput {
    /// Selects the form of Trade Republic interest: before 6 December 2023 it is Doh-Obr income.
    int mReportingYear{};
    std::uint64_t mRunRevision{1};
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
    /// Generated. Throws std::invalid_argument when a question has the question data of another
    /// type, a failure-resolution question has no investment, or a loss question has no sale or
    /// a sale without open units.
    [[nodiscard]] static ApplicationResult build(ApplicationResultInput&& aInput);
};

/// Appends the diagnostics and requests of one stage. The requests' diagnostic indices count from
/// the stage's first diagnostic and are shifted to the numbering of aInput.
void appendStageOutput(ApplicationResultInput& aInput,
                       std::vector<ProcessingDiagnostic>&& aDiagnostics,
                       std::vector<RequestInput>&& aRequests);

} // namespace taxbroker
