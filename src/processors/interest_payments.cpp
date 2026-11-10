#include "processors/interest_payments.hpp"

#include "processors/bond_years.hpp"
#include "processors/income_payers.hpp"
#include "taxbroker/diagnostic_messages.hpp"
#include "utils/xml_text.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>

namespace taxbroker {
namespace {

/// The smallest EUR amount that rounds to 0.01: half a cent.
constexpr Money kHalfCent = MONEY_SCALE / 200;

/// The residence country of a Slovenian payer.
constexpr std::string_view kSlovenia = "SI";

// Limits of the Doh-Obr schema that a row must meet. The XML writer has its own copy as a guard
// (core and XML are separate targets).
constexpr std::size_t kMaximumIdentificationLength = 30;
/// EUR 999999999999.9949: the largest amount that rounds to the largest value of 12 digits and 2
/// decimals, 999999999999.99.
constexpr Money kLargestObrAmount = 9'999'999'999'999'949;

/// The coupon form's share of the payer helpers. A Slovenian issuer is not left out: its listed
/// bonds are not subject to the withholding of the payer, so the user reports the coupons.
const PayerForm kCouponForm{
    .mReport = ReportType::OtherInterest,
    .mNotFoundCode = ProcessingDiagnosticCode::InterestPayerNotFound,
    .mNotFoundKey = MessageKey::InterestPayerNotFound,
    .mSlovenianExclusion = std::nullopt,
};

/// Where a payment goes.
enum class Route {
    Deposit, ///< Bank-deposit interest: Doh-DHO, under the allowance.
    Cash,    ///< Interest on the cash a broker holds: Doh-Obr.
    Coupon,  ///< An ordinary bond coupon: Doh-Obr.
};

/// One interest payment of the reporting year with its place in the statement.
struct Payment {
    StatementEventReference mReference;
    const InterestInstrument* mInstrument{};
    const InterestTransaction* mTransaction{};
    Route mRoute{};
};

/// What the deposit payments of one broker add up to.
struct DepositGroup {
    std::vector<StatementEventReference> mPayments;
    Money mInterest{};
    Money mForeignTax{};
};

ReportType reportOf(Route aRoute) {
    return aRoute == Route::Deposit ? ReportType::BankDepositInterest : ReportType::OtherInterest;
}

Broker brokerOf(const Payment& aPayment) {
    return primarySource(aPayment.mTransaction->mMetadata).mBroker;
}

/// What a person sees for the payment: the instrument's name, or the broker for interest on cash.
std::string nameOf(const Payment& aPayment) {
    const auto& name = aPayment.mInstrument->mName;

    return name.empty() ? std::string{brokerName(brokerOf(aPayment))} : name;
}

/// The route of a payment by who holds the cash, or none for an interest type nothing handles yet.
/// A payment on cash without a holder is a parser bug.
std::optional<Route> routeOf(const InterestInstrument& aInstrument,
                             const InterestTransaction& aTransaction) {
    switch (aInstrument.mInterestType)
    {
    case InterestType::BondInterest:
        return Route::Coupon;
    case InterestType::BrokerInterest:
        break;
    case InterestType::OtherInterest:
    case InterestType::UnknownInterest:
        return std::nullopt;
    }

    if (!aTransaction.mCashHolder)
    {
        throw std::logic_error("A cash interest payment needs its cash holder.");
    }

    switch (*aTransaction.mCashHolder)
    {
    case CashHolder::Bank:
        return Route::Deposit;
    case CashHolder::Broker:
        return Route::Cash;
    }

    throw std::logic_error("Unknown cash holder.");
}

/// The interest dated inside the reporting year, in ledger order.
std::vector<Payment> paymentsOf(const MergedStatement& aStatement,
                                const ReportingYearLedger& aLedger) {
    const auto& instruments = aStatement.mPresentation.mInterestInstruments;
    std::vector<Payment> payments;

    for (const auto& reference : aStatement.mChronologicalOrder)
    {
        if (reference.mKind != StatementEventKind::Interest)
        {
            continue;
        }

        const auto& instrument = instruments.at(reference.mInstrumentIndex.value());
        const auto& transaction = instrument.mTransactions.at(reference.mEventIndex);
        const auto date = transaction.mMetadata.mTaxDate;
        const auto route = routeOf(instrument, transaction);

        if (date < aLedger.mYearStart || date > aLedger.mYearEnd || !route)
        {
            continue;
        }

        payments.push_back({.mReference = reference,
                            .mInstrument = &instrument,
                            .mTransaction = &transaction,
                            .mRoute = *route});
    }

    return payments;
}

/// The warning that a coupon of a Slovenian issuer has tax: a Slovenian withholding is no foreign
/// tax, and a coupon the payer already taxed is not reported.
ProcessingDiagnostic slovenianCouponTaxWarning(const Payment& aPayment) {
    const auto& metadata = aPayment.mTransaction->mMetadata;

    return incomeDiagnostic(ReportType::OtherInterest,
                            DiagnosticSeverity::Warning,
                            ProcessingDiagnosticCode::SlovenianCouponTax,
                            makeDiagnosticMessage(MessageKey::SlovenianCouponTax),
                            metadata.mSources,
                            metadata.mTaxDate,
                            nameOf(aPayment),
                            aPayment.mInstrument->mIsin);
}

/// The warning that a payment is not reported because it is not above 0.
ProcessingDiagnostic notPositiveWarning(const Payment& aPayment) {
    const auto& metadata = aPayment.mTransaction->mMetadata;

    return incomeDiagnostic(reportOf(aPayment.mRoute),
                            DiagnosticSeverity::Warning,
                            ProcessingDiagnosticCode::InterestPaymentNotPositive,
                            makeDiagnosticMessage(MessageKey::InterestPaymentNotPositive),
                            metadata.mSources,
                            metadata.mTaxDate,
                            nameOf(aPayment),
                            aPayment.mInstrument->mIsin);
}

/// The payments of the year that are above 0, with a warning for each one that is not.
std::vector<Payment> positivePayments(std::vector<Payment>&& aPayments,
                                      std::vector<ProcessingDiagnostic>& aDiagnostics) {
    std::vector<Payment> positive;

    positive.reserve(aPayments.size());

    for (auto& payment : aPayments)
    {
        if (payment.mTransaction->mGrossAmount > 0)
        {
            positive.push_back(std::move(payment));
        }
        else
        {
            aDiagnostics.push_back(notPositiveWarning(payment));
        }
    }

    return positive;
}

/// The name, sources and first date of the year's coupons of each ISIN.
std::map<Isin, IsinPayments> couponsByIsin(const std::vector<Payment>& aPayments) {
    std::map<Isin, IsinPayments> groups;

    for (const auto& payment : aPayments)
    {
        if (payment.mRoute != Route::Coupon)
        {
            continue;
        }

        const auto& metadata = payment.mTransaction->mMetadata;
        auto [position, inserted] = groups.try_emplace(payment.mInstrument->mIsin.value());
        auto& group = position->second;

        if (inserted)
        {
            group.mName = nameOf(payment);
            group.mFirstDate = metadata.mTaxDate;
        }

        group.mSources.insert(group.mSources.end(),
                              metadata.mSources.begin(),
                              metadata.mSources.end());
        group.mFirstDate = std::min(group.mFirstDate, metadata.mTaxDate);
    }

    return groups;
}

/// The payer of the cash a broker holds: its only broker payer row, or none with one warning
/// (for the forms of its payments) when the reference files have no row or several.
std::map<Broker, PayerOutcome> brokerPayers(const std::vector<Payment>& aPayments,
                                            const ReferenceCatalogue& aCatalogue,
                                            std::vector<ProcessingDiagnostic>& aDiagnostics) {
    std::map<Broker, std::vector<const Payment*>> byBroker;
    std::map<Broker, PayerOutcome> payers;

    for (const auto& payment : aPayments)
    {
        if (payment.mRoute != Route::Coupon)
        {
            byBroker[brokerOf(payment)].push_back(&payment);
        }
    }

    for (const auto& [broker, payments] : byBroker)
    {
        const auto entries = aCatalogue.brokerPayersOf(broker);

        if (entries.size() == 1)
        {
            payers.emplace(broker, payerOutcomeOf(*entries.front()));

            continue;
        }

        std::set<ReportType> reports;
        std::vector<SourceReference> sources;

        for (const auto* payment : payments)
        {
            const auto& metadata = payment->mTransaction->mMetadata;

            reports.insert(reportOf(payment->mRoute));
            sources.insert(sources.end(), metadata.mSources.begin(), metadata.mSources.end());
        }

        const auto& first = *payments.front()->mTransaction;
        auto warning = incomeDiagnostic(*reports.begin(),
                                        DiagnosticSeverity::Warning,
                                        ProcessingDiagnosticCode::InterestPayerNotFound,
                                        makeDiagnosticMessage(MessageKey::InterestPayerNotFound),
                                        std::move(sources),
                                        first.mMetadata.mTaxDate,
                                        nameOf(*payments.front()),
                                        std::nullopt);

        warning.mContext.mAffectedReports.assign(reports.begin(), reports.end());
        aDiagnostics.push_back(std::move(warning));
        payers.emplace(broker, PayerOutcome{});
    }

    return payers;
}

/// The rate needs of the payments that are reported.
std::vector<RateNeed> needsOf(const std::vector<Payment>& aPayments,
                              const std::set<Isin>& aSkippedCoupons) {
    std::vector<RateNeed> needs;

    for (const auto& payment : aPayments)
    {
        const auto& transaction = *payment.mTransaction;

        if (payment.mRoute == Route::Coupon &&
            aSkippedCoupons.contains(payment.mInstrument->mIsin.value()))
        {
            continue;
        }

        addIncomeRateNeeds(needs,
                           reportOf(payment.mRoute),
                           transaction.mMetadata,
                           transaction.mCurrency,
                           transaction.mTaxPaid,
                           transaction.mTaxCurrency);
    }

    return needs;
}

/// The EUR gross and foreign tax of a payment.
struct Amounts {
    Money mGross{};
    Money mTax{};
};

/// The EUR gross and foreign tax of a payment, or none when one cannot be converted: a missing
/// rate or an amount that does not fit gives an error of this payment.
std::optional<Amounts> convert(IncomeEurConverter& aConverter,
                               const OfficialRateSelection& aRates,
                               const Payment& aPayment,
                               std::vector<ProcessingDiagnostic>& aDiagnostics) {
    if (addPaymentMissingRateErrors(aRates,
                                    reportOf(aPayment.mRoute),
                                    aPayment.mTransaction->mMetadata,
                                    nameOf(aPayment),
                                    aPayment.mInstrument->mIsin,
                                    aPayment.mTransaction->mCurrency,
                                    aPayment.mTransaction->mTaxPaid,
                                    aPayment.mTransaction->mTaxCurrency,
                                    aDiagnostics))
    {
        return std::nullopt;
    }

    const auto& transaction = *aPayment.mTransaction;
    const auto& metadata = transaction.mMetadata;
    const auto name = nameOf(aPayment);
    const auto report = reportOf(aPayment.mRoute);
    const auto gross = aConverter.toEur(report,
                                        metadata,
                                        name,
                                        aPayment.mInstrument->mIsin,
                                        transaction.mGrossAmount,
                                        transaction.mCurrency,
                                        "amount");
    std::optional<Money> tax = Money{0};

    if (transaction.mTaxPaid && *transaction.mTaxPaid != 0)
    {
        tax = aConverter.toEur(report,
                               metadata,
                               name,
                               aPayment.mInstrument->mIsin,
                               *transaction.mTaxPaid,
                               transaction.mTaxCurrency,
                               "tax");
    }

    if (!gross || !tax)
    {
        return std::nullopt;
    }

    return Amounts{.mGross = *gross, .mTax = *tax};
}

/// The Doh-Obr row of a cash interest payment or a coupon.
ObrInterestRow
obrRow(const Payment& aPayment, const Amounts& aAmounts, const PayerOutcome& aPayer) {
    const auto& transaction = *aPayment.mTransaction;
    std::variant<BrokerCashInterest, BondCoupon> kind;

    if (aPayment.mRoute == Route::Coupon)
    {
        kind = BondCoupon{.mIsin = aPayment.mInstrument->mIsin.value()};
    }
    else
    {
        kind = BrokerCashInterest{.mBroker = brokerOf(aPayment)};
    }

    return ObrInterestRow{
        .mPayment = aPayment.mReference,
        .mKind = kind,
        .mDate = transaction.mMetadata.mTaxDate,
        .mPayer = aPayer.mPayer,
        .mGrossInterest = aAmounts.mGross,
        .mWithholding = {.mForeignTax = aAmounts.mTax, .mBlankInExport = !transaction.mTaxPaid},
        .mSourceCountry = aPayer.mPayer.mCountry,
        .mTreatyClaim = std::nullopt,
    };
}

/// The error that a value of a Doh-Obr row does not fit the form. It is found here, before the
/// failure question, so the user can leave out the coupon's ISIN or the broker's cash interest.
ProcessingDiagnostic obrValueError(const Payment& aPayment, std::string_view aField) {
    const auto& metadata = aPayment.mTransaction->mMetadata;
    auto diagnostic =
        incomeDiagnostic(ReportType::OtherInterest,
                         DiagnosticSeverity::Error,
                         ProcessingDiagnosticCode::XmlValueOutOfRange,
                         makeDiagnosticMessage(MessageKey::XmlValueOutOfRange,
                                               {textParam("field", std::string{aField})}),
                         metadata.mSources,
                         metadata.mTaxDate,
                         nameOf(aPayment),
                         aPayment.mInstrument->mIsin);

    diagnostic.mContext.mField = std::string{aField};

    return diagnostic;
}

/// Text that XML accepts and that is not empty, within a limit when one is given.
bool isUsableText(const std::string& aText, std::optional<std::size_t> aMaximumLength) {
    const auto length = xmlTextLength(aText);

    return length && *length > 0 && (!aMaximumLength || *length <= *aMaximumLength);
}

/// The fields of a Doh-Obr row that the form cannot hold, in form order. The date, the countries
/// and the treaty claim (set only after the rows are made) cannot fail here, and a value that
/// rounds to 0.00 is no row. The foreign tax of a Slovenian payer is not written, so its value does
/// not matter.
std::vector<std::string_view> obrFieldProblems(const ObrInterestRow& aRow) {
    const auto& payer = aRow.mPayer;
    std::vector<std::string_view> problems;

    if (payer.mIdentificationNumber &&
        !isUsableText(*payer.mIdentificationNumber, kMaximumIdentificationLength))
    {
        problems.push_back("IdentificationNumber");
    }

    if (payer.mName && !isUsableText(*payer.mName, std::nullopt))
    {
        problems.push_back("Name");
    }

    if (payer.mAddress && !isUsableText(*payer.mAddress, std::nullopt))
    {
        problems.push_back("Address");
    }

    if (aRow.mGrossInterest > kLargestObrAmount)
    {
        problems.push_back("Value");
    }

    const auto& tax = aRow.mWithholding.mForeignTax;

    if (payer.mCountry != kSlovenia && (tax < 0 || tax > kLargestObrAmount))
    {
        problems.push_back("ForeignTax");
    }

    return problems;
}

/// Names the broker on every error added since `aFirst` for a cash-interest payment, so that the
/// failure question can offer to leave that broker's cash interest out.
void markCashInterestBroker(std::vector<ProcessingDiagnostic>& aDiagnostics,
                            std::size_t aFirst,
                            const Payment& aPayment) {
    if (aPayment.mRoute != Route::Cash)
    {
        return;
    }

    for (auto position = aFirst; position < aDiagnostics.size(); ++position)
    {
        if (aDiagnostics[position].mSeverity == DiagnosticSeverity::Error)
        {
            aDiagnostics[position].mContext.mCashInterestBroker = brokerOf(aPayment);
        }
    }
}

/// Adds aValue to aSum; on an overflow adds the error to the diagnostics and returns false.
bool addChecked(Money& aSum,
                Money aValue,
                const Payment& aPayment,
                std::vector<ProcessingDiagnostic>& aDiagnostics) {
    const auto sum = checkedAdd(aSum, aValue);

    if (const auto* error = std::get_if<NumericError>(&sum))
    {
        const auto& metadata = aPayment.mTransaction->mMetadata;

        aDiagnostics.push_back(
            numericDiagnostic(*error,
                              {.mStage = PipelineStage::FifoTax,
                               .mAffectedReports = {ReportType::BankDepositInterest},
                               .mSources = metadata.mSources,
                               .mTaxDate = metadata.mTaxDate,
                               .mField = "amount",
                               .mInstrumentName = nameOf(aPayment),
                               .mIsin = std::nullopt}));

        return false;
    }

    aSum = std::get<std::int64_t>(sum);

    return true;
}

/// The Doh-DHO rows, one per broker (its payer), sorted by payer name and country.
std::vector<DhoInterestRow> depositRows(const std::map<Broker, DepositGroup>& aGroups,
                                        const std::map<Broker, PayerOutcome>& aPayers) {
    std::vector<DhoInterestRow> rows;

    rows.reserve(aGroups.size());

    for (const auto& [broker, group] : aGroups)
    {
        const auto& payer = aPayers.at(broker).mPayer;

        rows.push_back(DhoInterestRow{.mPayments = group.mPayments,
                                      .mPayer = payer,
                                      .mKind = DepositInterestKind::BankDeposit,
                                      .mInterest = group.mInterest,
                                      .mForeignTax = group.mForeignTax,
                                      .mSourceCountry = payer.mCountry});
    }

    std::stable_sort(rows.begin(),
                     rows.end(),
                     [](const DhoInterestRow& aLeft, const DhoInterestRow& aRight) {
                         return std::tie(aLeft.mPayer.mName, aLeft.mSourceCountry) <
                                std::tie(aRight.mPayer.mName, aRight.mSourceCountry);
                     });

    return rows;
}

/// Decides, by the allowance, whether the deposit rows are made, and asks when only the other
/// banks can make them filing.
void applyAllowance(InterestPaymentsResult& aResult,
                    const std::map<Broker, DepositGroup>& aGroups,
                    const std::map<Broker, PayerOutcome>& aPayers,
                    const std::vector<Payment>& aPayments,
                    int aYear,
                    RunDecisions& aDecisions) {
    const auto total = *aResult.mQualifyingTotal;

    if (total > kDepositInterestAllowance)
    {
        aResult.mDepositReport.mRows = depositRows(aGroups, aPayers);

        return;
    }

    std::vector<SourceReference> sources;
    std::optional<Date> firstDate;

    for (const auto& payment : aPayments)
    {
        if (payment.mRoute != Route::Deposit)
        {
            continue;
        }

        const auto& metadata = payment.mTransaction->mMetadata;

        sources.insert(sources.end(), metadata.mSources.begin(), metadata.mSources.end());
        firstDate = firstDate ? std::min(*firstDate, metadata.mTaxDate) : metadata.mTaxDate;
    }

    DecisionQuestion question{
        .mType = RequestType::DepositInterestTotalRequired,
        .mAffectedReports = {ReportType::BankDepositInterest},
        .mSources = std::move(sources),
        .mTaxDate = firstDate,
        .mIsin = std::nullopt,
        .mData = DepositInterestTotalQuestion{.mYear = aYear, .mTotal = total},
    };
    const auto* answer = aDecisions.find(question);

    if (answer == nullptr)
    {
        aResult.mRequests.push_back(
            RequestInput{.mQuestion = std::move(question), .mProcessingDiagnostics = {}});

        return;
    }

    // The session accepts only a yes or no answer for this question.
    if (!std::get<DepositInterestTotalAnswer>(*answer).mAboveLimit)
    {
        aResult.mDepositNoDataReason = NoDataReason::BelowDepositInterestThreshold;

        return;
    }

    aResult.mDepositReport.mRows = depositRows(aGroups, aPayers);
    aResult.mDiagnostics.push_back(
        incomeDiagnostic(ReportType::BankDepositInterest,
                         DiagnosticSeverity::Warning,
                         ProcessingDiagnosticCode::DepositInterestAddOtherBanks,
                         makeDiagnosticMessage(MessageKey::DepositInterestAddOtherBanks),
                         question.mSources,
                         question.mTaxDate,
                         std::nullopt,
                         std::nullopt));
}

/// The error that the income of a bond cannot go into Doh-Obr: its type is a discount or
/// zero-coupon bond, whose interest nothing calculates yet, or its type was not given.
ProcessingDiagnostic bondTypeError(const Isin& aIsin, const BondYear& aBond, MessageKey aKey) {
    return incomeDiagnostic(ReportType::OtherInterest,
                            DiagnosticSeverity::Error,
                            ProcessingDiagnosticCode::UnsupportedTaxTreatment,
                            makeDiagnosticMessage(aKey),
                            aBond.mSources,
                            aBond.mFirstDate,
                            aBond.mName,
                            aIsin);
}

/// Asks the bond type of each bond with a sale or maturity in the year, whatever its issuer. A bond
/// whose units overflow is an error of the bond. Returns the ISINs whose coupons stay out of
/// Doh-Obr: the answer is open, is a discount or zero-coupon bond (an error), or was cancelled (an
/// error).
std::set<Isin> askBondTypes(const MergedStatement& aStatement,
                            const ReportingYearLedger& aLedger,
                            RunDecisions& aDecisions,
                            InterestPaymentsResult& aResult) {
    std::set<Isin> heldBack;

    for (const auto& [isin, bond] : bondYearsOf(aStatement, aLedger))
    {
        const auto& units = bond.mUnits;

        // Units that do not add up cannot tell whether the bond was sold or matured, so its coupons
        // stay out of Doh-Obr with an error of the bond.
        if (bond.mOverflow)
        {
            aResult.mDiagnostics.push_back(
                numericDiagnostic(NumericError::Overflow,
                                  {.mStage = PipelineStage::FifoTax,
                                   .mAffectedReports = {ReportType::OtherInterest},
                                   .mSources = bond.mSources,
                                   .mTaxDate = bond.mFirstDate,
                                   .mField = "bond_units",
                                   .mInstrumentName = bond.mName,
                                   .mIsin = isin}));
            heldBack.insert(isin);

            continue;
        }

        // A bond only bought has no sale or maturity to ask about.
        if (units.mSoldUnits <= 0 && units.mMaturedUnits <= 0)
        {
            continue;
        }

        DecisionQuestion question{
            .mType = RequestType::IncomeDetailsRequired,
            .mAffectedReports = {ReportType::OtherInterest},
            .mSources = bond.mSources,
            .mTaxDate = bond.mFirstDate,
            .mIsin = isin,
            .mData = BondTypeQuestion{.mIsin = isin,
                                      .mName = bond.mName,
                                      .mSoldUnits = units.mSoldUnits,
                                      .mMaturedUnits = units.mMaturedUnits,
                                      .mMaturityDate = units.mMaturityDate},
        };
        const auto* answer = aDecisions.find(question);

        if (answer == nullptr)
        {
            heldBack.insert(isin);
            aResult.mRequests.push_back(
                RequestInput{.mQuestion = std::move(question), .mProcessingDiagnostics = {}});

            continue;
        }

        if (const auto* typed = std::get_if<BondTypeAnswer>(answer))
        {
            if (typed->mType == BondType::Ordinary)
            {
                continue;
            }

            aResult.mDiagnostics.push_back(
                bondTypeError(isin, bond, MessageKey::UnsupportedTaxTreatmentDiscountBond));
        }
        else
        {
            // The session accepts only a bond type or a cancel for this question.
            aResult.mDiagnostics.push_back(
                bondTypeError(isin, bond, MessageKey::UnsupportedTaxTreatmentBondTypeNotGiven));
        }

        heldBack.insert(isin);
    }

    return heldBack;
}

/// One Doh-Obr row with foreign tax, for its treaty claim.
struct TreatyRow {
    std::size_t mIndex{};                  ///< Position in the report rows.
    TreatyIncome mIncome{};                ///< Coupon or cash interest; always set by the creator.
    std::optional<Isin> mIsin;             ///< The ISIN of a coupon; empty for cash interest.
    std::vector<SourceReference> mSources; ///< The sources of the payment.
    Date mTaxDate;                         ///< The receipt date.
};

/// The Doh-Obr rows of one payer country that had foreign tax, in report order.
using TreatyGroup = std::vector<TreatyRow>;

/// Adds the payment's row to the treaty group of its payer country. A row of a Slovenian payer has
/// no foreign tax in the file, and a row without a payer country has no question.
void addToTreatyGroup(std::map<std::string, TreatyGroup>& aGroups,
                      const Payment& aPayment,
                      const ObrInterestRow& aRow,
                      std::size_t aRowIndex) {
    if (aRow.mWithholding.mForeignTax <= 0 || !aRow.mPayer.mCountry ||
        *aRow.mPayer.mCountry == kSlovenia)
    {
        return;
    }

    const auto& metadata = aPayment.mTransaction->mMetadata;
    const bool isCoupon = aPayment.mRoute == Route::Coupon;

    aGroups[*aRow.mPayer.mCountry].push_back(TreatyRow{
        .mIndex = aRowIndex,
        .mIncome = isCoupon ? TreatyIncome::BondCoupon : TreatyIncome::CashInterest,
        .mIsin = isCoupon ? std::optional<Isin>{aPayment.mInstrument->mIsin.value()} : std::nullopt,
        .mSources = metadata.mSources,
        .mTaxDate = metadata.mTaxDate,
    });
}

/// Claims, for each payer country with foreign tax, the rows of a kind of income whose reference
/// rows give one treaty text, without a question. The rows of the other kinds are asked once per
/// country: a yes marks them as claimed without a text and adds one warning (the paragraph and
/// article are filled in eDavki); no answer or a no leaves them without a claim.
void applyTreatyClaims(InterestPaymentsResult& aResult,
                       const std::map<std::string, TreatyGroup>& aGroups,
                       int aYear,
                       const ReferenceCatalogue& aCatalogue,
                       RunDecisions& aDecisions) {
    for (const auto& [country, rows] : aGroups)
    {
        const auto couponText = treatyTextOf(aCatalogue, country, TreatyIncome::BondCoupon);
        const auto cashText = treatyTextOf(aCatalogue, country, TreatyIncome::CashInterest);
        std::vector<const TreatyRow*> asked;

        for (const auto& row : rows)
        {
            const auto& text = row.mIncome == TreatyIncome::BondCoupon ? couponText : cashText;

            if (text)
            {
                aResult.mOtherReport.mRows[row.mIndex].mTreatyClaim =
                    TreatyClaim{.mStatement = text->first, .mCatalogueRow = text->second};
            }
            else
            {
                asked.push_back(&row);
            }
        }

        if (asked.empty())
        {
            continue;
        }

        std::vector<Isin> isins;
        std::vector<SourceReference> sources;
        bool cashInterest = false;
        Date firstDate = asked.front()->mTaxDate;

        for (const auto* row : asked)
        {
            firstDate = std::min(firstDate, row->mTaxDate);
            sources.insert(sources.end(), row->mSources.begin(), row->mSources.end());

            if (row->mIsin)
            {
                isins.push_back(*row->mIsin);
            }
            else
            {
                cashInterest = true;
            }
        }

        DecisionQuestion question{
            .mType = RequestType::IncomeDetailsRequired,
            .mAffectedReports = {ReportType::OtherInterest},
            .mSources = sources,
            .mTaxDate = firstDate,
            .mIsin = std::nullopt,
            .mData = normalizedData(InterestTreatyClaimQuestion{
                .mYear = aYear,
                .mCountry = country,
                .mIsins = std::move(isins),
                .mCashInterest = cashInterest,
            }),
        };
        const auto* answer = aDecisions.find(question);

        if (answer == nullptr)
        {
            aResult.mRequests.push_back(
                RequestInput{.mQuestion = std::move(question), .mProcessingDiagnostics = {}});

            continue;
        }

        // The session accepts only a yes or no answer for this question.
        if (!std::get<InterestTreatyClaimAnswer>(*answer).mClaim)
        {
            continue;
        }

        for (const auto* row : asked)
        {
            aResult.mOtherReport.mRows[row->mIndex].mTreatyClaim =
                TreatyClaim{.mStatement = std::nullopt, .mCatalogueRow = std::nullopt};
        }

        aResult.mDiagnostics.push_back(
            incomeDiagnostic(ReportType::OtherInterest,
                             DiagnosticSeverity::Warning,
                             ProcessingDiagnosticCode::TreatyClaimDetailsMissing,
                             makeDiagnosticMessage(MessageKey::TreatyClaimDetailsMissing,
                                                   {textParam("country", country)}),
                             sources,
                             firstDate,
                             std::nullopt,
                             std::nullopt));
    }
}

} // namespace

InterestPaymentsResult prepareInterestPayments(const MergedStatement& aStatement,
                                               const ReportingYearLedger& aLedger,
                                               const ReferenceCatalogue& aCatalogue,
                                               const OfficialRateTable& aRates,
                                               RunDecisions& aDecisions) {
    InterestPaymentsResult result;
    const auto payments = positivePayments(paymentsOf(aStatement, aLedger), result.mDiagnostics);

    // One payer per coupon ISIN, in ISIN order, and one per broker for the cash interest.
    std::map<Isin, PayerOutcome> couponPayers;
    std::vector<ExcludedItemInput> noExcludedItems; // The coupon form leaves no payer out.
    const PayerSinks sinks{result.mDiagnostics, result.mRequests, noExcludedItems};

    for (const auto& [isin, group] : couponsByIsin(payments))
    {
        couponPayers.emplace(isin,
                             resolvePayer(kCouponForm, sinks, isin, group, aCatalogue, aDecisions));
    }

    // A bond with a sale or maturity is asked about even when it has no coupon in the year.
    // Coupons stay out of Doh-Obr for a bond whose type is open or not supported.
    const std::set<Isin> skippedCoupons = askBondTypes(aStatement, aLedger, aDecisions, result);

    if (payments.empty())
    {
        return result;
    }

    const auto cashPayers = brokerPayers(payments, aCatalogue, result.mDiagnostics);

    // Only the selection's warnings go on; the error of a missing rate is made for each payment.
    const auto needs = needsOf(payments, skippedCoupons);
    OfficialRateSelection selection;

    if (!needs.empty())
    {
        selection = selectOfficialRates(needs, aRates);

        for (const auto& diagnostic : selection.mDiagnostics)
        {
            if (diagnostic.mSeverity == DiagnosticSeverity::Warning)
            {
                result.mDiagnostics.push_back(diagnostic);
            }
        }
    }

    IncomeEurConverter converter{selection, result.mDiagnostics};
    std::map<Broker, DepositGroup> deposits;
    std::map<std::string, TreatyGroup> treatyGroups;
    Money total = 0;
    bool depositComplete = true;
    bool hasDeposit = false;

    for (const auto& payment : payments)
    {
        const bool isCoupon = payment.mRoute == Route::Coupon;

        if (isCoupon && skippedCoupons.contains(payment.mInstrument->mIsin.value()))
        {
            continue;
        }

        hasDeposit |= payment.mRoute == Route::Deposit;

        const auto firstNewDiagnostic = result.mDiagnostics.size();
        const auto amounts = convert(converter, selection, payment, result.mDiagnostics);

        if (!amounts)
        {
            markCashInterestBroker(result.mDiagnostics, firstNewDiagnostic, payment);
            depositComplete &= payment.mRoute != Route::Deposit;

            continue;
        }

        if (payment.mRoute == Route::Deposit)
        {
            auto& group = deposits[brokerOf(payment)];

            depositComplete &=
                addChecked(total, amounts->mGross, payment, result.mDiagnostics) &&
                addChecked(group.mInterest, amounts->mGross, payment, result.mDiagnostics) &&
                addChecked(group.mForeignTax, amounts->mTax, payment, result.mDiagnostics);
            group.mPayments.push_back(payment.mReference);

            continue;
        }

        // A row that rounds to 0.00 cannot be written, and is never netted against another one.
        if (amounts->mGross < kHalfCent)
        {
            result.mDiagnostics.push_back(notPositiveWarning(payment));

            continue;
        }

        const auto& payer = isCoupon ? couponPayers.at(payment.mInstrument->mIsin.value())
                                     : cashPayers.at(brokerOf(payment));

        // The tax of a Slovenian issuer's coupon cannot be foreign tax; the user checks it.
        if (isCoupon && payer.mPayer.mCountry == kSlovenia && amounts->mTax > 0)
        {
            result.mDiagnostics.push_back(slovenianCouponTaxWarning(payment));
        }

        auto row = obrRow(payment, *amounts, payer);
        const auto problems = obrFieldProblems(row);

        for (const auto field : problems)
        {
            result.mDiagnostics.push_back(obrValueError(payment, field));
        }

        markCashInterestBroker(result.mDiagnostics, firstNewDiagnostic, payment);

        // A row with a value the form cannot hold is not made, so no partial row is written.
        if (problems.empty())
        {
            addToTreatyGroup(treatyGroups, payment, row, result.mOtherReport.mRows.size());
            result.mOtherReport.mRows.push_back(std::move(row));
        }
    }

    applyTreatyClaims(result, treatyGroups, aLedger.mYear, aCatalogue, aDecisions);

    if (hasDeposit && depositComplete)
    {
        result.mQualifyingTotal = total;

        // Tiny foreign payments can convert to EUR 0.0000; a total of 0 is never asked about.
        if (total > 0)
        {
            applyAllowance(result, deposits, cashPayers, payments, aLedger.mYear, aDecisions);
        }
    }

    result.mUsedRates = converter.usedRates();

    return result;
}

void excludeCoupons(OtherInterestReportObr& aReport, const std::vector<Isin>& aIsins) {
    const auto isListed = [&aIsins](const ObrInterestRow& aRow) {
        const auto* coupon = std::get_if<BondCoupon>(&aRow.mKind);

        return coupon != nullptr &&
               std::find(aIsins.begin(), aIsins.end(), coupon->mIsin) != aIsins.end();
    };

    std::erase_if(aReport.mRows, isListed);
}

void excludeBrokerCashInterest(OtherInterestReportObr& aReport,
                               const std::vector<Broker>& aBrokers) {
    const auto isListed = [&aBrokers](const ObrInterestRow& aRow) {
        const auto* cash = std::get_if<BrokerCashInterest>(&aRow.mKind);

        return cash != nullptr &&
               std::find(aBrokers.begin(), aBrokers.end(), cash->mBroker) != aBrokers.end();
    };

    std::erase_if(aReport.mRows, isListed);
}

void appendInterestPayments(ApplicationResultInput& aInput, InterestPaymentsResult&& aResult) {
    appendStageOutput(aInput, std::move(aResult.mDiagnostics), std::move(aResult.mRequests));
}

} // namespace taxbroker
