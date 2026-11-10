#include "processors/income_classifier.hpp"

#include "processors/tax_classifier.hpp"
#include "taxbroker/diagnostic_messages.hpp"

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

/// What the user said about a payment.
enum class Outcome {
    Treatment, ///< A treatment from the question's options.
    Exclude,
    Cancel,
};

struct Decision {
    Outcome mOutcome{};           ///< Always set by the creator.
    IncomeTreatment mTreatment{}; ///< Only for Outcome::Treatment; always set by the creator.
};

/// The decision in an answer to an income classification question. The session accepts only these
/// answers for it.
Decision decisionOf(const AnswerValue& aAnswer) {
    const auto decide = [](const auto& aValue) -> Decision {
        using Answer = std::decay_t<decltype(aValue)>;

        if constexpr (std::is_same_v<Answer, IncomeTreatmentAnswer>)
        {
            return {Outcome::Treatment, aValue.mTreatment};
        }
        else if constexpr (std::is_same_v<Answer, ExcludeFromReportsAnswer>)
        {
            return {Outcome::Exclude, {}};
        }
        else if constexpr (std::is_same_v<Answer, CancelAnswer>)
        {
            return {Outcome::Cancel, {}};
        }
        else
        {
            static_assert(std::is_same_v<Answer, FailureResolutionAnswer> ||
                          std::is_same_v<Answer, NoOtherActivityAnswer> ||
                          std::is_same_v<Answer, OtherActivityAnswer> ||
                          std::is_same_v<Answer, SplitRatioAnswer> ||
                          std::is_same_v<Answer, SeparateEventsAnswer> ||
                          std::is_same_v<Answer, CapitalGainsAnswer> ||
                          std::is_same_v<Answer, QuantityCorrectionAnswer> ||
                          std::is_same_v<Answer, PayerChoiceAnswer> ||
                          std::is_same_v<Answer, DepositInterestTotalAnswer> ||
                          std::is_same_v<Answer, BondTypeAnswer> ||
                          std::is_same_v<Answer, TreatyClaimAnswer>);
            throw std::logic_error("Only an income treatment or a cancel can answer this.");
        }
    };

    return std::visit(decide, aAnswer);
}

/// A dividend or interest payment of the reporting year whose type is unknown. The transaction
/// is held in the interest form, which has every field of a dividend.
struct UnknownPayment {
    IncomeEventKind mKind{};   ///< Always set by the creator.
    std::size_t mInstrument{}; ///< Index of its instrument; always set by the creator.
    std::size_t mEvent{};      ///< Index in the instrument; always set by the creator.
    std::string mName;
    std::optional<Isin> mIsin;
    InterestTransaction mPayment;
};

bool isShareClass(AssetClass aAssetClass) {
    return aAssetClass == AssetClass::Stock || aAssetClass == AssetClass::Fund;
}

/// A dividend of an instrument with a row whose class is not shares or fund units.
bool hasUnknownClass(const DividendInstrument& aInstrument) {
    return std::any_of(aInstrument.mTransactions.begin(),
                       aInstrument.mTransactions.end(),
                       [](const DividendTransaction& aTransaction) {
                           return std::any_of(aTransaction.mInstrumentEvidence.begin(),
                                              aTransaction.mInstrumentEvidence.end(),
                                              [](const InstrumentSourceEvidence& aEvidence) {
                                                  return !isShareClass(aEvidence.mAssetClass);
                                              });
                       });
}

/// The payment in the interest form. aHolder is the cash holder the treatment gives.
template <typename Transaction>
InterestTransaction asInterest(const Transaction& aTransaction,
                               const std::optional<CashHolder> aHolder) {
    return InterestTransaction{
        .mMetadata = aTransaction.mMetadata,
        .mGrossAmount = aTransaction.mGrossAmount,
        .mTaxPaid = aTransaction.mTaxPaid,
        .mExchangeRate = aTransaction.mExchangeRate,
        .mCurrency = aTransaction.mCurrency,
        .mTaxCurrency = aTransaction.mTaxCurrency,
        .mInstrumentEvidence = aTransaction.mInstrumentEvidence,
        .mCashHolder = aHolder,
    };
}

