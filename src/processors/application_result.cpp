#include "taxbroker/application_result.hpp"
#include "utils/numbered_id.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace taxbroker {

namespace {

constexpr std::size_t kReportCount = 4;

/// Trade Republic interest received before 6 December 2023 is Doh-Obr income, so a run for this
/// year or an earlier one can still contain it.
constexpr int kLastYearWithEarlierDepositInterest = 2023;

using ReportList = std::vector<ReportType>;

const ReportList kAllReports{ReportType::CapitalGains,
                             ReportType::Dividends,
                             ReportType::BankDepositInterest,
                             ReportType::OtherInterest};

void normalizeReports(ReportList& aReports) {
    std::sort(aReports.begin(), aReports.end());
    aReports.erase(std::unique(aReports.begin(), aReports.end()), aReports.end());
}

[[nodiscard]] ReportList unionOf(ReportList aLeft, const ReportList& aRight) {
    aLeft.insert(aLeft.end(), aRight.begin(), aRight.end());
    normalizeReports(aLeft);

    return aLeft;
}

/// The forms an interest row can belong to when its subtype is not established. Interactive
/// Brokers cash interest is only ever Doh-Obr; Trade Republic deposit interest is Doh-DHO, and
/// Doh-Obr income before 6 December 2023.
[[nodiscard]] ReportList brokerInterestScope(Broker aBroker, int aReportingYear) {
    switch (aBroker)
    {
    case Broker::InteractiveBrokers:
        return {ReportType::OtherInterest};
    case Broker::TradeRepublic:
        if (aReportingYear > kLastYearWithEarlierDepositInterest)
        {
            return {ReportType::BankDepositInterest};
        }
        // reporting broken interest type, we cannot know if date is also broken
        return {ReportType::BankDepositInterest, ReportType::OtherInterest};
    case Broker::Unknown:
        break;
    }

    return {ReportType::BankDepositInterest, ReportType::OtherInterest};
}

[[nodiscard]] ReportList scopeOfRowKind(ParsedRowKind aKind, Broker aBroker, int aReportingYear) {
    switch (aKind)
    {
    case ParsedRowKind::Trade:
    case ParsedRowKind::CorporateAction:
        return {ReportType::CapitalGains};
    // Private-market events are skipped (no units or price), so a rejected one changes no form.
    case ParsedRowKind::PrivateMarket:
        return {};
    // A bond maturity is information only: bond sales and maturities are in no form yet. The same
    // row of another class has the capital scope (the row kind says so).
    case ParsedRowKind::BondRedemption:
        return {};
    case ParsedRowKind::Dividend:
        return {ReportType::Dividends};
    case ParsedRowKind::BondInterest:
        return {ReportType::OtherInterest};
    case ParsedRowKind::BrokerInterest:
        return brokerInterestScope(aBroker, aReportingYear);
    case ParsedRowKind::UnknownInterest:
        return {ReportType::BankDepositInterest, ReportType::OtherInterest};
    case ParsedRowKind::Benefit:
        return {};
    }

    return kAllReports;
}

[[nodiscard]] ReportList scopeOfParseDiagnostic(const SourcedParseDiagnostic& aDiagnostic,
                                                int aReportingYear) {
    const auto& diagnostic = aDiagnostic.mDiagnostic;

    // The row kind wins: an interest row with an unsupported subtype still belongs to an interest
    // form.
    if (diagnostic.mRowKind)
    {
        return scopeOfRowKind(*diagnostic.mRowKind, aDiagnostic.mBroker, aReportingYear);
    }
    // A recognized row type without an XML form in this application blocks no report.
    if (diagnostic.mCode == DiagnosticCode::UnsupportedRowType)
    {
        return {};
    }

    // A file, a header or a row of unknown meaning may contain any form.
    return kAllReports;
}

/// An interest event can be a bond coupon (Doh-Obr) or deposit interest, and the merger result
/// does not say which, so both interest forms are affected unless every source is Interactive
/// Brokers.
[[nodiscard]] ReportList scopeOfEventKind(StatementEventKind aKind,
                                          const std::vector<SourceReference>& aSources) {
    switch (aKind)
    {
    case StatementEventKind::Trade:
    case StatementEventKind::CorporateAction:
        return {ReportType::CapitalGains};
    // Private-market events are skipped (no units or price), so a rejected one changes no form.
    case StatementEventKind::PrivateMarket:
        return {};
    // A bond maturity is information only: no form uses it yet.
    case StatementEventKind::BondRedemption:
        return {};
    case StatementEventKind::Dividend:
        return {ReportType::Dividends};
    case StatementEventKind::Interest: {
        const bool isIbkrOnly =
            !aSources.empty() &&
            std::all_of(aSources.begin(), aSources.end(), [](const SourceReference& aSource) {
                return aSource.mBroker == Broker::InteractiveBrokers;
            });
        if (isIbkrOnly)
        {
            return {ReportType::OtherInterest};
        }

        return {ReportType::BankDepositInterest, ReportType::OtherInterest};
    }
    case StatementEventKind::Benefit:
        return {};
    }

    return kAllReports;
}

[[nodiscard]] ReportList scopeOfMergeDiagnostic(const MergeDiagnostic& aDiagnostic) {
    switch (aDiagnostic.mCode)
    {
    case MergeDiagnosticCode::DuplicateSourceIndex:
    case MergeDiagnosticCode::InconsistentSourceIndex:
        return kAllReports;
    case MergeDiagnosticCode::InstrumentAssetClassConflict:
        return {ReportType::CapitalGains};
    case MergeDiagnosticCode::ConflictingDuplicate:
    case MergeDiagnosticCode::DuplicateTransactionInFile:
    case MergeDiagnosticCode::InstrumentNameConflict:
        break;
    }

    if (aDiagnostic.mEventKinds.empty())
    {
        return kAllReports;
    }

    ReportList reports;
    for (const auto kind : aDiagnostic.mEventKinds)
    {
        reports = unionOf(std::move(reports), scopeOfEventKind(kind, aDiagnostic.mSources));
    }

    return reports;
}

/// A name that is not a plain filename is left out; the source ID still identifies the file.
[[nodiscard]] std::optional<SourceFilename> safeFilename(const std::string& aPath) {
    try
    { return SourceFilename::fromPath(aPath); } catch (const std::invalid_argument&)
    { return std::nullopt; }
}

[[nodiscard]] ApplicationSource sourceOf(const SourceReference& aReference) {
    return ApplicationSource{
        .mSourceIndex = aReference.mInputSequence.mSourceIndex,
        .mBroker = aReference.mBroker,
        .mFilename = safeFilename(aReference.mFilename.value()),
        .mRow = aReference.mSourceRow,
        .mTransactionId = aReference.mTransactionId,
        .mEventIndex = aReference.mInputSequence.mEventIndex,
    };
}

[[nodiscard]] std::vector<ApplicationSource>
sourcesOf(const std::vector<SourceReference>& aReferences) {
    std::vector<ApplicationSource> sources;

    std::transform(aReferences.begin(),
                   aReferences.end(),
                   std::back_inserter(sources),
                   [](const SourceReference& aReference) { return sourceOf(aReference); });

    return sources;
}

/// Orders the sources of one diagnostic by request index, event index, filename and row, and
/// drops repeats. Broker and transaction ID come last, so equal sources are always neighbour.
void normalizeSources(std::vector<ApplicationSource>& aSources) {
    const auto key = [](const ApplicationSource& aSource) {
        return std::make_tuple(aSource.mSourceIndex,
                               !aSource.mEventIndex.has_value(),
                               aSource.mEventIndex.value_or(0),
                               aSource.mFilename ? aSource.mFilename->value() : std::string{},
                               !aSource.mRow.has_value(),
                               aSource.mRow.value_or(0),
                               static_cast<int>(aSource.mBroker),
                               !aSource.mTransactionId.has_value(),
                               aSource.mTransactionId.value_or(std::string{}));
    };

    std::stable_sort(aSources.begin(),
                     aSources.end(),
                     [&key](const ApplicationSource& aLeft, const ApplicationSource& aRight) {
                         return key(aLeft) < key(aRight);
                     });
    aSources.erase(std::unique(aSources.begin(), aSources.end()), aSources.end());
}

/// A value that sorts after every present value when it is absent.
template <typename T> using NoneLast = std::pair<bool, T>;

template <typename T> [[nodiscard]] NoneLast<T> noneLast(const std::optional<T>& aValue) {
    return {!aValue.has_value(), aValue.value_or(T{})};
}

[[nodiscard]] std::int64_t dayNumber(const Date& aDate) {
    return aDate.time_since_epoch().count();
}

[[nodiscard]] NoneLast<std::int64_t> dateKey(const std::optional<Date>& aDate) {
    return {!aDate.has_value(), aDate ? dayNumber(*aDate) : 0};
}

/// The earliest source decides the position of a diagnostic; sources are already sorted.
[[nodiscard]] NoneLast<std::size_t> earliestFile(const std::vector<ApplicationSource>& aSources) {
    return {aSources.empty(), aSources.empty() ? 0 : aSources.front().mSourceIndex};
}

[[nodiscard]] NoneLast<std::size_t> earliestEvent(const std::vector<ApplicationSource>& aSources) {
    if (aSources.empty())
    {
        return {true, 0};
    }

    return noneLast(aSources.front().mEventIndex);
}

/// The first affected report in the fixed order; a diagnostic without a report sorts last.
[[nodiscard]] std::size_t reportKey(const ReportList& aReports) {
    return aReports.empty() ? kReportCount : static_cast<std::size_t>(aReports.front());
}

/// A diagnostic before its ID is known, with what the sort and the request link need.
struct DraftDiagnostic {
    ApplicationDiagnostic mDiagnostic;
    /// Position in ApplicationResultInput::mProcessingDiagnostics, when it came from there.
    std::optional<std::size_t> mProcessingIndex;
};

[[nodiscard]] bool isBlocking(DiagnosticSeverity aSeverity, const ReportList& aReports) {
    return aSeverity == DiagnosticSeverity::Error && !aReports.empty();
}

[[nodiscard]] DraftDiagnostic draftOfParse(const SourcedParseDiagnostic& aSourced,
                                           int aReportingYear) {
    const auto& parse = aSourced.mDiagnostic;
    auto reports = scopeOfParseDiagnostic(aSourced, aReportingYear);

    ApplicationSource source{
        .mSourceIndex = aSourced.mSourceIndex,
        .mBroker = aSourced.mBroker,
        .mFilename = safeFilename(parse.mSourceFile),
        .mRow = parse.mRowIndex,
        .mTransactionId = parse.mTransactionId,
    };
    // The header is row 1, so the first data row is event 0.
    if (parse.mRowIndex && *parse.mRowIndex >= 2)
    {
        source.mEventIndex = *parse.mRowIndex - 2;
    }

    ApplicationDiagnostic diagnostic{
        .mSeverity = parse.mSeverity,
        .mStage = PipelineStage::Parsing,
        .mCode = parse.mCode,
        .mMessage = parse.mMessage,
        .mMessageKey = parse.mMessageKey,
        .mMessageParams = parse.mMessageParams,
        .mBlocking = isBlocking(parse.mSeverity, reports),
        .mSources = {std::move(source)},
        .mField = parse.mField,
    };
    diagnostic.mAffectedReports = std::move(reports);

    return DraftDiagnostic{std::move(diagnostic), std::nullopt};
}

[[nodiscard]] DraftDiagnostic draftOfMerge(const MergeDiagnostic& aMerge) {
    auto reports = scopeOfMergeDiagnostic(aMerge);

    auto sources = sourcesOf(aMerge.mSources);

    if (sources.empty() && aMerge.mSourceIndex)
    {
        sources.push_back(ApplicationSource{.mSourceIndex = *aMerge.mSourceIndex});
    }

    ApplicationDiagnostic diagnostic{
        .mSeverity = aMerge.mSeverity,
        .mStage = PipelineStage::Merging,
        .mCode = aMerge.mCode,
        .mMessage = aMerge.mMessage,
        .mMessageKey = aMerge.mMessageKey,
        .mMessageParams = aMerge.mMessageParams,
        .mBlocking = isBlocking(aMerge.mSeverity, reports),
        .mSources = std::move(sources),
        .mTaxDate = aMerge.mTaxDate,
        .mInstrumentName = aMerge.mInstrumentName,
        .mIsin = aMerge.mIsin,
    };
    diagnostic.mAffectedReports = std::move(reports);

    return DraftDiagnostic{std::move(diagnostic), std::nullopt};
}

/// The draft of a parser or merger diagnostic, as the merger returns it.
[[nodiscard]] DraftDiagnostic draftOfMergeInput(const StatementMergeDiagnostic& aDiagnostic,
                                                int aReportingYear) {
    const auto toDraft = [aReportingYear](const auto& aEntry) {
        using Diagnostic = std::decay_t<decltype(aEntry)>;

        if constexpr (std::is_same_v<Diagnostic, SourcedParseDiagnostic>)
        {
            return draftOfParse(aEntry, aReportingYear);
        }
        else
        {
            static_assert(std::is_same_v<Diagnostic, MergeDiagnostic>);
            return draftOfMerge(aEntry);
        }
    };

    return std::visit(toDraft, aDiagnostic);
}

[[nodiscard]] DraftDiagnostic draftOfProcessing(const ProcessingDiagnostic& aProcessing,
                                                std::size_t aIndex) {
    const auto& context = aProcessing.mContext;
    auto reports = context.mAffectedReports;
    normalizeReports(reports);

    auto sources = sourcesOf(context.mSources);

    ApplicationDiagnostic diagnostic{
        .mSeverity = aProcessing.mSeverity,
        .mStage = context.mStage,
        .mCode = aProcessing.mCode,
        .mMessage = aProcessing.mMessage,
        .mMessageKey = aProcessing.mMessageKey,
        .mMessageParams = aProcessing.mMessageParams,
        .mBlocking = isBlocking(aProcessing.mSeverity, reports),
        .mSources = std::move(sources),
        .mTaxDate = context.mTaxDate,
        .mField = context.mField,
        .mInstrumentName = context.mInstrumentName,
        .mIsin = context.mIsin,
        .mCashInterestBroker = context.mCashInterestBroker,
    };
    diagnostic.mAffectedReports = std::move(reports);

    return DraftDiagnostic{std::move(diagnostic), aIndex};
}

/// The deterministic order of diagnostics. The creation sequence is the position in the
/// combined input, which is stage by stage in processing order.
[[nodiscard]] auto sortKey(const ApplicationDiagnostic& aDiagnostic, std::size_t aSequence) {
    const auto codeIndex = [](const auto& aCode) { return static_cast<int>(aCode); };

    return std::make_tuple(static_cast<int>(aDiagnostic.mStage),
                           earliestFile(aDiagnostic.mSources),
                           earliestEvent(aDiagnostic.mSources),
                           dateKey(aDiagnostic.mTaxDate),
                           reportKey(aDiagnostic.mAffectedReports),
                           noneLast(aDiagnostic.mIsin),
                           aDiagnostic.mSeverity == DiagnosticSeverity::Error ? 0 : 1,
                           aDiagnostic.mCode.index(),
                           std::visit(codeIndex, aDiagnostic.mCode),
                           aSequence);
}

[[nodiscard]] bool affects(const ReportList& aReports, ReportType aType) {
    return std::find(aReports.begin(), aReports.end(), aType) != aReports.end();
}

[[nodiscard]] OverallStatus overallStatus(const ApplicationResult& aResult) {
    const auto hasStatus = [&aResult](ReportStatus aStatus) {
        return std::any_of(
            aResult.mReports.begin(),
            aResult.mReports.end(),
            [aStatus](const ReportResult& aReport) { return aReport.mStatus == aStatus; });
    };
    const auto failedCount = static_cast<std::size_t>(std::count_if(
        aResult.mReports.begin(),
        aResult.mReports.end(),
        [](const ReportResult& aReport) { return aReport.mStatus == ReportStatus::Failed; }));

    if (hasStatus(ReportStatus::NeedsInput))
    {
        return OverallStatus::NeedsInput;
    }
    if (failedCount == kReportCount)
    {
        return OverallStatus::Failed;
    }
    if (failedCount > 0)
    {
        return OverallStatus::PartialSuccess;
    }

    const bool hasNonBlocking = std::any_of(
        aResult.mDiagnostics.begin(),
        aResult.mDiagnostics.end(),
        [](const ApplicationDiagnostic& aDiagnostic) { return !aDiagnostic.mBlocking; });

    return hasNonBlocking ? OverallStatus::CompletedWithWarnings : OverallStatus::Success;
}

[[nodiscard]] bool isFailureResolution(RequestType aType) {
    return aType == RequestType::CapitalGainsFailureResolutionRequired ||
           aType == RequestType::DividendsFailureResolutionRequired ||
           aType == RequestType::OtherInterestFailureResolutionRequired;
}

/// True for exactly two characters from A to Z.
bool isTwoCapitalLetters(const std::string& aText) {
    return aText.size() == 2 && aText[0] >= 'A' && aText[0] <= 'Z' && aText[1] >= 'A' &&
           aText[1] <= 'Z';
}

/// A failure-resolution type needs a list with at least one investment or broker, a loss question
/// at least one sale, each with open units, a ratio question a source, open units and a date range
/// around its effective date, a classification question an ISIN, a bond type question an ISIN and
/// sold or matured units, and a quantity question an ISIN and open units below the sale units;
/// every other type has no question data until its processor defines it.
void checkQuestionData(const DecisionQuestion& aQuestion) {
    const auto* failure = std::get_if<FailureResolutionQuestion>(&aQuestion.mData);
    const auto* loss = std::get_if<LossEligibilityQuestion>(&aQuestion.mData);
    const auto* ratio = std::get_if<CorporateActionRatioQuestion>(&aQuestion.mData);
    const auto* classification = std::get_if<TaxClassificationQuestion>(&aQuestion.mData);
    const auto* quantity = std::get_if<QuantityCorrectionQuestion>(&aQuestion.mData);
    const auto* payer = std::get_if<PayerChoiceQuestion>(&aQuestion.mData);
    const auto* deposit = std::get_if<DepositInterestTotalQuestion>(&aQuestion.mData);
    const auto* income = std::get_if<IncomeClassificationQuestion>(&aQuestion.mData);
    const auto* bond = std::get_if<BondTypeQuestion>(&aQuestion.mData);
    const auto* treaty = std::get_if<TreatyClaimQuestion>(&aQuestion.mData);
    const auto* interestTreaty = std::get_if<InterestTreatyClaimQuestion>(&aQuestion.mData);
    const bool isLossType = aQuestion.mType == RequestType::LossEligibilityConfirmationRequired;
    const bool isRatioType = aQuestion.mType == RequestType::CorporateActionRatioRequired;
    const bool isClassificationType = aQuestion.mType == RequestType::TaxClassificationRequired;
    const bool isQuantityType = aQuestion.mType == RequestType::QuantityCorrectionRequired;
    const bool isIncomeDetailsType = aQuestion.mType == RequestType::IncomeDetailsRequired;
    const bool isDepositType = aQuestion.mType == RequestType::DepositInterestTotalRequired;

    if ((failure != nullptr) != isFailureResolution(aQuestion.mType) ||
        (loss != nullptr) != isLossType || (ratio != nullptr) != isRatioType ||
        ((classification != nullptr || income != nullptr) != isClassificationType) ||
        (quantity != nullptr) != isQuantityType ||
        ((payer != nullptr || bond != nullptr || treaty != nullptr || interestTreaty != nullptr) &&
         !isIncomeDetailsType) ||
        (deposit != nullptr) != isDepositType)
    {
        throw std::invalid_argument("The question data does not fit the request type.");
    }

    if (quantity != nullptr && (quantity->mIsin.empty() || quantity->mOpenUnits <= 0 ||
                                quantity->mOriginalUnits <= quantity->mOpenUnits))
    {
        throw std::invalid_argument("A quantity question needs an ISIN and open units below the "
                                    "sale units.");
    }

    if (payer != nullptr && (payer->mIsin.empty() || payer->mOptions.size() < 2))
    {
        throw std::invalid_argument("A payer choice question needs an ISIN and at least two "
                                    "reference rows.");
    }

    if (treaty != nullptr && (!isTwoCapitalLetters(treaty->mCountry) || treaty->mIsins.empty()))
    {
        throw std::invalid_argument("A treaty claim question needs a country of two capital "
                                    "letters and an ISIN.");
    }

    if (interestTreaty != nullptr &&
        (!isTwoCapitalLetters(interestTreaty->mCountry) ||
         (interestTreaty->mIsins.empty() && !interestTreaty->mCashInterest)))
    {
        throw std::invalid_argument("An interest treaty claim question needs a country of two "
                                    "capital letters and an ISIN or cash interest.");
    }

    if (deposit != nullptr && (deposit->mTotal <= 0 || deposit->mTotal > kDepositInterestAllowance))
    {
        throw std::invalid_argument("A deposit interest question needs a total above 0 and at "
                                    "most EUR 1,000.");
    }

    if (bond != nullptr &&
        (bond->mIsin.empty() || (bond->mSoldUnits <= 0 && bond->mMaturedUnits <= 0)))
    {
        throw std::invalid_argument("A bond type question needs an ISIN and sold or matured "
                                    "units.");
    }

    if (income != nullptr && income->mIsin && income->mIsin->empty())
    {
        throw std::invalid_argument("An income classification question has no ISIN or a real one.");
    }

    if (classification != nullptr && classification->mIsin.empty())
    {
        throw std::invalid_argument("A classification question needs an ISIN.");
    }

    if (failure != nullptr && failure->mInvestments.empty() && failure->mBrokers.empty())
    {
        throw std::invalid_argument("A failure-resolution question needs an investment or a "
                                    "broker.");
    }

    // Either the units at the row date, or the earlier sale that the action must come before.
    if (ratio != nullptr &&
        (ratio->mSources.empty() || ratio->mDateRanges.empty() ||
         ratio->mPositionBefore.has_value() == ratio->mEarlierSaleDate.has_value() ||
         ratio->mPositionBefore.value_or(1) <= 0))
    {
        throw std::invalid_argument("A ratio question needs a source and open units.");
    }

    if (ratio != nullptr && ratio->mPositionBefore)
    {
        const auto* rowRange = findEffectiveDateRange(*ratio, ratio->mEffectiveDate);

        if (rowRange == nullptr || rowRange->mPositionBefore != *ratio->mPositionBefore)
        {
            throw std::invalid_argument("A ratio question needs a date range around its date.");
        }
    }

    if (ratio != nullptr && ratio->mEarlierSaleDate &&
        ratio->mDateRanges.back().mLatestDate > *ratio->mEarlierSaleDate)
    {
        throw std::invalid_argument("A ratio question before a sale needs dates up to that sale.");
    }

    // Rows on different dates are asked whether they are one event, and only they are.
    if (ratio != nullptr &&
        ratio->mAskSameEvent != std::any_of(ratio->mSources.begin(),
                                            ratio->mSources.end(),
                                            [ratio](const CorporateActionSourceFacts& aSource) {
                                                return aSource.mDate !=
                                                       ratio->mSources.front().mDate;
                                            }))
    {
        throw std::invalid_argument("Only a ratio question with rows on different dates asks "
                                    "whether they are one event.");
    }

    if (loss == nullptr)
    {
        return;
    }

    const bool hasOpenUnits =
        std::all_of(loss->mSales.begin(), loss->mSales.end(), [](const LossSaleFacts& aSale) {
            return aSale.mOpenUnits > 0 && aSale.mOpenUnits <= aSale.mLossUnits;
        });

    if (loss->mSales.empty() || !hasOpenUnits)
    {
        throw std::invalid_argument("A loss question needs sales with open units.");
    }
}

/// Orders loss sales by date, then by transaction, then by units; a sale without a transaction
/// ID comes first on its date.
[[nodiscard]] auto lossSaleKey(const LossSaleFacts& aSale) {
    const auto& transaction = aSale.mSaleTransaction;

    return std::make_tuple(aSale.mSaleDate,
                           transaction.has_value(),
                           transaction ? transaction->mBroker : Broker::Unknown,
                           transaction ? transaction->mTransactionId : std::string{},
                           aSale.mSaleUnits,
                           aSale.mLossUnits);
}

/// The investments of a failure question, each with the IDs of its linked diagnostics.
[[nodiscard]] std::vector<RequestInvestment>
investmentsOf(const QuestionData& aData,
              const std::vector<std::size_t>& aLinkedDiagnostics,
              const std::vector<ApplicationDiagnostic>& aDiagnostics) {
    const auto* failure = std::get_if<FailureResolutionQuestion>(&aData);
    std::vector<RequestInvestment> investments;

    if (failure == nullptr)
    {
        return investments;
    }

    for (const auto& failed : failure->mInvestments)
    {
        RequestInvestment investment{
            .mIsin = failed.mIsin,
            .mName = failed.mName,
            .mReasonCodes = failed.mReasonCodes,
        };

        for (const auto position : aLinkedDiagnostics)
        {
            if (aDiagnostics[position].mIsin == failed.mIsin)
            {
                investment.mDiagnosticIds.push_back(aDiagnostics[position].mId);
            }
        }

        investments.push_back(std::move(investment));
    }

    return investments;
}

/// The brokers of a failure question, each with the IDs of its linked diagnostics.
[[nodiscard]] std::vector<RequestBroker>
brokersOf(const std::vector<FailedBroker>& aBrokers,
          const std::vector<std::size_t>& aLinkedDiagnostics,
          const std::vector<ApplicationDiagnostic>& aDiagnostics) {
    std::vector<RequestBroker> brokers;

    for (const auto& failed : aBrokers)
    {
        RequestBroker broker{.mBroker = failed.mBroker, .mReasonCodes = failed.mReasonCodes};

        for (const auto position : aLinkedDiagnostics)
        {
            if (aDiagnostics[position].mCashInterestBroker == failed.mBroker)
            {
                broker.mDiagnosticIds.push_back(aDiagnostics[position].mId);
            }
        }

        brokers.push_back(std::move(broker));
    }

    return brokers;
}

/// The exclusion of a form needs a non-empty list; every other choice, or none, has none.
void checkFailureResolution(const ReportInput& aInput) {
    const bool excludes = aInput.mFailureResolution == FailureResolution::ExcludeFailedInstruments;

    if (excludes != (!aInput.mExcludedInvestments.empty() || !aInput.mExcludedBrokers.empty()))
    {
        throw std::invalid_argument("Only an exclusion lists excluded investments or brokers.");
    }
}

/// The excluded investments of a form, sorted like the question, each with the IDs of the resolved
/// diagnostics of its ISIN that affect this form, in diagnostic order. An error of the same ISIN
/// that only another form has belongs to that form's exclusion.
[[nodiscard]] std::vector<RequestInvestment>
excludedInvestmentsOf(const ReportInput& aInput,
                      ReportType aType,
                      const std::vector<std::size_t>& aResolved,
                      const std::vector<std::size_t>& aProcessingToDiagnostic,
                      const std::vector<ApplicationDiagnostic>& aDiagnostics) {
    std::vector<std::size_t> resolvedPositions;

    for (const auto index : aResolved)
    {
        const auto position = aProcessingToDiagnostic.at(index);

        if (affects(aDiagnostics[position].mAffectedReports, aType))
        {
            resolvedPositions.push_back(position);
        }
    }

    std::sort(resolvedPositions.begin(), resolvedPositions.end());

    return investmentsOf(
        normalizedData(
            FailureResolutionQuestion{.mInvestments = aInput.mExcludedInvestments, .mBrokers = {}}),
        resolvedPositions,
        aDiagnostics);
}

/// The excluded brokers of a form, sorted like the question, each with the IDs of the resolved
/// diagnostics of its cash interest that affect this form, in diagnostic order.
[[nodiscard]] std::vector<RequestBroker>
excludedBrokersOf(const ReportInput& aInput,
                  ReportType aType,
                  const std::vector<std::size_t>& aResolved,
                  const std::vector<std::size_t>& aProcessingToDiagnostic,
                  const std::vector<ApplicationDiagnostic>& aDiagnostics) {
    std::vector<std::size_t> resolvedPositions;

    for (const auto index : aResolved)
    {
        const auto position = aProcessingToDiagnostic.at(index);

        if (affects(aDiagnostics[position].mAffectedReports, aType))
        {
            resolvedPositions.push_back(position);
        }
    }

    std::sort(resolvedPositions.begin(), resolvedPositions.end());

    const auto data = normalizedData(
        FailureResolutionQuestion{.mInvestments = {}, .mBrokers = aInput.mExcludedBrokers});

    return brokersOf(std::get<FailureResolutionQuestion>(data).mBrokers,
                     resolvedPositions,
                     aDiagnostics);
}

/// The excluded items of a run sorted by ISIN, each with its sources normalized and the IDs of its
/// diagnostics in diagnostic order.
[[nodiscard]] std::vector<ExcludedItem>
excludedItemsOf(std::vector<ExcludedItemInput>& aInputs,
                const std::vector<std::size_t>& aProcessingToDiagnostic,
                const ApplicationResult& aResult) {
    std::vector<ExcludedItem> items;

    for (auto& input : aInputs)
    {
        auto item = std::move(input.mItem);

        item.mSources = sourcesOf(input.mSources);
        normalizeSources(item.mSources);

        std::vector<std::size_t> positions;

        std::transform(input.mProcessingDiagnostics.begin(),
                       input.mProcessingDiagnostics.end(),
                       std::back_inserter(positions),
                       [&aProcessingToDiagnostic](std::size_t aIndex) {
                           return aProcessingToDiagnostic.at(aIndex);
                       });
        std::sort(positions.begin(), positions.end());
        positions.erase(std::unique(positions.begin(), positions.end()), positions.end());

        for (const auto position : positions)
        {
            item.mDiagnosticIds.push_back(aResult.mDiagnostics[position].mId);
        }

        items.push_back(std::move(item));
    }

    std::stable_sort(items.begin(),
                     items.end(),
                     [](const ExcludedItem& aLeft, const ExcludedItem& aRight) {
                         return aLeft.mIsin < aRight.mIsin;
                     });

    return items;
}

/// The investments with a sale of the year, by ISIN. An excluded one keeps the reason codes of its
/// exclusion; a blocked one the codes of its blocking capital errors in diagnostic order, each
/// once.
[[nodiscard]] std::vector<CapitalInvestment>
capitalInvestmentsOf(std::vector<CapitalInvestmentInput>& aInputs,
                     const ApplicationResult& aResult) {
    const auto& excluded = aResult.report(ReportType::CapitalGains).mExcludedInvestments;
    std::vector<CapitalInvestment> investments;

    investments.reserve(aInputs.size());

    for (auto& input : aInputs)
    {
        CapitalInvestment investment{
            .mIsin = input.mIsin,
            .mName = std::move(input.mName),
            .mListedSales = input.mListedSales,
            .mListedUnits = input.mListedUnits,
            .mBlockedLossUnits = input.mBlockedLossUnits,
        };

        for (const auto& [date, source] : input.mSales)
        {
            investment.mSales.push_back({.mDate = date, .mSource = sourceOf(source)});
        }

        const auto left = std::find_if(excluded.begin(),
                                       excluded.end(),
                                       [&input](const RequestInvestment& aInvestment) {
                                           return aInvestment.mIsin == input.mIsin;
                                       });

        if (left != excluded.end())
        {
            investment.mState = CapitalInvestmentState::Excluded;
            investment.mReasonCodes = left->mReasonCodes;
        }
        else
        {
            for (const auto& diagnostic : aResult.mDiagnostics)
            {
                if (diagnostic.mBlocking && diagnostic.mIsin == input.mIsin &&
                    affects(diagnostic.mAffectedReports, ReportType::CapitalGains) &&
                    std::find(investment.mReasonCodes.begin(),
                              investment.mReasonCodes.end(),
                              diagnostic.mCode) == investment.mReasonCodes.end())
                {
                    investment.mReasonCodes.push_back(diagnostic.mCode);
                }
            }

            if (!investment.mReasonCodes.empty())
            {
                investment.mState = CapitalInvestmentState::Blocked;
            }
            else
            {
                investment.mState = input.mListed ? CapitalInvestmentState::Listed
                                                  : CapitalInvestmentState::LongHeldOnly;
            }
        }

        investments.push_back(std::move(investment));
    }

    std::stable_sort(investments.begin(),
                     investments.end(),
                     [](const CapitalInvestment& aLeft, const CapitalInvestment& aRight) {
                         return aLeft.mIsin < aRight.mIsin;
                     });

    return investments;
}

/// The corrections by ISIN, keeping the ledger order inside an ISIN.
[[nodiscard]] std::vector<AppliedCorrection>
correctionsOf(std::vector<AppliedCorrectionInput>& aInputs) {
    std::vector<AppliedCorrection> corrections;

    corrections.reserve(aInputs.size());

    std::transform(aInputs.begin(),
                   aInputs.end(),
                   std::back_inserter(corrections),
                   [](AppliedCorrectionInput& aInput) {
                       return AppliedCorrection{
                           .mIsin = std::move(aInput.mIsin),
                           .mSaleDate = aInput.mSaleDate,
                           .mSaleSource = sourceOf(aInput.mSaleSource),
                           .mOriginalUnits = aInput.mOriginalUnits,
                           .mCorrectedUnits = aInput.mCorrectedUnits,
                           .mKind = aInput.mKind,
                           .mImpact = std::move(aInput.mImpact),
                       };
                   });

    std::stable_sort(corrections.begin(),
                     corrections.end(),
                     [](const AppliedCorrection& aLeft, const AppliedCorrection& aRight) {
                         return aLeft.mIsin < aRight.mIsin;
                     });

    return corrections;
}

} // namespace

