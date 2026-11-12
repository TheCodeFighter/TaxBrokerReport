#include "processors/income_reports.hpp"

#include "processors/failure_resolution_stage.hpp"
#include "processors/income_classifier.hpp"
#include "taxbroker/currency.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace taxbroker {

namespace {

/// Adds EUR amounts; a total that does not fit is a defect of the input, never a wrong number.
Money addMoney(Money aLeft, Money aRight) {
    const auto sum = checkedAdd(aLeft, aRight);

    if (std::holds_alternative<NumericError>(sum))
    {
        throw std::overflow_error("An income total does not fit.");
    }

    return std::get<Money>(sum);
}

void setPayer(IncomeRowFacts& aRow, const IncomePayer& aPayer) {
    aRow.mPayerName = aPayer.mName;
    aRow.mPayerCountry = aPayer.mCountry;
    aRow.mPayerRow = aPayer.mCatalogueRow;
}

void setTreaty(IncomeRowFacts& aRow, const std::optional<TreatyClaim>& aClaim) {
    if (aClaim)
    {
        if (!aClaim->mStatement)
        {
            aRow.mTreaty = TreatyClaimKind::ClaimedWithoutText;
        }
        else if (aClaim->mCatalogueRow)
        {
            aRow.mTreaty = TreatyClaimKind::ReferenceText;
            aRow.mTreatyRow = aClaim->mCatalogueRow;
        }
        else
        {
            aRow.mTreaty = TreatyClaimKind::UserText;
        }
    }

    // The user attaches the evidence of a foreign tax and of a treaty claim in eDavki.
    aRow.mEvidenceRequired = aRow.mForeignTax > 0 || aClaim.has_value();
}

IncomeSummary totalsOf(std::vector<IncomeRowFacts> aRows) {
    IncomeSummary summary{.mRows = std::move(aRows)};

    for (const auto& row : summary.mRows)
    {
        summary.mGross = addMoney(summary.mGross, row.mGross);
        summary.mForeignTax = addMoney(summary.mForeignTax, row.mForeignTax);
    }

    return summary;
}

IncomeSummary summaryOf(const DividendReportDiv& aReport) {
    std::vector<IncomeRowFacts> rows;

    for (const auto& row : aReport.mRows)
    {
        IncomeRowFacts facts{.mKind = IncomeRowKind::Dividend,
                             .mIsin = row.mIsin,
                             .mDate = row.mDate,
                             .mGross = row.mGrossIncome,
                             .mForeignTax = row.mWithholding.mForeignTax,
                             .mTaxBlankInExport = row.mWithholding.mBlankInExport};

        setPayer(facts, row.mPayer);
        setTreaty(facts, row.mTreatyClaim);
        rows.push_back(std::move(facts));
    }

    return totalsOf(std::move(rows));
}

IncomeSummary summaryOf(const DepositInterestReportDHO& aReport) {
    std::vector<IncomeRowFacts> rows;

    for (const auto& row : aReport.mRows)
    {
        IncomeRowFacts facts{.mKind = IncomeRowKind::DepositInterest,
                             .mGross = row.mInterest,
                             .mForeignTax = row.mForeignTax};

        setPayer(facts, row.mPayer);
        setTreaty(facts, std::nullopt);
        rows.push_back(std::move(facts));
    }

    return totalsOf(std::move(rows));
}

IncomeSummary summaryOf(const OtherInterestReportObr& aReport) {
    std::vector<IncomeRowFacts> rows;

    for (const auto& row : aReport.mRows)
    {
        IncomeRowFacts facts{.mKind = IncomeRowKind::BrokerCashInterest,
                             .mDate = row.mDate,
                             .mGross = row.mGrossInterest,
                             .mForeignTax = row.mWithholding.mForeignTax,
                             .mTaxBlankInExport = row.mWithholding.mBlankInExport};

        if (const auto* coupon = std::get_if<BondCoupon>(&row.mKind))
        {
            facts.mKind = IncomeRowKind::BondCoupon;
            facts.mIsin = coupon->mIsin;
        }
        else
        {
            facts.mBroker = std::get<BrokerCashInterest>(row.mKind).mBroker;
        }

        setPayer(facts, row.mPayer);
        setTreaty(facts, row.mTreatyClaim);
        rows.push_back(std::move(facts));
    }

    return totalsOf(std::move(rows));
}

} // namespace

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

    if (interest.mQualifyingTotal)
    {
        const auto isReminder = [](const ProcessingDiagnostic& aDiagnostic) {
            return aDiagnostic.mCode == ProcessingDiagnosticCode::DepositInterestAddOtherBanks;
        };

        aInput.mDepositInterest = DepositInterestSummary{
            .mImportedQualifying = *interest.mQualifyingTotal,
            .mAddOtherBanks =
                std::any_of(interest.mDiagnostics.begin(), interest.mDiagnostics.end(), isReminder),
        };
    }

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

    aInput.mReports[static_cast<std::size_t>(ReportType::Dividends)].mIncome =
        summaryOf(reports.mDividends);
    aInput.mReports[static_cast<std::size_t>(ReportType::BankDepositInterest)].mIncome =
        summaryOf(reports.mDeposit);
    aInput.mReports[static_cast<std::size_t>(ReportType::OtherInterest)].mIncome =
        summaryOf(reports.mOther);

    reports.mUsedRates = mergedRates(std::move(dividendRates), std::move(interestRates));

    return reports;
}

} // namespace taxbroker