DividendTransaction asDividend(const InterestTransaction& aTransaction) {
    return DividendTransaction{
        .mMetadata = aTransaction.mMetadata,
        .mGrossAmount = aTransaction.mGrossAmount,
        .mTaxPaid = aTransaction.mTaxPaid,
        .mExchangeRate = aTransaction.mExchangeRate,
        .mCurrency = aTransaction.mCurrency,
        .mTaxCurrency = aTransaction.mTaxCurrency,
        .mInstrumentEvidence = aTransaction.mInstrumentEvidence,
    };
}

/// Whether a share, fund or bond row is behind the payment: such an instrument has an ISIN, so a
/// missing one is worth a note. Interest on cash and other rows are expected to have none.
bool expectsIsin(const InterestTransaction& aPayment) {
    return std::any_of(aPayment.mInstrumentEvidence.begin(),
                       aPayment.mInstrumentEvidence.end(),
                       [](const InstrumentSourceEvidence& aEvidence) {
                           return isShareClass(aEvidence.mAssetClass) ||
                                  aEvidence.mAssetClass == AssetClass::Bond;
                       });
}

bool isInYear(const ReportingYearLedger& aLedger, Date aDate) {
    return aDate >= aLedger.mYearStart && aDate <= aLedger.mYearEnd;
}

/// The payments of the year that are interest of unknown type, and the dividends of an
/// instrument that is not shares or fund units, in instrument and event order.
std::vector<UnknownPayment> unknownPaymentsOf(const BrokerStatement& aStatement,
                                              const ReportingYearLedger& aLedger) {
    std::vector<UnknownPayment> payments;

    for (std::size_t instrumentIndex = 0; instrumentIndex < aStatement.mDividendInstruments.size();
         ++instrumentIndex)
    {
        const auto& instrument = aStatement.mDividendInstruments[instrumentIndex];

        if (!hasUnknownClass(instrument))
        {
            continue;
        }

        for (std::size_t index = 0; index < instrument.mTransactions.size(); ++index)
        {
            const auto& transaction = instrument.mTransactions[index];

            if (isInYear(aLedger, transaction.mMetadata.mTaxDate))
            {
                // A blank ISIN is no ISIN: the question then offers no dividend and no coupon.
                payments.push_back({.mKind = IncomeEventKind::Dividend,
                                    .mInstrument = instrumentIndex,
                                    .mEvent = index,
                                    .mName = instrument.mName,
                                    .mIsin = instrument.mIsin.empty()
                                                 ? std::nullopt
                                                 : std::optional<Isin>{instrument.mIsin},
                                    .mPayment = asInterest(transaction, std::nullopt)});
            }
        }
    }

    for (std::size_t instrumentIndex = 0; instrumentIndex < aStatement.mInterestInstruments.size();
         ++instrumentIndex)
    {
        const auto& instrument = aStatement.mInterestInstruments[instrumentIndex];

        if (instrument.mInterestType != InterestType::UnknownInterest)
        {
            continue;
        }

        for (std::size_t index = 0; index < instrument.mTransactions.size(); ++index)
        {
            const auto& transaction = instrument.mTransactions[index];

            if (isInYear(aLedger, transaction.mMetadata.mTaxDate))
            {
                payments.push_back({.mKind = IncomeEventKind::Interest,
                                    .mInstrument = instrumentIndex,
                                    .mEvent = index,
                                    .mName = instrument.mName,
                                    .mIsin = instrument.mIsin,
                                    .mPayment = transaction});
            }
        }
    }

    return payments;
}

/// The class label of each row of the payment as the file gave it, sorted, each once.
std::vector<std::string> classLabelsOf(const InterestTransaction& aPayment) {
    std::vector<std::string> labels;

    labels.reserve(aPayment.mInstrumentEvidence.size());
    std::transform(
        aPayment.mInstrumentEvidence.begin(),
        aPayment.mInstrumentEvidence.end(),
        std::back_inserter(labels),
        [](const InstrumentSourceEvidence& aEvidence) { return aEvidence.mAssetClassLabel; });

    std::sort(labels.begin(), labels.end());
    labels.erase(std::unique(labels.begin(), labels.end()), labels.end());

    return labels;
}

/// The forms a payment can belong to: Doh-Div needs an ISIN.
std::vector<ReportType> reportsOf(const UnknownPayment& aPayment) {
    if (aPayment.mIsin)
    {
        return {ReportType::Dividends, ReportType::BankDepositInterest, ReportType::OtherInterest};
    }

    return {ReportType::BankDepositInterest, ReportType::OtherInterest};
}