const EffectiveDateRange* findEffectiveDateRange(const CorporateActionRatioQuestion& aQuestion,
                                                 Date aDate) {
    const auto range =
        std::find_if(aQuestion.mDateRanges.begin(),
                     aQuestion.mDateRanges.end(),
                     [aDate](const EffectiveDateRange& aRange) {
                         return aRange.mEarliestDate <= aDate && aDate <= aRange.mLatestDate;
                     });

    return range == aQuestion.mDateRanges.end() ? nullptr : &*range;
}

std::string_view trimmedTreatyStatement(std::string_view aText) {
    constexpr std::array<std::string_view, 2> kBlanks{" ", "\xC2\xA0"}; // space, no-break space
    bool isTrimmed = false;

    while (!isTrimmed)
    {
        isTrimmed = true;

        for (const auto blank : kBlanks)
        {
            if (aText.starts_with(blank))
            {
                aText.remove_prefix(blank.size());
                isTrimmed = false;
            }

            if (aText.ends_with(blank))
            {
                aText.remove_suffix(blank.size());
                isTrimmed = false;
            }
        }
    }

    return aText;
}

bool isValidTreatyStatement(std::string_view aText) {
    const auto text = trimmedTreatyStatement(aText);
    std::size_t length = 0;
    std::size_t position = 0;

    while (position < text.size())
    {
        const auto lead = static_cast<unsigned char>(text[position]);
        std::size_t extraBytes = 0;
        char32_t code = lead;

        if (lead >= 0xF0 && lead <= 0xF4)
        {
            extraBytes = 3;
            code = lead & 0x07U;
        }
        else if (lead >= 0xE0 && lead <= 0xEF)
        {
            extraBytes = 2;
            code = lead & 0x0FU;
        }
        else if (lead >= 0xC2 && lead <= 0xDF)
        {
            extraBytes = 1;
            code = lead & 0x1FU;
        }
        else if (lead >= 0x80)
        {
            return false; // a continuation byte alone, or an invalid lead byte
        }

        if (position + extraBytes >= text.size())
        {
            return false; // the sequence is cut off
        }

        for (std::size_t offset = 1; offset <= extraBytes; ++offset)
        {
            const auto next = static_cast<unsigned char>(text[position + offset]);

            if ((next & 0xC0U) != 0x80U)
            {
                return false;
            }

            code = (code << 6U) | (next & 0x3FU);
        }

        const bool overlong =
            (extraBytes == 2 && code < 0x800) || (extraBytes == 3 && code < 0x10000);
        const bool surrogate = code >= 0xD800 && code <= 0xDFFF;
        const bool control = code < 0x20 || (code >= 0x7F && code <= 0x9F);

        if (overlong || surrogate || code > 0x10FFFF || control)
        {
            return false;
        }

        ++length;
        position += extraBytes + 1;
    }

    // The words true and false are the form's yes/no values, not a treaty text.
    std::string lower{text};

    std::transform(lower.begin(), lower.end(), lower.begin(), [](char aCharacter) {
        return aCharacter >= 'A' && aCharacter <= 'Z' ? static_cast<char>(aCharacter + ('a' - 'A'))
                                                      : aCharacter;
    });

    // A trimmed text that is not empty starts with a visible character.
    return !text.empty() && length <= kMaxTreatyStatementLength && lower != "true" &&
           lower != "false";
}

