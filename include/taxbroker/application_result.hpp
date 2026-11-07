#pragma once

#include "taxbroker/exact_arithmetic.hpp"
#include "taxbroker/processing_diagnostics.hpp"
#include "taxbroker/reference_catalogue.hpp"
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
    DepositInterestTotalRequired,
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
    std::optional<TransactionIdentity> mSaleTransaction; ///< Broker and transaction ID of the sale.
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

/// One source record of a corporate action. Any broker; the question never depends on which.
struct CorporateActionSourceFacts {
    Broker mBroker{Broker::Unknown};
    std::optional<std::string> mTransactionId;
    Date mDate;
    /// The broker's own quantity field as an exact decimal, when the record has one (Trade
    /// Republic `shares`). Its meaning is not verified, so it is shown read-only and is never a
    /// ratio.
    std::optional<std::string> mSourceValue;

    bool operator==(const CorporateActionSourceFacts&) const = default;
};

/// Effective dates that give the same open units just before the action. An action comes before
/// the trades of its day, so the units on a day are those of the trades dated before it, in the
/// units before the action.
struct EffectiveDateRange {
    Date mEarliestDate;
    Date mLatestDate;
    /// Open units across all brokers at the start of every date of the range, always above 0.
    Units mPositionBefore{};
    /// The lowest running total of the trades after the action (signed units, in ledger order),
    /// up to 31 December, or up to 11 days before the next action's row (a trade later than that
    /// may come after the next action, whose question checks it).
    /// A trade date later than that is a range of its own with the trades of that day only: they
    /// are always before the next action.
    /// The units after the action plus this must not be below 0, or a later sale lacks units.
    Units mLowestChangeAfter{};

    bool operator==(const EffectiveDateRange&) const = default;
};

/// The question data: one corporate action without a verified ratio, with every source record of
/// it and the units just before it for every date the user may give. A new source, another
/// position or a moved trade is another question.
struct CorporateActionRatioQuestion {
    Isin mIsin;
    std::string mName;
    /// The date of the broker's row, shown pre-filled; the announcement may give another one.
    Date mEffectiveDate;
    std::vector<CorporateActionSourceFacts> mSources;
    /// Open units across all brokers at the start of mEffectiveDate. Absent when a sale before
    /// that date needs more units than were held (mEarlierSaleDate): the action must then take
    /// effect on or before that sale.
    std::optional<Units> mPositionBefore;
    /// The sale before mEffectiveDate that needs more units than were held, when the row may be
    /// a later booking of an action that took effect before it.
    std::optional<Date> mEarlierSaleDate;
    /// Every date the action may have, in date order; one of them holds mEffectiveDate unless
    /// mEarlierSaleDate is set, when the last one ends on or before that sale's date. They are
    /// after the previous action of the ISIN, before its next one and up to 31 December of the
    /// selected year, or up to the end of the loss-analysis view (30 January) for an action with
    /// a row after 31 December. A date with no units before the action, or after a sale of more
    /// units than were held, is in no range.
    std::vector<EffectiveDateRange> mDateRanges;
    /// Rows of different brokers on different dates: a ratio answer also says that they are one
    /// event, and SeparateEventsAnswer says they are not.
    bool mAskSameEvent{};

    bool operator==(const CorporateActionRatioQuestion&) const = default;
};

/// The range that holds aDate, or nullptr.
[[nodiscard]] const EffectiveDateRange*
findEffectiveDateRange(const CorporateActionRatioQuestion& aQuestion, Date aDate);

/// The question data of `tax_classification_required` for a trade investment whose asset class is
/// missing, not recognized or different between rows.
struct TaxClassificationQuestion {
    Isin mIsin;
    std::string mName;
    /// The class label of each row as the file gave it.
    std::vector<std::string> mClassLabels;

    bool operator==(const TaxClassificationQuestion&) const = default;
};

/// The question data of `quantity_correction_required`: one sale that needs a little more units
/// than the earlier purchases left (less than 0.00000100), without proof that it is rounding.
struct QuantityCorrectionQuestion {
    Isin mIsin;
    std::string mName;
    Date mSaleDate;
    std::optional<TransactionIdentity> mSaleTransaction; ///< Broker and transaction ID of the sale.
    Units mOriginalUnits{};
    /// The units the earlier purchases left, always above 0 and below mOriginalUnits: the
    /// proposed quantity.
    Units mOpenUnits{};
    /// EUR per unit that values the removed units: the higher of the sale price and the highest
    /// exact value per unit of the open lots.
    ExactRational mUnitValue;
    /// The EUR impact that other roundings of the ISIN have used before this sale.
    ExactRational mBudgetUsed;

    bool operator==(const QuantityCorrectionQuestion&) const = default;
};

/// One reference row of a payer, as a payer choice question offers it.
struct PayerChoiceOption {
    std::string mName;                     ///< The legal name.
    std::string mAddress;                  ///< One line.
    std::string mCountry;                  ///< Residence country, 2 uppercase letters.
    std::optional<std::string> mTaxNumber; ///< None when the row has none.
    CatalogueSource mSource;               ///< The row it came from.

