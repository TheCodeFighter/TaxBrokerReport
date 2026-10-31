#include "processors/capital_failure_resolution.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace taxbroker {
namespace {

bool affectsCapital(const std::vector<ReportType>& aReports) {
    return std::find(aReports.begin(), aReports.end(), ReportType::CapitalGains) != aReports.end();
}

/// A capital error that blocks the form, as the result builder decides it.
bool isCapitalError(const ProcessingDiagnostic& aDiagnostic) {
    return aDiagnostic.mSeverity == DiagnosticSeverity::Error &&
           affectsCapital(aDiagnostic.mContext.mAffectedReports);
}

/// An error that only the capital form has and that names a complete ISIN: missing history, a
/// corporate action, the FIFO or loss rule, or a gate. Leaving every row of that ISIN out is then
/// safe. The XML writer finds its errors only in the final document, so they never are.
bool belongsToOneInvestment(const ProcessingDiagnostic& aDiagnostic) {
    const auto& context = aDiagnostic.mContext;

    return context.mStage != PipelineStage::XmlGeneration && context.mIsin.has_value() &&
           context.mAffectedReports == std::vector<ReportType>{ReportType::CapitalGains};
}

/// The positions of the diagnostics that an open capital question can resolve.
std::set<std::size_t> linkedToCapitalQuestion(const ApplicationResultInput& aInput) {
    std::set<std::size_t> linked;

    for (const auto& request : aInput.mRequests)
    {
        if (affectsCapital(request.mQuestion.mAffectedReports))
        {
            linked.insert(request.mProcessingDiagnostics.begin(),
                          request.mProcessingDiagnostics.end());
        }
    }

    return linked;
}

/// The question with one entry per ISIN of the listed diagnostics.
FailureResolutionQuestion questionOf(const std::vector<ProcessingDiagnostic>& aDiagnostics,
                                     const std::vector<std::size_t>& aListed) {
    std::map<Isin, FailedInvestment> investments;

    for (const auto position : aListed)
    {
        const auto& context = aDiagnostics.at(position).mContext;
        auto& investment = investments[context.mIsin.value()];

        investment.mIsin = context.mIsin.value();
        investment.mReasonCodes.push_back(aDiagnostics.at(position).mCode);

        if (investment.mName.empty() && context.mInstrumentName)
        {
            investment.mName = *context.mInstrumentName;
        }
    }

    FailureResolutionQuestion question;

    for (auto& [isin, investment] : investments)
    {
        question.mInvestments.push_back(std::move(investment));
    }

    return question;
}

/// The choice in an answer to the failure question, or nothing when the user cancelled.
std::optional<FailureResolution> choiceOf(const AnswerValue& aAnswer) {
    const auto choice = [](const auto& aValue) -> std::optional<FailureResolution> {
        using Answer = std::decay_t<decltype(aValue)>;

        if constexpr (std::is_same_v<Answer, FailureResolutionAnswer>)
        {
            return aValue.mChoice;
        }
        else if constexpr (std::is_same_v<Answer, CancelAnswer>)
        {
            return std::nullopt;
        }
        else
        {
            static_assert(std::is_same_v<Answer, NoOtherActivityAnswer> ||
                          std::is_same_v<Answer, OtherActivityAnswer> ||
                          std::is_same_v<Answer, SplitRatioAnswer> ||
                          std::is_same_v<Answer, SeparateEventsAnswer>);
            // The session accepts only a failure choice or a cancel for this question.
            throw std::logic_error("Only a failure choice or a cancel can answer this question.");
        }
    };

    return std::visit(choice, aAnswer);
}

/// Resolves the errors of the excluded ISINs and drops their open questions, as no answer to those
/// can change the report any more. A question whose linked error stays unresolved (it also
/// affects another form) stays open.
void exclude(ApplicationResultInput& aInput, const std::set<Isin>& aExcluded) {
    const auto& diagnostics = aInput.mProcessingDiagnostics;

    for (std::size_t position = 0; position < diagnostics.size(); ++position)
    {
        const auto& diagnostic = diagnostics[position];

        if (isCapitalError(diagnostic) && belongsToOneInvestment(diagnostic) &&
            aExcluded.contains(diagnostic.mContext.mIsin.value()))
        {
            aInput.mResolvedDiagnostics.push_back(position);
        }
    }

    const auto isResolved = [&aInput](std::size_t aPosition) {
        return std::find(aInput.mResolvedDiagnostics.begin(),
                         aInput.mResolvedDiagnostics.end(),
                         aPosition) != aInput.mResolvedDiagnostics.end();
    };
    const auto isOfExcluded = [&aExcluded, &isResolved](const RequestInput& aRequest) {
        const auto& question = aRequest.mQuestion;

        return affectsCapital(question.mAffectedReports) && question.mIsin &&
               aExcluded.contains(*question.mIsin) &&
               std::all_of(aRequest.mProcessingDiagnostics.begin(),
                           aRequest.mProcessingDiagnostics.end(),
                           isResolved);
    };

    std::erase_if(aInput.mRequests, isOfExcluded);
}

} // namespace

std::vector<Isin> CapitalFailureResolution::resolve(ApplicationResultInput& aInput,
                                                    RunDecisions& aDecisions) {
    const auto& diagnostics = aInput.mProcessingDiagnostics;
    const auto linked = linkedToCapitalQuestion(aInput);
    std::vector<std::size_t> listed;

    for (std::size_t position = 0; position < diagnostics.size(); ++position)
    {
        if (!isCapitalError(diagnostics[position]) || linked.contains(position))
        {
            continue;
        }

        // An error that no investment owns cannot be solved by leaving investments out.
        if (!belongsToOneInvestment(diagnostics[position]))
        {
            return {};
        }

        listed.push_back(position);
    }

    if (listed.empty() || hasSourceError(aInput, ReportType::CapitalGains))
    {
        return {};
    }

    DecisionQuestion question{
        .mType = RequestType::CapitalGainsFailureResolutionRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mData = questionOf(diagnostics, listed),
    };

    for (const auto position : listed)
    {
        const auto& sources = diagnostics[position].mContext.mSources;

        question.mSources.insert(question.mSources.end(), sources.begin(), sources.end());
    }

    const auto* answer = aDecisions.find(question);

    if (answer == nullptr)
    {
        aInput.mRequests.push_back(
            RequestInput{.mQuestion = std::move(question), .mProcessingDiagnostics = listed});

        return {};
    }

    const auto choice = choiceOf(*answer);

    if (!choice)
    {
        return {};
    }

    auto& capital = aInput.mReports[static_cast<std::size_t>(ReportType::CapitalGains)];

    capital.mFailureResolution = choice;

    if (choice == FailureResolution::FailReport)
    {
        return {};
    }

    const auto& investments = std::get<FailureResolutionQuestion>(question.mData).mInvestments;
    std::set<Isin> excluded;

    std::transform(investments.begin(),
                   investments.end(),
                   std::inserter(excluded, excluded.end()),
                   [](const FailedInvestment& aInvestment) { return aInvestment.mIsin; });
    capital.mExcludedInvestments = investments;
    exclude(aInput, excluded);

    return {excluded.begin(), excluded.end()};
}

} // namespace taxbroker
