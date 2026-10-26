#include "taxbroker/application_result.hpp"

#include <algorithm>
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

[[nodiscard]] std::string numberedId(std::string_view aPrefix, std::size_t aNumber) {
    auto digits = std::to_string(aNumber);

    if (digits.size() < 4)
    {
        digits.insert(0, 4 - digits.size(), '0');
    }

    return std::string{aPrefix} + "-" + digits;
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

} // namespace

ApplicationResult ApplicationResultBuilder::build(ApplicationResultInput&& aInput) {
    // Collect the diagnostics of every stage in stage order; the position is the creation order.
    std::vector<DraftDiagnostic> drafts;

    for (const auto& merged : aInput.mMergeDiagnostics)
    {
        const auto toDraft = [&aInput](const auto& aDiagnostic) {
            using Diagnostic = std::decay_t<decltype(aDiagnostic)>;

            if constexpr (std::is_same_v<Diagnostic, SourcedParseDiagnostic>)
            {
                return draftOfParse(aDiagnostic, aInput.mReportingYear);
            }
            else
            {
                static_assert(std::is_same_v<Diagnostic, MergeDiagnostic>);
                return draftOfMerge(aDiagnostic);
            }
        };

        drafts.push_back(std::visit(toDraft, merged));
    }
    for (std::size_t index = 0; index < aInput.mProcessingDiagnostics.size(); ++index)
    {
        drafts.push_back(draftOfProcessing(aInput.mProcessingDiagnostics[index], index));
    }
    for (auto& draft : drafts)
    {
        normalizeSources(draft.mDiagnostic.mSources);
    }

    // Sort first, then assign IDs.
    std::vector<std::size_t> diagnosticOrder(drafts.size());
    std::iota(diagnosticOrder.begin(), diagnosticOrder.end(), std::size_t{0});
    std::stable_sort(diagnosticOrder.begin(),
                     diagnosticOrder.end(),
                     [&drafts](std::size_t aLeft, std::size_t aRight) {
                         return sortKey(drafts[aLeft].mDiagnostic, aLeft) <
                                sortKey(drafts[aRight].mDiagnostic, aRight);
                     });

    ApplicationResult result;
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

    for (const auto& input : aInput.mRequests)
    {
        DecisionRequest request{
            .mType = input.mType,
            .mAffectedReports = input.mAffectedReports,
            .mTaxDate = input.mTaxDate,
            .mIsin = input.mIsin,
        };
        normalizeReports(request.mAffectedReports);
        request.mSources = sourcesOf(input.mSources);
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

    std::vector<std::size_t> requestOrder(requests.size());
    std::iota(requestOrder.begin(), requestOrder.end(), std::size_t{0});
    std::stable_sort(requestOrder.begin(),
                     requestOrder.end(),
                     [&requestKey](std::size_t aLeft, std::size_t aRight) {
                         return requestKey(aLeft) < requestKey(aRight);
                     });

    // The requests linked to each diagnostic, by position in result.mRequests.
    std::vector<std::vector<std::size_t>> linkedRequests(result.mDiagnostics.size());

    for (const auto requestIndex : requestOrder)
    {
        auto& request = requests[requestIndex];
        request.mId = numberedId("request", result.mRequests.size() + 1);

        // A diagnostic names the first request, in final order, that resolves it.
        for (const auto processingIndex : aInput.mRequests[requestIndex].mProcessingDiagnostics)
        {
            const auto position = processingToDiagnostic.at(processingIndex);

            if (!result.mDiagnostics[position].mRequestId)
            {
                result.mDiagnostics[position].mRequestId = request.mId;
            }

            linkedRequests[position].push_back(result.mRequests.size());
        }

        result.mRequests.push_back(std::move(request));
    }

    // The status of each form.
    for (std::size_t index = 0; index < kReportCount; ++index)
    {
        const auto type = static_cast<ReportType>(index);
        auto& report = result.mReports[index];
        auto& input = aInput.mReports[index];

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
        for (const auto& request : result.mRequests)
        {
            if (affects(request.mAffectedReports, type))
            {
                report.mRequestIds.push_back(request.mId);
            }
        }

        if (hasUnresolvedBlocker)
        {
            report.mStatus = ReportStatus::Failed;
        }
        else if (!report.mRequestIds.empty())
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
    }

    result.mStatus = overallStatus(result);

    return result;
}

} // namespace taxbroker