    bool operator==(const PayerChoiceOption&) const = default;
};

/// The question data of `income_details_required` for a dividend payer whose reference rows
/// disagree. The user picks one row or skips. It never blocks: a skip, or no answer, gives a payer
/// without facts, like a payer that is not found.
struct PayerChoiceQuestion {
    Isin mIsin;
    std::string mName; ///< The name of the dividend payment's instrument.
    /// The disagreeing rows, sorted by name, address, country, tax number and source row.
    std::vector<PayerChoiceOption> mOptions;

    bool operator==(const PayerChoiceQuestion&) const = default;
};

/// The yearly bank-deposit interest allowance in EUR, across all qualifying banks. At or below it a
/// return is not needed; above it all reportable amounts go in Doh-DHO.
inline constexpr Money kDepositInterestAllowance = 1000 * MONEY_SCALE;

/// The question data of `deposit_interest_total_required`: the imported qualifying interest of
/// the year is above 0 and at most EUR 1,000, so only the interest of the user's other banks can
/// make it filing. The user answers whether the total with that interest is above EUR 1,000.
struct DepositInterestTotalQuestion {
    int mYear{};    ///< The reporting year.
    Money mTotal{}; ///< The imported qualifying interest in EUR, exact at 4 decimals.

    bool operator==(const DepositInterestTotalQuestion&) const = default;
};

/// The facts an answer depends on. std::monostate for a type whose processor does not define its
/// question data yet.
using QuestionData = std::variant<std::monostate,
                                  FailureResolutionQuestion,
                                  LossEligibilityQuestion,
                                  CorporateActionRatioQuestion,
                                  TaxClassificationQuestion,
                                  QuantityCorrectionQuestion,
                                  PayerChoiceQuestion,
                                  DepositInterestTotalQuestion>;

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
/// the loss sales by date and transaction with their replacements by date and units, the
/// sources of a corporate action by broker and transaction ID, and the class labels of a
/// classification question, so the order in which a stage lists them never changes the question.
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

/// The ratio from the announcement, new shares per old shares, its effective date, and the
/// position after the action that the user saw in the preview and confirmed. The session keeps the
/// reduced numbers. For a question with mAskSameEvent it also says that the rows are one event.
struct SplitRatioAnswer {
    std::int64_t mNewShares{};
    std::int64_t mOldShares{};
    /// From the announcement; it must be in one of the question's mDateRanges.
    Date mEffectiveDate;
    Units mConfirmedUnits{};

    bool operator==(const SplitRatioAnswer&) const = default;
};

/// `no` to the question whether the rows of a ratio question with mAskSameEvent are one event.
struct SeparateEventsAnswer {
    bool operator==(const SeparateEventsAnswer&) const = default;
};

/// `capital_gains` to a classification question: treat the investment as shares or fund units
/// (Doh-KDVP).
struct CapitalGainsAnswer {
    bool operator==(const CapitalGainsAnswer&) const = default;
};

/// `exclude_from_supported_reports` to a classification question: leave the investment out of the
/// reports and list it in the excluded items.
struct ExcludeFromReportsAnswer {
    bool operator==(const ExcludeFromReportsAnswer&) const = default;
};

/// The sale units to use instead of the original ones, at most the open units of the question.
struct QuantityCorrectionAnswer {
    Units mCorrectedUnits{};

    bool operator==(const QuantityCorrectionAnswer&) const = default;
};

/// The answer to a payer choice question: the position of the picked row in the question's
/// options, or no position for a skip.
struct PayerChoiceAnswer {
    std::optional<std::size_t> mPickedOption;

    bool operator==(const PayerChoiceAnswer&) const = default;
};

/// The answer to a deposit interest total question: whether the year's total, with the interest
/// of the user's other Slovenian and EU banks, is above EUR 1,000.
struct DepositInterestTotalAnswer {
    bool mAboveLimit{}; ///< True: make Doh-DHO from the imported interest.

    bool operator==(const DepositInterestTotalAnswer&) const = default;
};

using AnswerValue = std::variant<FailureResolutionAnswer,
                                 CancelAnswer,
                                 NoOtherActivityAnswer,
                                 OtherActivityAnswer,
                                 SplitRatioAnswer,
                                 SeparateEventsAnswer,
                                 CapitalGainsAnswer,
                                 ExcludeFromReportsAnswer,
                                 QuantityCorrectionAnswer,
                                 PayerChoiceAnswer,
                                 DepositInterestTotalAnswer>;

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

/// The category an excluded item belongs to.
enum class ExcludedCategory {
    Bond,     ///< An ordinary debt security; only its interest is taxable.
    Crypto,   ///< A crypto asset; not part of the supported reports.
    Dividend, ///< The dividends of a Slovenian payer; the company deducts the tax itself.
    Interest, ///< The coupons of a Slovenian issuer; the payer deducts the tax itself.
    Unknown,  ///< The class was missing, not recognized or conflicting, and the user left it out.
};

