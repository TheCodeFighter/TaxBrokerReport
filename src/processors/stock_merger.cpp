#include "processors/stock_merger.hpp"
#include "processors/lot_carry.hpp"
#include "taxbroker/diagnostic_messages.hpp"
#include "taxbroker/exact_arithmetic.hpp"
#include "taxbroker/text_format.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace taxbroker {
namespace {

using namespace lot_carry;

/// The rows of one merger of two companies, found by ISIN, and the date of its first row.
struct MergerGroup {
    IsinChangePair mPair;
    Date mDate;
};

/// A row of a stock merger, by the position of its instrument and its row.
struct MergerRow {
    std::size_t mInstrument{};
    std::size_t mRow{};
};

/// The rows of one action while the rows are grouped.
struct RowGroup {
    std::optional<std::string> mActionId;
    Date mFirstDate;
    std::vector<MergerRow> mRows;
};

/// Rows of one broker action ID are one merger; rows without an ID are one merger when they are at
/// most kSameActionWindowDays from the first row of the group.
std::vector<RowGroup> groupRows(const MergedStatement& aStatement) {
    std::vector<RowGroup> groups;
    const std::vector<TradeInstrument>& instruments = aStatement.mPresentation.mTradeInstruments;

    for (std::size_t instrument = 0; instrument < instruments.size(); ++instrument)
    {
        const std::vector<CorporateAction>& actions = instruments[instrument].mCorporateActions;

        for (std::size_t row = 0; row < actions.size(); ++row)
        {
            const CorporateAction& action = actions[row];

            if (action.mType != CorporateActionType::StockMerger)
            {
                continue;
            }

            const Date date = action.mMetadata.mTaxDate;
            const auto group =
                std::find_if(groups.begin(), groups.end(), [&action, date](const RowGroup& aGroup) {
                    if (action.mBrokerActionId || aGroup.mActionId)
                    {
                        return action.mBrokerActionId == aGroup.mActionId;
                    }

                    const DayDuration distance = date < aGroup.mFirstDate
                                                     ? aGroup.mFirstDate - date
                                                     : date - aGroup.mFirstDate;

                    return distance <= DayDuration{kSameActionWindowDays};
                });

            if (group == groups.end())
            {
                groups.push_back({.mActionId = action.mBrokerActionId,
                                  .mFirstDate = date,
                                  .mRows = {{instrument, row}}});
            }
            else
            {
                group->mRows.push_back({instrument, row});
            }
        }
    }

    return groups;
}

/// The mergers that are exactly one ISIN out and one other ISIN in, with the rows of each; other
/// groups (more ISINs, a row without units) stay unsupported.
std::vector<MergerGroup> mergersOf(const MergedStatement& aStatement) {
    std::vector<MergerGroup> mergers;
    const std::vector<TradeInstrument>& instruments = aStatement.mPresentation.mTradeInstruments;

    for (const RowGroup& group : groupRows(aStatement))
    {
        std::set<std::size_t> outInstruments;
        std::set<std::size_t> inInstruments;
        bool hasRowWithoutUnits = false;

        for (const MergerRow& row : group.mRows)
        {
            const Units delta =
                instruments[row.mInstrument].mCorporateActions[row.mRow].mUnitsDelta;

            hasRowWithoutUnits = hasRowWithoutUnits || delta == 0;

            if (delta < 0)
            {
                outInstruments.insert(row.mInstrument);
            }
            else if (delta > 0)
            {
                inInstruments.insert(row.mInstrument);
            }
        }

        if (hasRowWithoutUnits || outInstruments.size() != 1 || inInstruments.size() != 1 ||
            *outInstruments.begin() == *inInstruments.begin())
        {
            continue;
        }

        MergerGroup merger{
            .mPair = {.mOldIsin = instruments[*outInstruments.begin()].mIsin,
                      .mNewIsin = instruments[*inInstruments.begin()].mIsin},
            .mDate = group.mFirstDate,
        };

        for (const MergerRow& row : group.mRows)
        {
            const bool isOut = row.mInstrument == *outInstruments.begin();

            (isOut ? merger.mPair.mOutRows : merger.mPair.mInRows).push_back(row.mRow);
        }

        mergers.push_back(std::move(merger));
    }

    return mergers;
}

/// The total of the cash (aIsCash) or of the market values of the rows. A row without cash counts
/// as zero; none when a row has no market value or the total does not fit.
std::optional<Money> totalAmount(const TradeInstrument& aInstrument,
                                 const std::vector<std::size_t>& aRows,
                                 const bool aIsCash) {
    Money total = 0;

    for (const std::size_t row : aRows)
    {
        const CorporateAction& action = aInstrument.mCorporateActions[row];
        const std::optional<Money>& amount = aIsCash ? action.mCashProceeds : action.mMarketValue;

        if (!amount)
        {
            if (aIsCash)
            {
                continue;
            }

            return std::nullopt;
        }

        const NumericResult<std::int64_t> sum = checkedAdd(total, *amount);

        if (const std::int64_t* value = std::get_if<std::int64_t>(&sum))
        {
            total = *value;
        }
        else
        {
            return std::nullopt;
        }
    }

    return total;
}

/// The currency of the amounts of all rows of the merger; Unknown when a row has none or they
/// differ.
Currency
currencyOf(const TradeInstrument& aOld, const TradeInstrument& aNew, const IsinChangePair& aPair) {
    std::set<Currency> currencies;

    for (const std::size_t row : aPair.mOutRows)
    {
        currencies.insert(aOld.mCorporateActions[row].mAmountCurrency);
    }

    for (const std::size_t row : aPair.mInRows)
    {
        currencies.insert(aNew.mCorporateActions[row].mAmountCurrency);
    }

    return currencies.size() == 1 ? *currencies.begin() : Currency::Unknown;
}

/// The price of one unit: the total divided by the units, rounded once to eight decimals. None
/// when it does not fit or is zero at eight decimals.
std::optional<UnitPrice> unitPriceOf(const Money aTotal, const Units aUnits) {
    const NumericResult<ExactRational> exact =
        ExactRational::create(WideInteger{aTotal} * UNITS_SCALE,
                              WideInteger{MONEY_SCALE} * WideInteger{aUnits});

    if (const ExactRational* value = std::get_if<ExactRational>(&exact))
    {
        const NumericResult<std::int64_t> rounded =
            roundToScaled(*value,
                          UNIT_PRICE_SCALE,
                          DestinationRange{.mPolicy = ValuePolicy::Positive});

        if (const std::int64_t* price = std::get_if<std::int64_t>(&rounded))
        {
            return *price;
        }
    }

    return std::nullopt;
}

/// The date and time of a trade made from a row on the answered date. A date other than the
/// row's has no time of day.
EventMetadata metadataOn(const CorporateAction& aRow, const Date aDate) {
    EventMetadata metadata = aRow.mMetadata;

    if (metadata.mTaxDate != aDate)
    {
        metadata.mTaxDate = aDate;
        metadata.mSourceTimestamp = std::nullopt;
        metadata.mOrderingTimestamp = std::nullopt;
    }

    return metadata;
}

/// Marks the rows of a merger the stage asked about or blocked, so that no other stage adds an
/// error of its own for them.
void markPending(MergedStatement& aStatement, const IsinChangePair& aPair) {
    std::vector<TradeInstrument>& instruments = aStatement.mPresentation.mTradeInstruments;

    for (const std::size_t row : aPair.mOutRows)
    {
        instruments[instrumentIndexOf(aStatement, aPair.mOldIsin)].mCorporateActions[row].mType =
            CorporateActionType::StockMergerPending;
    }

    for (const std::size_t row : aPair.mInRows)
    {
        instruments[instrumentIndexOf(aStatement, aPair.mNewIsin)].mCorporateActions[row].mType =
            CorporateActionType::StockMergerPending;
    }
}

/// Replaces the rows of a taxable merger by the sale of the old shares and the purchase of the new
/// ones, both on the answered date. The old shares are sold at (value + cash) / old units and the
/// new ones bought at value / new units, each rounded once to eight decimals. Returns false when a
/// price does not fit; the statement is then unchanged.
bool applyTaxable(MergedStatement& aStatement,
                  const IsinChangePair& aPair,
                  const StockMergerQuestion& aQuestion,
                  const StockMergerAnswer& aAnswer) {
    std::vector<TradeInstrument>& instruments = aStatement.mPresentation.mTradeInstruments;
    TradeInstrument& oldInstrument = instruments[instrumentIndexOf(aStatement, aPair.mOldIsin)];
    TradeInstrument& newInstrument = instruments[instrumentIndexOf(aStatement, aPair.mNewIsin)];

    const NumericResult<std::int64_t> proceeds = checkedAdd(aAnswer.mMarketValue, aQuestion.mCash);
    const std::int64_t* proceedsValue = std::get_if<std::int64_t>(&proceeds);
    const std::optional<UnitPrice> salePrice =
        proceedsValue ? unitPriceOf(*proceedsValue, aQuestion.mOldUnits) : std::nullopt;
    const std::optional<UnitPrice> purchasePrice =
        unitPriceOf(aAnswer.mMarketValue, aQuestion.mNewUnits);

    if (!salePrice || !purchasePrice)
    {
        return false;
    }

    std::vector<TradeTransaction> sales;
    std::vector<TradeTransaction> purchases;

    for (const std::size_t row : aPair.mOutRows)
    {
        const CorporateAction& action = oldInstrument.mCorporateActions[row];

        sales.push_back({
            .mMetadata = metadataOn(action, aAnswer.mEffectiveDate),
            .mTradeSide = TradeSide::Sell,
            .mUnitPrice = *salePrice,
            .mUnits = -action.mUnitsDelta,
            .mAmount = std::nullopt,
            .mFeePaid = 0,
            .mExchangeRate = std::nullopt,
            .mCurrency = aQuestion.mCurrency,
            .mFeeCurrency = aQuestion.mCurrency,
            .mTransactionTaxPaid = 0,
            .mUnitEvidence = action.mUnitEvidence,
            .mInstrumentEvidence = {},
            .mFromCashMerger = false,
            .mFromStockMerger = true,
            .mPurchaseKind = PurchaseKind::NotAPurchase,
            .mListedAs = std::nullopt,
        });
    }

    for (const std::size_t row : aPair.mInRows)
    {
        const CorporateAction& action = newInstrument.mCorporateActions[row];

        purchases.push_back({
            .mMetadata = metadataOn(action, aAnswer.mEffectiveDate),
            .mTradeSide = TradeSide::Buy,
            .mUnitPrice = *purchasePrice,
            .mUnits = action.mUnitsDelta,
            .mAmount = std::nullopt,
            .mFeePaid = 0,
            .mExchangeRate = std::nullopt,
            .mCurrency = aQuestion.mCurrency,
            .mFeeCurrency = aQuestion.mCurrency,
            .mTransactionTaxPaid = 0,
            .mUnitEvidence = action.mUnitEvidence,
            .mInstrumentEvidence = {},
            .mFromCashMerger = false,
            .mFromStockMerger = false,
            .mPurchaseKind = PurchaseKind::StatusChangeExchange,
            .mListedAs = std::nullopt,
        });
    }

    // Erase from the back, so the earlier positions stay valid.
    for (auto row = aPair.mOutRows.rbegin(); row != aPair.mOutRows.rend(); ++row)
    {
        oldInstrument.mCorporateActions.erase(oldInstrument.mCorporateActions.begin() +
                                              static_cast<std::ptrdiff_t>(*row));
    }

    for (auto row = aPair.mInRows.rbegin(); row != aPair.mInRows.rend(); ++row)
    {
        newInstrument.mCorporateActions.erase(newInstrument.mCorporateActions.begin() +
                                              static_cast<std::ptrdiff_t>(*row));
    }

    oldInstrument.mTransactions.insert(oldInstrument.mTransactions.end(),
                                       std::make_move_iterator(sales.begin()),
                                       std::make_move_iterator(sales.end()));
    newInstrument.mTransactions.insert(newInstrument.mTransactions.end(),
                                       std::make_move_iterator(purchases.begin()),
                                       std::make_move_iterator(purchases.end()));
    sortChronologically(oldInstrument.mTransactions);
    sortChronologically(newInstrument.mTransactions);
    rebuildChronologicalOrder(aStatement);

    return true;
}

} // namespace