QuestionData normalizedData(QuestionData aData) {
    if (auto* failure = std::get_if<FailureResolutionQuestion>(&aData))
    {
        auto& investments = failure->mInvestments;

        for (auto& investment : investments)
        {
            auto& codes = investment.mReasonCodes;
            std::sort(codes.begin(), codes.end());
            codes.erase(std::unique(codes.begin(), codes.end()), codes.end());
        }

        std::sort(investments.begin(),
                  investments.end(),
                  [](const FailedInvestment& aLeft, const FailedInvestment& aRight) {
                      return std::tie(aLeft.mIsin, aLeft.mName) <
                             std::tie(aRight.mIsin, aRight.mName);
                  });

        for (auto& broker : failure->mBrokers)
        {
            auto& codes = broker.mReasonCodes;
            std::sort(codes.begin(), codes.end());
            codes.erase(std::unique(codes.begin(), codes.end()), codes.end());
        }

        std::sort(failure->mBrokers.begin(),
                  failure->mBrokers.end(),
                  [](const FailedBroker& aLeft, const FailedBroker& aRight) {
                      return aLeft.mBroker < aRight.mBroker;
                  });
    }

    if (auto* loss = std::get_if<LossEligibilityQuestion>(&aData))
    {
        for (auto& sale : loss->mSales)
        {
            std::sort(sale.mKnownReplacements.begin(),
                      sale.mKnownReplacements.end(),
                      [](const KnownReplacement& aLeft, const KnownReplacement& aRight) {
                          return std::tie(aLeft.mDate, aLeft.mUnits) <
                                 std::tie(aRight.mDate, aRight.mUnits);
                      });
        }

        std::sort(loss->mSales.begin(),
                  loss->mSales.end(),
                  [](const LossSaleFacts& aLeft, const LossSaleFacts& aRight) {
                      return lossSaleKey(aLeft) < lossSaleKey(aRight);
                  });
    }

    if (auto* classification = std::get_if<TaxClassificationQuestion>(&aData))
    {
        auto& labels = classification->mClassLabels;

        std::sort(labels.begin(), labels.end());
        labels.erase(std::unique(labels.begin(), labels.end()), labels.end());
    }

    if (auto* income = std::get_if<IncomeClassificationQuestion>(&aData))
    {
        auto& labels = income->mClassLabels;

        std::sort(labels.begin(), labels.end());
        labels.erase(std::unique(labels.begin(), labels.end()), labels.end());
    }

    if (auto* payer = std::get_if<PayerChoiceQuestion>(&aData))
    {
        std::sort(payer->mOptions.begin(),
                  payer->mOptions.end(),
                  [](const PayerChoiceOption& aLeft, const PayerChoiceOption& aRight) {
                      return std::tie(aLeft.mName,
                                      aLeft.mAddress,
                                      aLeft.mCountry,
                                      aLeft.mTaxNumber,
                                      aLeft.mSource.mFile,
                                      aLeft.mSource.mRowNumber) <
                             std::tie(aRight.mName,
                                      aRight.mAddress,
                                      aRight.mCountry,
                                      aRight.mTaxNumber,
                                      aRight.mSource.mFile,
                                      aRight.mSource.mRowNumber);
                  });
    }

    if (auto* treaty = std::get_if<TreatyClaimQuestion>(&aData))
    {
        auto& isins = treaty->mIsins;

        std::sort(isins.begin(), isins.end());
        isins.erase(std::unique(isins.begin(), isins.end()), isins.end());
    }

    if (auto* interestTreaty = std::get_if<InterestTreatyClaimQuestion>(&aData))
    {
        auto& isins = interestTreaty->mIsins;

        std::sort(isins.begin(), isins.end());
        isins.erase(std::unique(isins.begin(), isins.end()), isins.end());
    }

    if (auto* ratio = std::get_if<CorporateActionRatioQuestion>(&aData))
    {
        std::sort(
            ratio->mSources.begin(),
            ratio->mSources.end(),
            [](const CorporateActionSourceFacts& aLeft, const CorporateActionSourceFacts& aRight) {
                return std::tie(aLeft.mDate,
                                aLeft.mBroker,
                                aLeft.mTransactionId,
                                aLeft.mSourceValue) < std::tie(aRight.mDate,
                                                               aRight.mBroker,
                                                               aRight.mTransactionId,
                                                               aRight.mSourceValue);
            });
    }

    return aData;
}