/// Why an item is left out.
enum class ExcludedReason {
    DebtSecurity,      ///< Debt-security sales and redemptions are no capital gain.
    CryptoNotReported, ///< Crypto is not part of Doh-KDVP.
    UserChoice,        ///< The user answered `exclude_from_supported_reports`.
    /// A Slovenian payer deducts the final tax itself, so the payments are not reported.
    SlovenianTaxSettled,
};

/// Units of an excluded bond in the selected year. Matured units are added up by sum, never by
/// order: a cancelled maturity brings units back.
struct BondExclusionFacts {
    Units mBoughtUnits{};
    Units mSoldUnits{};
    Units mMaturedUnits{};             ///< Units out minus units back.
    std::optional<Date> mMaturityDate; ///< The date of the first maturity row.

    bool operator==(const BondExclusionFacts&) const = default;
};

/// An instrument left out of the supported reports, by a tax rule or by the user. It never
/// masquerades as a processed event, and it is not an entry of a report's `excludedInvestments`.
struct ExcludedItem {
    Isin mIsin;
    std::string mName;
    ExcludedCategory mCategory{ExcludedCategory::Unknown};
    ExcludedReason mReason{ExcludedReason::UserChoice};
    /// The class labels of the rows as the files gave them, sorted; "" for a blank class.
    std::vector<std::string> mClassLabels;
    std::optional<BondExclusionFacts> mBond; ///< Only for a bond.
    std::vector<ApplicationSource> mSources;
    /// The diagnostics that explain the exclusion, in diagnostic order.
    std::vector<std::string> mDiagnosticIds;

    [[nodiscard]] bool isTaxRule() const {
        return mReason != ExcludedReason::UserChoice;
    }
};

/// An excluded item as a stage hands it over: the sources are source references and the
/// diagnostics are positions in ApplicationResultInput::mProcessingDiagnostics.
struct ExcludedItemInput {
    ExcludedItem mItem;
    std::vector<SourceReference> mSources;
    std::vector<std::size_t> mProcessingDiagnostics;
};

/// What the report stage produced for one form, before the status is derived.
struct ReportInput {
    /// The generated XML document, when the report stage produced one.
    std::optional<std::string> mXml;
    /// The reason when the form has no rows and no XML.
    std::optional<NoDataReason> mNoDataReason;
    /// What the user chose for the failed investments of this form, when a stage asked.
    std::optional<FailureResolution> mFailureResolution;
    /// The investments left out of this form, only with ExcludeFailedInstruments.
    std::vector<FailedInvestment> mExcludedInvestments;
};

struct ReportResult {
    ReportStatus mStatus{ReportStatus::NoData};
    std::vector<std::string> mDiagnosticIds;
    std::vector<std::string> mRequestIds;
    /// Present only for Generated. The builder moves it in from ReportInput; read it by reference.
    std::optional<std::string> mXml;
    /// Present only for NoData, and only when the report stage gave a reason.
    std::optional<NoDataReason> mNoDataReason;
    /// Present only after the user answered the failure question of this form.
    std::optional<FailureResolution> mFailureResolution;
    /// The investments left out of this form, by ISIN and name, each with the IDs of the
    /// diagnostics that the exclusion resolved. Empty unless mFailureResolution is
    /// ExcludeFailedInstruments.
    std::vector<RequestInvestment> mExcludedInvestments;
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
    std::vector<RecordedDecision> mDecisions; ///< Builder leaves mDecisions empty.
    std::vector<ExcludedItem> mExcludedItems; ///< Sorted by ISIN.

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
    /// Positions in mProcessingDiagnostics of the errors that an exclusion resolved. They stay
    /// errors but no longer block.
    std::vector<std::size_t> mResolvedDiagnostics;
    /// Positions in mMergeDiagnostics of the class errors that the classification stage took
    /// over. They stay errors but no longer block; the classification stage gives its own.
    std::vector<std::size_t> mResolvedMergeDiagnostics;
    std::vector<ExcludedItemInput> mExcludedItems;
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
    /// Generated. An open payer choice question is listed with its form but does not make it
    /// `needs_input`. Throws std::invalid_argument when a question has the question data of another
    /// type, a failure-resolution question has no investment, a loss question has no sale or
    /// a sale without open units, a ratio question has no source, no open units or a date
    /// range that does not contain its effective date, or a payer choice question has no ISIN or
    /// fewer than two rows.
    [[nodiscard]] static ApplicationResult build(ApplicationResultInput&& aInput);
};

/// True when a parse or merge error of aInput affects aReport. Such an error belongs to a file or
/// a row, not to one investment, so leaving an investment out cannot resolve it.
[[nodiscard]] bool hasSourceError(const ApplicationResultInput& aInput, ReportType aReport);

/// Appends the diagnostics and requests of one stage. The requests' diagnostic indices count from
/// the stage's first diagnostic and are shifted to the numbering of aInput.
void appendStageOutput(ApplicationResultInput& aInput,
                       std::vector<ProcessingDiagnostic>&& aDiagnostics,
                       std::vector<RequestInput>&& aRequests);

} // namespace taxbroker