/// One diagnostic of the stage; it affects every form the payment can belong to.
ProcessingDiagnostic paymentDiagnostic(const UnknownPayment& aPayment,
                                       DiagnosticSeverity aSeverity,
                                       ProcessingDiagnosticCode aCode,
                                       DiagnosticMessage aMessage) {
    const auto& metadata = aPayment.mPayment.mMetadata;

    return ProcessingDiagnostic{
        .mSeverity = aSeverity,
        .mCode = aCode,
        .mContext =
            {
                .mStage = PipelineStage::FifoTax,
                .mAffectedReports = reportsOf(aPayment),
                .mSources = metadata.mSources,
                .mTaxDate = metadata.mTaxDate,
                .mInstrumentName = aPayment.mName,
                .mIsin = aPayment.mIsin,
            },
        .mMessage = std::move(aMessage.mEnglish),
        .mMessageKey = aMessage.mKey,
        .mMessageParams = std::move(aMessage.mParams),
    };
}

/// The positions of the parse errors about the interest type of the rows that became payments of
/// unknown type; this stage gives the blocking error instead.
std::vector<std::size_t>
takenOverInterestErrors(const BrokerStatement& aStatement,
                        const std::vector<StatementMergeDiagnostic>& aMergeDiagnostics) {
    std::set<std::pair<std::size_t, std::size_t>> rows;

    for (const auto& instrument : aStatement.mInterestInstruments)
    {
        if (instrument.mInterestType != InterestType::UnknownInterest)
        {
            continue;
        }

        for (const auto& transaction : instrument.mTransactions)
        {
            for (const auto& source : transaction.mMetadata.mSources)
            {
                rows.emplace(source.mInputSequence.mSourceIndex, source.mSourceRow);
            }
        }
    }

    std::vector<std::size_t> positions;

    for (std::size_t position = 0; position < aMergeDiagnostics.size(); ++position)
    {
        const auto* parse = std::get_if<SourcedParseDiagnostic>(&aMergeDiagnostics[position]);

        if (parse != nullptr &&
            parse->mDiagnostic.mMessageKey == MessageKey::UnsupportedAssetClassInterestMapping &&
            parse->mDiagnostic.mRowIndex &&
            rows.contains({parse->mSourceIndex, *parse->mDiagnostic.mRowIndex}))
        {
            positions.push_back(position);
        }
    }

    return positions;
}

/// What the statement changes by the answers: payments to drop from their instrument, and
/// payments to add to the instrument of their treatment.
struct Rewrite {
    std::set<std::pair<std::size_t, std::size_t>> mDropDividends; ///< (instrument, event)
    std::set<std::pair<std::size_t, std::size_t>> mDropInterest;  ///< (instrument, event)

    struct Addition {
        IncomeTreatment mTreatment{}; ///< Always set by the creator.
        std::string mName;
        std::optional<Isin> mIsin;
        InterestTransaction mPayment;
    };

    std::vector<Addition> mAdditions;
};

/// The cash holder an interest treatment gives to the payment.
std::optional<CashHolder> holderOf(IncomeTreatment aTreatment) {
    switch (aTreatment)
    {
    case IncomeTreatment::BankDepositInterest:
        return CashHolder::Bank;
    case IncomeTreatment::BrokerCashInterest:
        return CashHolder::Broker;
    case IncomeTreatment::Dividends:
    case IncomeTreatment::BondCoupon:
        break;
    }

    return std::nullopt;
}

DividendInstrument&
dividendInstrumentOf(BrokerStatement& aStatement, const Isin& aIsin, const std::string& aName) {
    auto& instruments = aStatement.mDividendInstruments;
    auto found = std::find_if(
        instruments.begin(),
        instruments.end(),
        [&aIsin](const DividendInstrument& aInstrument) { return aInstrument.mIsin == aIsin; });

    if (found == instruments.end())
    {
        instruments.push_back(DividendInstrument{.mName = aName, .mIsin = aIsin});
        found = std::prev(instruments.end());
    }

    return *found;
}

/// The interest instrument of a coupon (by ISIN) or of interest on cash (by name, no ISIN).
InterestInstrument& interestInstrumentOf(BrokerStatement& aStatement,
                                         InterestType aType,
                                         const std::optional<Isin>& aIsin,
                                         const std::string& aName) {
    auto& instruments = aStatement.mInterestInstruments;
    auto found = std::find_if(instruments.begin(),
                              instruments.end(),
                              [&](const InterestInstrument& aInstrument) {
                                  return aInstrument.mInterestType == aType &&
                                         aInstrument.mIsin == aIsin &&
                                         (aIsin || aInstrument.mName == aName);
                              });

    if (found == instruments.end())
    {
        instruments.push_back(
            InterestInstrument{.mName = aName, .mIsin = aIsin, .mInterestType = aType});
        found = std::prev(instruments.end());
    }

    return *found;
}