bool hasSourceError(const ApplicationResultInput& aInput, ReportType aReport) {
    for (std::size_t position = 0; position < aInput.mMergeDiagnostics.size(); ++position)
    {
        const bool isResolved = std::find(aInput.mResolvedMergeDiagnostics.begin(),
                                          aInput.mResolvedMergeDiagnostics.end(),
                                          position) != aInput.mResolvedMergeDiagnostics.end();
        const auto draft =
            draftOfMergeInput(aInput.mMergeDiagnostics[position], aInput.mReportingYear);

        if (!isResolved && draft.mDiagnostic.mBlocking &&
            affects(draft.mDiagnostic.mAffectedReports, aReport))
        {
            return true;
        }
    }

    return false;
}

void appendStageOutput(ApplicationResultInput& aInput,
                       std::vector<ProcessingDiagnostic>&& aDiagnostics,
                       std::vector<RequestInput>&& aRequests) {
    const auto offset = aInput.mProcessingDiagnostics.size();

    std::move(aDiagnostics.begin(),
              aDiagnostics.end(),
              std::back_inserter(aInput.mProcessingDiagnostics));

    for (auto& request : aRequests)
    {
        auto& indices = request.mProcessingDiagnostics;

        std::transform(indices.begin(),
                       indices.end(),
                       indices.begin(),
                       [offset](std::size_t aIndex) { return aIndex + offset; });
        aInput.mRequests.push_back(std::move(request));
    }
}

