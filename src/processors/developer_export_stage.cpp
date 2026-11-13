#include "processors/developer_export_stage.hpp"

#include <utility>
#include <variant>

namespace taxbroker {
namespace {

/// The number of confirmations before the unsafe output.
constexpr int kConfirmationSteps = 3;

} // namespace

DeveloperConfirmation
DeveloperExportStage::confirm(ApplicationResultInput& aInput,
                              RunDecisions& aDecisions,
                              const std::vector<FailedInvestment>& aInvestments,
                              const std::vector<SourceReference>& aSources,
                              const std::vector<std::size_t>& aLinkedDiagnostics) {
    for (int step = 1; step <= kConfirmationSteps; ++step)
    {
        DecisionQuestion question{
            .mType = RequestType::DeveloperExportConfirmationRequired,
            .mAffectedReports = {ReportType::CapitalGains},
            .mSources = aSources,
            .mData = DeveloperExportQuestion{.mStep = step, .mInvestments = aInvestments},
        };

        const auto* answer = aDecisions.find(question);

        if (answer == nullptr)
        {
            aInput.mRequests.push_back(RequestInput{.mQuestion = std::move(question),
                                                    .mProcessingDiagnostics = aLinkedDiagnostics});

            return DeveloperConfirmation::Open;
        }

        // The session accepts only a confirmation or a cancel for this question.
        if (std::holds_alternative<CancelAnswer>(*answer))
        {
            return DeveloperConfirmation::Cancelled;
        }
    }

    return DeveloperConfirmation::Confirmed;
}

} // namespace taxbroker
