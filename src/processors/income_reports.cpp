#include "processors/income_reports.hpp"

#include "processors/failure_resolution_stage.hpp"
#include "processors/income_classifier.hpp"
#include "taxbroker/currency.hpp"

#include <optional>
#include <string>
#include <utility>

namespace taxbroker {

IncomeReports prepareIncomeReports(ApplicationResultInput& aInput,
                                   const MergedStatement& aStatement,
                                   const ReportingYearLedger& aLedger,
                                   const ReferenceCatalogue& aCatalogue,
                                   const OfficialRateTable& aRates,
                                   RunDecisions& aDecisions) {
    IncomeReports reports;
    auto classification =
        IncomeClassifier::classify(aStatement, aLedger, aInput.mMergeDiagnostics, aDecisions);
    // The stages read the statement the classification rewrote, when it rewrote one. It is taken
    // out before the result is moved, so the reference never points into a moved-from result.
    const std::optional<MergedStatement> rewritten = std::move(classification.mStatement);
    const MergedStatement& statement = rewritten ? *rewritten : aStatement;

    appendIncomeClassification(aInput, std::move(classification));

    auto dividends = prepareDividendPayments(statement, aLedger, aCatalogue, aRates, aDecisions);
    auto dividendRates = std::move(dividends.mUsedRates);

    reports.mDividends = std::move(dividends.mReport);
    appendDividendPayments(aInput, std::move(dividends));

    const auto excludedDividends =
        FailureResolutionStage::resolve(aInput, aDecisions, kDividendsFailure);

    excludeDividends(reports.mDividends, excludedDividends);

    // The notice is made after the exclusions, so it names only the rows that stay in the form.
    if (auto notice = foreignTaxEvidenceNotice(statement, reports.mDividends))
    {
        appendStageOutput(aInput, {std::move(*notice)}, {});
    }

    auto interest = prepareInterestPayments(statement, aLedger, aCatalogue, aRates, aDecisions);
    auto interestRates = std::move(interest.mUsedRates);

    reports.mDeposit = std::move(interest.mDepositReport);
    reports.mOther = std::move(interest.mOtherReport);

    if (interest.mDepositNoDataReason)
    {
        auto& deposit = aInput.mReports[static_cast<std::size_t>(ReportType::BankDepositInterest)];

        deposit.mNoDataReason = interest.mDepositNoDataReason;
    }

    appendInterestPayments(aInput, std::move(interest));

    const auto excludedOther =
        FailureResolutionStage::resolveUnits(aInput, aDecisions, kOtherInterestFailure);

    excludeCoupons(reports.mOther, excludedOther.mIsins);
    excludeBrokerCashInterest(reports.mOther, excludedOther.mBrokers);

    if (auto notice = foreignTaxEvidenceNotice(statement, reports.mOther))
    {
        appendStageOutput(aInput, {std::move(*notice)}, {});
    }

    reports.mUsedRates = mergedRates(std::move(dividendRates), std::move(interestRates));

    return reports;
}

} // namespace taxbroker