/// Removes the dropped payments and adds the moved ones, then builds the order again.
MergedStatement rewritten(const MergedStatement& aStatement, const Rewrite& aRewrite) {
    MergedStatement statement = aStatement;
    auto& presentation = statement.mPresentation;

    // Erase from the back so the earlier positions stay valid.
    for (auto position = aRewrite.mDropDividends.rbegin();
         position != aRewrite.mDropDividends.rend();
         ++position)
    {
        auto& transactions = presentation.mDividendInstruments[position->first].mTransactions;

        transactions.erase(transactions.begin() + static_cast<std::ptrdiff_t>(position->second));
    }

    for (auto position = aRewrite.mDropInterest.rbegin(); position != aRewrite.mDropInterest.rend();
         ++position)
    {
        auto& transactions = presentation.mInterestInstruments[position->first].mTransactions;

        transactions.erase(transactions.begin() + static_cast<std::ptrdiff_t>(position->second));
    }

    for (const auto& addition : aRewrite.mAdditions)
    {
        switch (addition.mTreatment)
        {
        case IncomeTreatment::Dividends:
            dividendInstrumentOf(presentation, addition.mIsin.value(), addition.mName)
                .mTransactions.push_back(asDividend(addition.mPayment));
            break;
        case IncomeTreatment::BondCoupon:
            interestInstrumentOf(presentation,
                                 InterestType::BondInterest,
                                 addition.mIsin,
                                 addition.mName)
                .mTransactions.push_back(addition.mPayment);
            break;
        case IncomeTreatment::BankDepositInterest:
        case IncomeTreatment::BrokerCashInterest:
            interestInstrumentOf(presentation,
                                 InterestType::BrokerInterest,
                                 std::nullopt,
                                 addition.mName)
                .mTransactions.push_back(addition.mPayment);
            break;
        }
    }

    // Moved payments keep the order of their dates inside the instrument they joined.
    const auto byDate = [](const auto& aLeft, const auto& aRight) {
        return ChronologicalEventOrder{}(aLeft.mMetadata, aRight.mMetadata);
    };

    for (auto& instrument : presentation.mDividendInstruments)
    {
        std::stable_sort(instrument.mTransactions.begin(), instrument.mTransactions.end(), byDate);
    }

    for (auto& instrument : presentation.mInterestInstruments)
    {
        std::stable_sort(instrument.mTransactions.begin(), instrument.mTransactions.end(), byDate);
    }

    rebuildChronologicalOrder(statement);

    return statement;
}

} // namespace

