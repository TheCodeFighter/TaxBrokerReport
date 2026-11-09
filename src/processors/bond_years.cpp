#include "processors/bond_years.hpp"

#include "taxbroker/exact_arithmetic.hpp"

#include <cstdint>
#include <variant>

namespace taxbroker {
namespace {

bool isInYear(const ReportingYearLedger& aLedger, Date aDate) {
    return aDate >= aLedger.mYearStart && aDate <= aLedger.mYearEnd;
}

/// Adds aDelta to aTotal; false on overflow.
bool addUnits(Units& aTotal, Units aDelta) {
    const auto sum = checkedAdd(aTotal, aDelta);
    const auto* value = std::get_if<std::int64_t>(&sum);

    if (value == nullptr)
    {
        return false;
    }

    aTotal = *value;

    return true;
}

/// Adds the sources and the date of one event to the bond.
void addEvent(BondYear& aBond, const EventMetadata& aMetadata) {
    aBond.mSources.insert(aBond.mSources.end(),
                          aMetadata.mSources.begin(),
                          aMetadata.mSources.end());

    if (!aBond.mFirstDate || aMetadata.mTaxDate < *aBond.mFirstDate)
    {
        aBond.mFirstDate = aMetadata.mTaxDate;
    }
}

} // namespace

std::map<Isin, BondYear> bondYearsOf(const MergedStatement& aStatement,
                                     const ReportingYearLedger& aLedger) {
    const auto& presentation = aStatement.mPresentation;
    std::map<Isin, BondYear> bonds;
    std::map<Isin, AssetClass> tradeClasses;

    for (const auto& instrument : presentation.mTradeInstruments)
    {
        tradeClasses[instrument.mIsin] = instrument.mAssetClass;

        if (instrument.mAssetClass != AssetClass::Bond)
        {
            continue;
        }

        for (const auto& trade : instrument.mTransactions)
        {
            if (!isInYear(aLedger, trade.mMetadata.mTaxDate))
            {
                continue;
            }

            auto& bond = bonds[instrument.mIsin];
            const bool isSale = trade.mTradeSide == TradeSide::Sell;

            bond.mName = instrument.mName;
            addEvent(bond, trade.mMetadata);
            bond.mOverflow |=
                !addUnits(isSale ? bond.mUnits.mSoldUnits : bond.mUnits.mBoughtUnits, trade.mUnits);
        }
    }

    for (const auto& redemption : presentation.mBondRedemptions)
    {
        const auto traded = tradeClasses.find(redemption.mIsin);
        const auto date = redemption.mMetadata.mTaxDate;

        // A bond row of an ISIN that trades under another class is not a bond list entry.
        if (!isInYear(aLedger, date) ||
            (traded != tradeClasses.end() && traded->second != AssetClass::Bond))
        {
            continue;
        }

        auto& bond = bonds[redemption.mIsin];

        // A traded bond keeps the name of its merged instrument.
        if (bond.mName.empty())
        {
            bond.mName = redemption.mName;
        }

        addEvent(bond, redemption.mMetadata);

        switch (redemption.mKind)
        {
        case BondRedemptionKind::Maturity:
            bond.mOverflow |= !addUnits(bond.mUnits.mMaturedUnits, redemption.mUnits.value_or(0));

            if (!bond.mUnits.mMaturityDate || date < *bond.mUnits.mMaturityDate)
            {
                bond.mUnits.mMaturityDate = date;
            }
            break;
        case BondRedemptionKind::MaturityCancelled:
            bond.mOverflow |= !addUnits(bond.mUnits.mMaturedUnits, -redemption.mUnits.value_or(0));
            break;
        case BondRedemptionKind::Payment:
        case BondRedemptionKind::PaymentReversal:
            break;
        }
    }

    return bonds;
}

} // namespace taxbroker