StockMergerResult applyStockMergers(const MergedStatement& aStatement,
                                    const ReportingYearLedger& aLedger,
                                    RunDecisions& aDecisions) {
    StockMergerResult result;
    std::vector<MergerGroup> mergers = mergersOf(aStatement);

    // Most runs have no such merger: the statement is read in place and copied only when the first
    // merger has to change it.
    if (mergers.empty())
    {
        return result;
    }

    std::optional<MergedStatement> changed;
    const auto current = [&aStatement, &changed]() -> const MergedStatement& {
        return changed ? *changed : aStatement;
    };

    const auto writable = [&aStatement, &changed]() -> MergedStatement& {
        if (!changed)
        {
            changed = aStatement;
        }

        return *changed;
    };

    std::stable_sort(mergers.begin(),
                     mergers.end(),
                     [](const MergerGroup& aLeft, const MergerGroup& aRight) {
                         return aLeft.mDate < aRight.mDate;
                     });

    for (MergerGroup& merger : mergers)
    {
        // Rows moved only when a statement was changed: the positions are then found again.
        if (changed)
        {
            std::vector<MergerGroup> refreshed = mergersOf(*changed);
            const auto found =
                std::find_if(refreshed.begin(),
                             refreshed.end(),
                             [&merger](const MergerGroup& aOther) {
                                 return aOther.mPair.mOldIsin == merger.mPair.mOldIsin &&
                                        aOther.mPair.mNewIsin == merger.mPair.mNewIsin &&
                                        aOther.mDate == merger.mDate;
                             });

            if (found == refreshed.end())
            {
                continue;
            }

            merger = std::move(*found);
        }

        const IsinChangePair& pair = merger.mPair;
        const std::vector<TradeInstrument>& instruments = current().mPresentation.mTradeInstruments;
        const TradeInstrument& oldInstrument =
            instruments[instrumentIndexOf(current(), pair.mOldIsin)];
        const TradeInstrument& newInstrument =
            instruments[instrumentIndexOf(current(), pair.mNewIsin)];

        const std::optional<Units> oldUnits = totalUnits(oldInstrument, pair.mOutRows, true);
        const std::optional<Units> newUnits = totalUnits(newInstrument, pair.mInRows, false);
        const std::optional<Money> cash = totalAmount(oldInstrument, pair.mOutRows, true);
        const Currency currency = currencyOf(oldInstrument, newInstrument, pair);

        // A merger whose rows give no usable units, cash or currency stays unsupported.
        if (!oldUnits || !newUnits || !cash || currency == Currency::Unknown)
        {
            continue;
        }

        const CorporateAction& firstRow = oldInstrument.mCorporateActions[pair.mOutRows.front()];
        Date earliest = firstRow.mMetadata.mTaxDate;
        Date latest = earliest;
        std::vector<SourceReference> sources;
        StockMergerQuestion data{
            .mOldIsin = pair.mOldIsin,
            .mOldName = oldInstrument.mName,
            .mNewIsin = pair.mNewIsin,
            .mNewName = newInstrument.mName,
            .mOldUnits = *oldUnits,
            .mNewUnits = *newUnits,
            .mCurrency = currency,
            .mCash = *cash,
            .mSuggestedValue = totalAmount(newInstrument, pair.mInRows, false),
            .mEffectiveDate = earliest,
        };

        const auto add = [&](const CorporateAction& aAction) {
            earliest = std::min(earliest, aAction.mMetadata.mTaxDate);
            latest = std::max(latest, aAction.mMetadata.mTaxDate);
            sources.insert(sources.end(),
                           aAction.mMetadata.mSources.begin(),
                           aAction.mMetadata.mSources.end());
            data.mSources.push_back(factsOf(aAction));
        };

        for (const std::size_t row : pair.mOutRows)
        {
            add(oldInstrument.mCorporateActions[row]);
        }

        for (const std::size_t row : pair.mInRows)
        {
            add(newInstrument.mCorporateActions[row]);
        }

        data.mEarliestDate = earliest;
        data.mLatestDate = latest;

        // A merger with a row in the selected year is reported that year (the sale of the old
        // shares, the basis of the new ones). An earlier one matters for a sale of the year of
        // either ISIN (also one that a later change of the year makes), and a later one up to the
        // end of the loss-analysis view for the same sales.
        const bool inYear = earliest <= aLedger.mYearEnd && latest >= aLedger.mYearStart;
        const bool matters = inYear || ((hasSaleInYear(oldInstrument, aLedger, pair.mOutRows) ||
                                         hasSaleInYear(newInstrument, aLedger, pair.mInRows)) &&
                                        earliest <= aLedger.mLossWindowEnd);

        if (!matters)
        {
            continue;
        }

        const Date effectiveDate = data.mEffectiveDate;
        DecisionQuestion question{
            .mType = RequestType::CorporateActionStockMergerRequired,
            .mAffectedReports = {ReportType::CapitalGains},
            .mSources = sources,
            .mTaxDate = effectiveDate,
            .mIsin = pair.mOldIsin,
            .mData = std::move(data),
        };
        const AnswerValue* answer = aDecisions.find(question);
        const StockMergerAnswer* given = answer ? std::get_if<StockMergerAnswer>(answer) : nullptr;

        // Both ISINs get the same diagnostic, so both stay blocked until it is resolved. The rows
        // are marked, so that the corporate-action stage adds no error of its own.
        const auto blockBoth = [&](ProcessingDiagnosticCode aCode, MessageKey aKey) {
            std::vector<std::size_t> positions;

            markPending(writable(), pair);

            for (const TradeInstrument* instrument : {&oldInstrument, &newInstrument})
            {
                positions.push_back(result.mDiagnostics.size());
                result.mDiagnostics.push_back(isinDiagnostic(DiagnosticSeverity::Error,
                                                             aCode,
                                                             makeDiagnosticMessage(aKey),
                                                             *instrument,
                                                             sources,
                                                             effectiveDate));
            }

            return positions;
        };

        if (answer == nullptr)
        {
            result.mRequests.push_back(
                {.mQuestion = std::move(question),
                 .mProcessingDiagnostics =
                     blockBoth(ProcessingDiagnosticCode::CorporateActionAmbiguous,
                               MessageKey::CorporateActionAmbiguousStockMergerRequired)});
            continue;
        }

        // The session accepts only a stock merger answer or a cancel.
        if (given == nullptr)
        {
            blockBoth(ProcessingDiagnosticCode::CorporateActionAmbiguous,
                      MessageKey::CorporateActionAmbiguousCancelled);
            continue;
        }

        const StockMergerQuestion& asked = std::get<StockMergerQuestion>(question.mData);
        const std::vector<MessageParam> params{
            textParam("oldIsin", pair.mOldIsin),
            textParam("newIsin", pair.mNewIsin),
            textParam("effectiveDate", formatCalendarDate(given->mEffectiveDate))};

        if (given->mTreatment == StockMergerTreatment::Deferred)
        {
            // The part paid in cash is a sale of a share of the old lots; that is not supported.
            if (asked.mCash > 0)
            {
                blockBoth(ProcessingDiagnosticCode::UnsupportedCorporateAction,
                          MessageKey::UnsupportedCorporateActionStockMergerDeferralWithCash);
                continue;
            }

            if (crossesDate(oldInstrument, newInstrument, pair, given->mEffectiveDate))
            {
                blockBoth(ProcessingDiagnosticCode::CorporateActionInconsistent,
                          MessageKey::CorporateActionInconsistentTradeAcrossIsinChange);
                continue;
            }

            result.mDiagnostics.push_back(isinDiagnostic(
                DiagnosticSeverity::Warning,
                ProcessingDiagnosticCode::CorporateActionApplied,
                makeDiagnosticMessage(MessageKey::CorporateActionAppliedStockMergerDeferred,
                                      params),
                newInstrument,
                sources,
                given->mEffectiveDate));

            // The lots keep their values; the units change in the ratio of the units that came in
            // over the units that left, reduced.
            const std::int64_t divisor = std::gcd(*oldUnits, *newUnits);

            // Nothing below reads the instruments by reference: they may be the ones moved here.
            carryOut(writable(),
                     pair,
                     {.mSameIssuer = true,
                      .mNewShares = *newUnits / divisor,
                      .mOldShares = *oldUnits / divisor,
                      .mEffectiveDate = given->mEffectiveDate},
                     true);
            continue;
        }

        result.mDiagnostics.push_back(isinDiagnostic(
            DiagnosticSeverity::Warning,
            ProcessingDiagnosticCode::CorporateActionApplied,
            makeDiagnosticMessage(MessageKey::CorporateActionAppliedStockMerger, params),
            oldInstrument,
            sources,
            given->mEffectiveDate));

        // A price that does not fit leaves the rows as they are, so the merger stays unsupported.
        const IsinChangePair appliedPair = pair;

        if (!applyTaxable(writable(), appliedPair, asked, *given))
        {
            result.mDiagnostics.pop_back();
        }
    }

    result.mStatement = std::move(changed);

    return result;
}

} // namespace taxbroker