IncomeClassificationResult
IncomeClassifier::classify(const MergedStatement& aStatement,
                           const ReportingYearLedger& aLedger,
                           const std::vector<StatementMergeDiagnostic>& aMergeDiagnostics,
                           RunDecisions& aDecisions) {
    const auto& presentation = aStatement.mPresentation;
    IncomeClassificationResult result;

    result.mResolvedMergeDiagnostics = takenOverInterestErrors(presentation, aMergeDiagnostics);

    const auto payments = unknownPaymentsOf(presentation, aLedger);

    if (payments.empty())
    {
        return result;
    }

    Rewrite rewrite;

    for (const auto& payment : payments)
    {
        const auto& metadata = payment.mPayment.mMetadata;
        const auto position = result.mDiagnostics.size();
        DecisionQuestion question{
            .mType = RequestType::TaxClassificationRequired,
            .mAffectedReports = reportsOf(payment),
            .mSources = metadata.mSources,
            .mTaxDate = metadata.mTaxDate,
            .mIsin = payment.mIsin,
            .mData = IncomeClassificationQuestion{.mEventKind = payment.mKind,
                                                  .mTransaction = transactionIdentity(metadata),
                                                  .mDate = metadata.mTaxDate,
                                                  .mName = payment.mName,
                                                  .mIsin = payment.mIsin,
                                                  .mClassLabels = classLabelsOf(payment.mPayment),
                                                  .mGrossAmount = payment.mPayment.mGrossAmount,
                                                  .mCurrency = payment.mPayment.mCurrency},
        };
        const auto* answer = aDecisions.find(question);

        // Whatever the answer, the payment leaves its instrument; a treatment adds it again below.
        auto& drop = payment.mKind == IncomeEventKind::Dividend ? rewrite.mDropDividends
                                                                : rewrite.mDropInterest;

        if (answer == nullptr)
        {
            // Payments with the same facts are one question: they share the request.
            const auto& data = std::get<IncomeClassificationQuestion>(question.mData);
            auto request = std::find_if(result.mRequests.begin(),
                                        result.mRequests.end(),
                                        [&data](const RequestInput& aRequest) {
                                            const auto* other =
                                                std::get_if<IncomeClassificationQuestion>(
                                                    &aRequest.mQuestion.mData);

                                            return other != nullptr && *other == data;
                                        });

            if (request == result.mRequests.end())
            {
                result.mRequests.push_back(RequestInput{.mQuestion = std::move(question)});
                request = std::prev(result.mRequests.end());
            }
            else
            {
                request->mQuestion.mSources.insert(request->mQuestion.mSources.end(),
                                                   metadata.mSources.begin(),
                                                   metadata.mSources.end());
            }

            request->mProcessingDiagnostics.push_back(position);
            result.mDiagnostics.push_back(paymentDiagnostic(
                payment,
                DiagnosticSeverity::Error,
                ProcessingDiagnosticCode::TaxClassificationUnknown,
                makeDiagnosticMessage(
                    MessageKey::TaxClassificationUnknownIncomeConfirmationRequired)));
            drop.emplace(payment.mInstrument, payment.mEvent);

            continue;
        }

        const auto decision = decisionOf(*answer);
        InterestTransaction moved;

        switch (decision.mOutcome)
        {
        case Outcome::Cancel:
            result.mDiagnostics.push_back(paymentDiagnostic(
                payment,
                DiagnosticSeverity::Error,
                ProcessingDiagnosticCode::TaxClassificationUnknown,
                makeDiagnosticMessage(MessageKey::TaxClassificationUnknownIncomeCancelled)));
            drop.emplace(payment.mInstrument, payment.mEvent);
            break;
        case Outcome::Exclude: {
            const bool isIsinMissing = !payment.mIsin && expectsIsin(payment.mPayment);

            result.mDiagnostics.push_back(paymentDiagnostic(
                payment,
                DiagnosticSeverity::Warning,
                ProcessingDiagnosticCode::TaxCategoryNotReported,
                makeDiagnosticMessage(
                    isIsinMissing ? MessageKey::TaxCategoryNotReportedUserChoiceIncomeMissingIsin
                                  : MessageKey::TaxCategoryNotReportedUserChoiceIncome)));
            result.mExcludedItems.push_back(
                ExcludedItemInput{.mItem = {.mIsin = payment.mIsin.value_or(Isin{}),
                                            .mName = payment.mName,
                                            .mCategory = ExcludedCategory::Unknown,
                                            .mReason = ExcludedReason::UserChoice,
                                            .mClassLabels = classLabelsOf(payment.mPayment)},
                                  .mSources = metadata.mSources,
                                  .mProcessingDiagnostics = {position}});
            drop.emplace(payment.mInstrument, payment.mEvent);
            break;
        }
        case Outcome::Treatment:
            // A dividend answered as a dividend stays where it is.
            if (payment.mKind == IncomeEventKind::Dividend &&
                decision.mTreatment == IncomeTreatment::Dividends)
            {
                break;
            }

            drop.emplace(payment.mInstrument, payment.mEvent);
            moved = payment.mPayment;
            moved.mCashHolder = holderOf(decision.mTreatment);
            rewrite.mAdditions.push_back({.mTreatment = decision.mTreatment,
                                          .mName = payment.mName,
                                          .mIsin = payment.mIsin,
                                          .mPayment = std::move(moved)});
            break;
        }
    }

    result.mStatement = rewritten(aStatement, rewrite);

    return result;
}

void appendIncomeClassification(ApplicationResultInput& aInput,
                                IncomeClassificationResult&& aResult) {
    TaxClassificationResult stage;

    stage.mDiagnostics = std::move(aResult.mDiagnostics);
    stage.mRequests = std::move(aResult.mRequests);
    stage.mExcludedItems = std::move(aResult.mExcludedItems);
    stage.mResolvedMergeDiagnostics = std::move(aResult.mResolvedMergeDiagnostics);

    appendClassification(aInput, std::move(stage));
}

} // namespace taxbroker
