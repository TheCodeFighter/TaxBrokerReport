#include "processors/failure_resolution_stage.hpp"

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

bool affects(const std::vector<ReportType>& aReports, ReportType aReport) {
    return std::find(aReports.begin(), aReports.end(), aReport) != aReports.end();
}

/// An error of the form that blocks it, as the result builder decides it.
bool isFormError(const ProcessingDiagnostic& aDiagnostic, ReportType aReport) {
    return aDiagnostic.mSeverity == DiagnosticSeverity::Error &&
           affects(aDiagnostic.mContext.mAffectedReports, aReport);
}

/// An error that only this form has and that names a complete ISIN: missing history, a corporate
/// action, the FIFO or loss rule, a gate, or a rate of one payment. Leaving every row of that ISIN
/// out is then safe. The XML writer finds its errors only in the final document, so they never are.
bool belongsToOneInvestment(const ProcessingDiagnostic& aDiagnostic, ReportType aReport) {
    const auto& context = aDiagnostic.mContext;

    return context.mStage != PipelineStage::XmlGeneration && context.mIsin.has_value() &&
           context.mAffectedReports == std::vector<ReportType>{aReport};
}

/// The positions of the diagnostics that an open question of the form can resolve.
std::set<std::size_t> linkedToQuestion(const ApplicationResultInput& aInput, ReportType aReport) {
    std::set<std::size_t> linked;

    for (const auto& request : aInput.mRequests)
    {
        if (affects(request.mQuestion.mAffectedReports, aReport))
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
                          std::is_same_v<Answer, SeparateEventsAnswer> ||
                          std::is_same_v<Answer, CapitalGainsAnswer> ||
                          std::is_same_v<Answer, ExcludeFromReportsAnswer> ||
                          std::is_same_v<Answer, QuantityCorrectionAnswer> ||
                          std::is_same_v<Answer, PayerChoiceAnswer> ||
                          std::is_same_v<Answer, DepositInterestTotalAnswer> ||
                          std::is_same_v<Answer, IncomeTreatmentAnswer> ||
                          std::is_same_v<Answer, BondTypeAnswer> ||
                          std::is_same_v<Answer, TreatyClaimAnswer>);
            // The session accepts only a failure choice or a cancel for this question.
            throw std::logic_error("Only a failure choice or a cancel can answer this question.");
        }
    };

    return std::visit(choice, aAnswer);
}

/// Resolves the errors of the excluded ISINs and drops their open questions, as no answer to those
/// can change the report any more. A question whose linked error stays unresolved (it also
/// affects another form) stays open. A linked warning never keeps a question open: the payer choice
/// of a dividend or coupon is linked to the warning that its payer is not found.
void exclude(ApplicationResultInput& aInput, const std::set<Isin>& aExcluded, ReportType aReport) {
    const auto& diagnostics = aInput.mProcessingDiagnostics;

    for (std::size_t position = 0; position < diagnostics.size(); ++position)
    {
        const auto& diagnostic = diagnostics[position];

        if (isFormError(diagnostic, aReport) && belongsToOneInvestment(diagnostic, aReport) &&
            aExcluded.contains(diagnostic.mContext.mIsin.value()))
        {
            aInput.mResolvedDiagnostics.push_back(position);
        }
    }

    const auto isResolved = [&aInput](std::size_t aPosition) {
        return aInput.mProcessingDiagnostics.at(aPosition).mSeverity != DiagnosticSeverity::Error ||
               std::find(aInput.mResolvedDiagnostics.begin(),
                         aInput.mResolvedDiagnostics.end(),
                         aPosition) != aInput.mResolvedDiagnostics.end();
    };
    const auto isOfExcluded = [&aExcluded, &isResolved, aReport](const RequestInput& aRequest) {
        const auto& question = aRequest.mQuestion;

        return affects(question.mAffectedReports, aReport) && question.mIsin &&
               aExcluded.contains(*question.mIsin) &&
               std::all_of(aRequest.mProcessingDiagnostics.begin(),
                           aRequest.mProcessingDiagnostics.end(),
                           isResolved);
    };

    std::erase_if(aInput.mRequests, isOfExcluded);
}

} // namespace

std::vector<Isin> FailureResolutionStage::resolve(ApplicationResultInput& aInput,
                                                  RunDecisions& aDecisions,
                                                  const FailureForm& aForm) {
    const auto& diagnostics = aInput.mProcessingDiagnostics;
    const auto linked = linkedToQuestion(aInput, aForm.mReport);
    std::vector<std::size_t> listed;

    for (std::size_t position = 0; position < diagnostics.size(); ++position)
    {
        if (!isFormError(diagnostics[position], aForm.mReport) || linked.contains(position))
        {
            continue;
        }

        // An error that no investment owns cannot be solved by leaving investments out.
        if (!belongsToOneInvestment(diagnostics[position], aForm.mReport))
        {
            return {};
        }

        listed.push_back(position);
    }

    if (listed.empty() || hasSourceError(aInput, aForm.mReport))
    {
        return {};
    }

    DecisionQuestion question{
        .mType = aForm.mRequest,
        .mAffectedReports = {aForm.mReport},
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

    auto& report = aInput.mReports[static_cast<std::size_t>(aForm.mReport)];

    report.mFailureResolution = choice;

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
    report.mExcludedInvestments = investments;
    exclude(aInput, excluded, aForm.mReport);

    return {excluded.begin(), excluded.end()};
}

} // namespace taxbroker