ApplicationResult ApplicationResultBuilder::build(ApplicationResultInput&& aInput) {
    for (const auto& request : aInput.mRequests)
    {
        checkQuestionData(request.mQuestion);
    }

    // Collect the diagnostics of every stage in stage order; the position is the creation order.
    std::vector<DraftDiagnostic> drafts;

    std::transform(aInput.mMergeDiagnostics.begin(),
                   aInput.mMergeDiagnostics.end(),
                   std::back_inserter(drafts),
                   [&aInput](const StatementMergeDiagnostic& aMerged) {
                       return draftOfMergeInput(aMerged, aInput.mReportingYear);
                   });
    for (std::size_t index = 0; index < aInput.mProcessingDiagnostics.size(); ++index)
    {
        drafts.push_back(draftOfProcessing(aInput.mProcessingDiagnostics[index], index));
    }
    for (const auto position : aInput.mResolvedMergeDiagnostics)
    {
        if (position >= aInput.mMergeDiagnostics.size())
        {
            throw std::invalid_argument("A resolved merge diagnostic must be a merge diagnostic.");
        }

        // The class error stays in the result, but the classification stage gives the blocking one.
        drafts[position].mDiagnostic.mBlocking = false;
    }
    for (const auto position : aInput.mResolvedDiagnostics)
    {
        if (position >= aInput.mProcessingDiagnostics.size())
        {
            throw std::invalid_argument("A resolved diagnostic must be a processing diagnostic.");
        }

        // The error stays in the result, but the exclusion lifted its block.
        drafts[aInput.mMergeDiagnostics.size() + position].mDiagnostic.mBlocking = false;
    }
    for (auto& draft : drafts)
    {
        normalizeSources(draft.mDiagnostic.mSources);
    }

    // Sort first, then assign IDs.
    std::vector<std::size_t> diagnosticOrder(drafts.size());
    std::iota(diagnosticOrder.begin(), diagnosticOrder.end(), std::size_t{0});

    // Each key is made once; a key holds an ISIN string, so making it in every comparison is slow.
    using DiagnosticSortKey =
        decltype(sortKey(std::declval<const ApplicationDiagnostic&>(), std::size_t{}));
    std::vector<DiagnosticSortKey> diagnosticKeys;

    diagnosticKeys.reserve(drafts.size());

    for (std::size_t index = 0; index < drafts.size(); ++index)
    {
        diagnosticKeys.push_back(sortKey(drafts[index].mDiagnostic, index));
    }

    std::stable_sort(diagnosticOrder.begin(),
                     diagnosticOrder.end(),
                     [&diagnosticKeys](std::size_t aLeft, std::size_t aRight) {
                         return diagnosticKeys[aLeft] < diagnosticKeys[aRight];
                     });

    ApplicationResult result;
    result.mRunRevision = aInput.mRunRevision;
    // Every processing diagnostic has a draft, so every entry is set in the loop below.
    std::vector<std::size_t> processingToDiagnostic(aInput.mProcessingDiagnostics.size());

    for (const auto draftIndex : diagnosticOrder)
    {
        auto& draft = drafts[draftIndex];
        draft.mDiagnostic.mId = numberedId("diagnostic", result.mDiagnostics.size() + 1);

        if (draft.mProcessingIndex)
        {
            processingToDiagnostic[*draft.mProcessingIndex] = result.mDiagnostics.size();
        }

        result.mDiagnostics.push_back(std::move(draft.mDiagnostic));
    }

    // Requests use the same ordering keys that apply to them.
    std::vector<DecisionRequest> requests;

    // Only mProcessingDiagnostics of a request input is read after this loop.
    for (auto& input : aInput.mRequests)
    {
        auto& question = input.mQuestion;

        DecisionRequest request{
            .mType = question.mType,
            .mRunRevision = aInput.mRunRevision,
            .mAffectedReports = std::move(question.mAffectedReports),
            .mSources = sourcesOf(question.mSources),
            .mTaxDate = question.mTaxDate,
            .mIsin = std::move(question.mIsin),
            .mData = normalizedData(std::move(question.mData)),
        };
        normalizeReports(request.mAffectedReports);
        normalizeSources(request.mSources);

        requests.push_back(std::move(request));
    }

    const auto requestKey = [&requests](std::size_t aIndex) {
        const auto& request = requests[aIndex];

        return std::make_tuple(earliestFile(request.mSources),
                               earliestEvent(request.mSources),
                               dateKey(request.mTaxDate),
                               reportKey(request.mAffectedReports),
                               noneLast(request.mIsin),
                               static_cast<int>(request.mType),
                               aIndex);
    };

    std::vector<decltype(requestKey(std::size_t{}))> requestKeys;

    requestKeys.reserve(requests.size());

    for (std::size_t index = 0; index < requests.size(); ++index)
    {
        requestKeys.push_back(requestKey(index));
    }

    std::vector<std::size_t> requestOrder(requests.size());
    std::iota(requestOrder.begin(), requestOrder.end(), std::size_t{0});
    std::stable_sort(requestOrder.begin(),
                     requestOrder.end(),
                     [&requestKeys](std::size_t aLeft, std::size_t aRight) {
                         return requestKeys[aLeft] < requestKeys[aRight];
                     });

    // The requests linked to each diagnostic, by position in result.mRequests.
    std::vector<std::vector<std::size_t>> linkedRequests(result.mDiagnostics.size());

    for (const auto requestIndex : requestOrder)
    {
        auto& request = requests[requestIndex];
        request.mId = numberedId("request", result.mRequests.size() + 1);

        // A diagnostic names the first request, in final order, that resolves it.
        std::vector<std::size_t> linkedDiagnostics;

        for (const auto processingIndex : aInput.mRequests[requestIndex].mProcessingDiagnostics)
        {
            const auto position = processingToDiagnostic.at(processingIndex);

            if (!result.mDiagnostics[position].mRequestId)
            {
                result.mDiagnostics[position].mRequestId = request.mId;
            }

            linkedRequests[position].push_back(result.mRequests.size());
            linkedDiagnostics.push_back(position);
        }

        std::sort(linkedDiagnostics.begin(), linkedDiagnostics.end());
        linkedDiagnostics.erase(std::unique(linkedDiagnostics.begin(), linkedDiagnostics.end()),
                                linkedDiagnostics.end());
        request.mInvestments = investmentsOf(request.mData, linkedDiagnostics, result.mDiagnostics);

        if (const auto* failure = std::get_if<FailureResolutionQuestion>(&request.mData))
        {
            request.mBrokers = brokersOf(failure->mBrokers, linkedDiagnostics, result.mDiagnostics);
        }

        result.mRequests.push_back(std::move(request));
    }

    // The status of each form.
    for (std::size_t index = 0; index < kReportCount; ++index)
    {
        const auto type = static_cast<ReportType>(index);
        auto& report = result.mReports[index];
        auto& input = aInput.mReports[index];

        checkFailureResolution(input);
        report.mFailureResolution = input.mFailureResolution;
        report.mExcludedInvestments = excludedInvestmentsOf(input,
                                                            type,
                                                            aInput.mResolvedDiagnostics,
                                                            processingToDiagnostic,
                                                            result.mDiagnostics);
        report.mExcludedBrokers = excludedBrokersOf(input,
                                                    type,
                                                    aInput.mResolvedDiagnostics,
                                                    processingToDiagnostic,
                                                    result.mDiagnostics);

        bool hasUnresolvedBlocker = false;

        for (std::size_t position = 0; position < result.mDiagnostics.size(); ++position)
        {
            const auto& diagnostic = result.mDiagnostics[position];

            if (!affects(diagnostic.mAffectedReports, type))
            {
                continue;
            }

            report.mDiagnosticIds.push_back(diagnostic.mId);

            // Only a linked request of this report can lead to a valid report.
            const bool hasRequestOfThisReport =
                std::any_of(linkedRequests[position].begin(),
                            linkedRequests[position].end(),
                            [&result, type](std::size_t aRequest) {
                                return affects(result.mRequests[aRequest].mAffectedReports, type);
                            });

            if (diagnostic.mBlocking && !hasRequestOfThisReport)
            {
                hasUnresolvedBlocker = true;
            }
        }

        bool hasBlockingRequest = false;

        for (const auto& request : result.mRequests)
        {
            if (affects(request.mAffectedReports, type))
            {
                report.mRequestIds.push_back(request.mId);

                // A payer choice, a treaty claim and the deposit interest total are listed but
                // never stop a form: a skip, no claim or no answer is a valid way to go on.
                hasBlockingRequest |=
                    !std::holds_alternative<PayerChoiceQuestion>(request.mData) &&
                    !std::holds_alternative<DepositInterestTotalQuestion>(request.mData) &&
                    !std::holds_alternative<TreatyClaimQuestion>(request.mData) &&
                    !std::holds_alternative<InterestTreatyClaimQuestion>(request.mData);
            }
        }

        if (hasUnresolvedBlocker)
        {
            report.mStatus = ReportStatus::Failed;
        }
        else if (hasBlockingRequest)
        {
            report.mStatus = ReportStatus::NeedsInput;
        }
        else if (input.mXml)
        {
            report.mStatus = ReportStatus::Generated;
            report.mXml = std::move(input.mXml);
        }
        else
        {
            report.mStatus = ReportStatus::NoData;
            report.mNoDataReason = input.mNoDataReason;
        }

        report.mIncome = std::move(input.mIncome);
    }

    result.mExcludedItems = excludedItemsOf(aInput.mExcludedItems, processingToDiagnostic, result);
    result.mCapitalInvestments = capitalInvestmentsOf(aInput.mCapitalInvestments, result);
    result.mLongHeld = std::move(aInput.mLongHeld);
    result.mUsedRates = std::move(aInput.mUsedRates);

    // By date, then currency code. The stage already keeps one rate per currency and day.
    std::stable_sort(result.mUsedRates.begin(),
                     result.mUsedRates.end(),
                     [](const UsedOfficialRate& aLeft, const UsedOfficialRate& aRight) {
                         return std::tie(aLeft.mTaxDate, aLeft.mRate.mCurrencyCode) <
                                std::tie(aRight.mTaxDate, aRight.mRate.mCurrencyCode);
                     });

    result.mDepositInterest = aInput.mDepositInterest;
    result.mCorrections = correctionsOf(aInput.mCorrections);
    result.mReconciliation = std::move(aInput.mReconciliation);

    std::stable_sort(result.mReconciliation.begin(),
                     result.mReconciliation.end(),
                     [](const ReconciliationImpact& aLeft, const ReconciliationImpact& aRight) {
                         return aLeft.mIsin < aRight.mIsin;
                     });

    result.mDataSources = std::move(aInput.mDataSources);
    result.mStatus = overallStatus(result);

    return result;
}

} // namespace taxbroker
