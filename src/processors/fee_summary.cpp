#include "processors/fee_summary.hpp"

#include "taxbroker/currency.hpp"
#include "taxbroker/exact_arithmetic.hpp"

#include <algorithm>
#include <iterator>
#include <map>
#include <utility>
#include <variant>

namespace taxbroker {

namespace {

/// The running sums of one broker (and, for benefits, one type).
struct Accumulator {
    /// One entry per original currency; an empty total means the sum did not fit.
    std::map<Currency, CurrencyTotal> mByCurrency;
    Money mEurSum{};
    bool mEurComplete{true};
    std::size_t mBrokerRateCount{};

    /// Adds an amount in aCurrency. aEur is the EUR value: none when it cannot be known.
    void add(Currency aCurrency, Money aAmount, std::optional<Money> aEur) {
        auto& total =
            mByCurrency.try_emplace(aCurrency, CurrencyTotal{.mCurrency = aCurrency}).first->second;

        ++total.mCount;

        // The first amount starts the sum; a sum that does not fit stays empty for good.
        if (total.mCount == 1)
        {
            total.mTotal = aAmount;
        }
        else if (total.mTotal)
        {
            const auto sum = checkedAdd(*total.mTotal, aAmount);

            total.mTotal = std::holds_alternative<std::int64_t>(sum)
                               ? std::optional<Money>{std::get<std::int64_t>(sum)}
                               : std::nullopt;
        }

        if (!mEurComplete)
        {
            return;
        }

        if (!aEur)
        {
            mEurComplete = false;

            return;
        }

        const auto eurSum = checkedAdd(mEurSum, *aEur);

        if (const auto* value = std::get_if<std::int64_t>(&eurSum))
        {
            mEurSum = *value;
        }
        else
        {
            mEurComplete = false;
        }
    }

    /// The amounts by currency code, as the result lists them.
    [[nodiscard]] std::vector<CurrencyTotal> currencies() const {
        std::vector<CurrencyTotal> result;

        result.reserve(mByCurrency.size());
        std::ranges::transform(mByCurrency, std::back_inserter(result), [](const auto& aEntry) {
            return aEntry.second;
        });

        std::ranges::sort(result, [](const CurrencyTotal& aLeft, const CurrencyTotal& aRight) {
            return currencyCode(aLeft.mCurrency) < currencyCode(aRight.mCurrency);
        });

        return result;
    }

    [[nodiscard]] std::optional<Money> eurTotal() const {
        return mEurComplete ? std::optional<Money>{mEurSum} : std::nullopt;
    }
};

/// The sum of the EUR totals of all rows; none without a row or when a row's total is none.
template <typename Row> std::optional<Money> overallEur(const std::vector<Row>& aRows) {
    if (aRows.empty())
    {
        return std::nullopt;
    }

    Money sum{};

    for (const auto& row : aRows)
    {
        if (!row.mEurTotal)
        {
            return std::nullopt;
        }

        const auto added = checkedAdd(sum, *row.mEurTotal);

        if (!std::holds_alternative<std::int64_t>(added))
        {
            return std::nullopt;
        }

        sum = std::get<std::int64_t>(added);
    }

    return sum;
}

/// A fee of one event, as the walk collects it.
struct FeeInput {
    Broker mBroker{Broker::Unknown};
    Currency mCurrency{Currency::Unknown};
    Money mAmount{};
};

/// For an event that has no broker rate.
const std::optional<BrokerExchangeRate> kNoBrokerRate;

/// The EUR value of a fee: itself in EUR, else the event's own verified broker rate, given for
/// aRateCurrency, when that is the fee's currency. Never an official rate and never the rate of
/// another event.
std::optional<Money> feeInEur(const FeeInput& aFee,
                              const std::optional<BrokerExchangeRate>& aRate,
                              Currency aRateCurrency,
                              bool& aUsedBrokerRate) {
    aUsedBrokerRate = false;

    if (aFee.mCurrency == Currency::EUR)
    {
        return aFee.mAmount;
    }

    if (!aRate || aRateCurrency != aFee.mCurrency)
    {
        return std::nullopt;
    }

    const auto converted = convertBrokerMoney(aFee.mAmount, *aRate);

    if (!std::holds_alternative<Money>(converted))
    {
        return std::nullopt;
    }

    aUsedBrokerRate = true;

    return std::get<Money>(converted);
}

} // namespace

FeeAndBenefitSummary summarizeFeesAndBenefits(const MergedStatement& aStatement,
                                              const ReportingYearLedger& aLedger) {
    const auto& statement = aStatement.mPresentation;
    std::map<Broker, Accumulator> fees;
    std::map<std::pair<Broker, BenefitType>, Accumulator> benefits;

    // aRate is the event's own broker rate, given for aRateCurrency (the foreign side).
    const auto addFee = [&fees](const FeeInput& aFee,
                                const std::optional<BrokerExchangeRate>& aRate,
                                Currency aRateCurrency) {
        // An empty fee field means zero and is not a fee.
        if (aFee.mAmount <= 0)
        {
            return;
        }

        bool usedBrokerRate = false;
        const auto eur = feeInEur(aFee, aRate, aRateCurrency, usedBrokerRate);
        auto& accumulator = fees[aFee.mBroker];

        accumulator.add(aFee.mCurrency, aFee.mAmount, eur);
        accumulator.mBrokerRateCount += usedBrokerRate ? 1 : 0;
    };

    for (const auto& reference : aLedger.mSelectedYearEvents)
    {
        switch (reference.mKind)
        {
        case StatementEventKind::Trade: {
            const auto& trade = statement.mTradeInstruments.at(reference.mInstrumentIndex.value())
                                    .mTransactions.at(reference.mEventIndex);

            addFee({.mBroker = primarySource(trade.mMetadata).mBroker,
                    .mCurrency = trade.mFeeCurrency,
                    .mAmount = trade.mFeePaid},
                   trade.mExchangeRate,
                   trade.mCurrency);
            break;
        }
        case StatementEventKind::BondRedemption: {
            const auto& redemption = statement.mBondRedemptions.at(reference.mEventIndex);

            // The fee has its own currency; the event's broker rate converts it only when that is
            // the amount's currency, like a trade fee.
            addFee({.mBroker = primarySource(redemption.mMetadata).mBroker,
                    .mCurrency = redemption.mFeeCurrency,
                    .mAmount = redemption.mFeePaid},
                   redemption.mExchangeRate,
                   redemption.mCurrency);
            break;
        }
        case StatementEventKind::PrivateMarket: {
            const auto& event = statement.mPrivateMarketEvents.at(reference.mEventIndex);

            addFee({.mBroker = primarySource(event.mMetadata).mBroker,
                    .mCurrency = event.mCurrency,
                    .mAmount = event.mFeePaid},
                   kNoBrokerRate,
                   Currency::Unknown);
            break;
        }
        case StatementEventKind::Benefit: {
            const auto& benefit = statement.mBenefitEvents.at(reference.mEventIndex);
            const auto broker = primarySource(benefit.mMetadata).mBroker;
            const auto eur = benefit.mCurrency == Currency::EUR
                                 ? std::optional<Money>{benefit.mAmount}
                                 : std::nullopt;

            benefits[{broker, benefit.mType}].add(benefit.mCurrency, benefit.mAmount, eur);
            break;
        }
        case StatementEventKind::CorporateAction:
        case StatementEventKind::Dividend:
        case StatementEventKind::Interest:
            // Income rows carry no fee, and an action has no amount.
            break;
        }
    }

    FeeAndBenefitSummary summary;

    for (const auto& [broker, accumulator] : fees)
    {
        summary.mFees.mBrokers.push_back({.mBroker = broker,
                                          .mByCurrency = accumulator.currencies(),
                                          .mEurTotal = accumulator.eurTotal(),
                                          .mBrokerRateCount = accumulator.mBrokerRateCount});
    }

    for (const auto& [key, accumulator] : benefits)
    {
        summary.mBenefits.mBrokers.push_back({.mBroker = key.first,
                                              .mType = key.second,
                                              .mByCurrency = accumulator.currencies(),
                                              .mEurTotal = accumulator.eurTotal()});
    }

    summary.mFees.mEurTotal = overallEur(summary.mFees.mBrokers);
    summary.mBenefits.mEurTotal = overallEur(summary.mBenefits.mBrokers);

    return summary;
}

} // namespace taxbroker
