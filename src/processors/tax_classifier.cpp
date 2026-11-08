#include "processors/tax_classifier.hpp"
#include "taxbroker/diagnostic_messages.hpp"
#include "taxbroker/exact_arithmetic.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace taxbroker {
namespace {

/// What the user chose for an unknown class.
enum class Choice {
    CapitalGains,
    Exclude,
    Cancel,
};

/// The choice in an answer to a classification question. The session accepts only these answers
/// for it.
Choice choiceOf(const AnswerValue& aAnswer) {
    const auto choice = [](const auto& aValue) -> Choice {
        using Answer = std::decay_t<decltype(aValue)>;

        if constexpr (std::is_same_v<Answer, CapitalGainsAnswer>)
        {
            return Choice::CapitalGains;
        }
        else if constexpr (std::is_same_v<Answer, ExcludeFromReportsAnswer>)
        {
            return Choice::Exclude;
        }
        else if constexpr (std::is_same_v<Answer, CancelAnswer>)
        {
            return Choice::Cancel;
        }
        else
        {
            static_assert(std::is_same_v<Answer, FailureResolutionAnswer> ||
                          std::is_same_v<Answer, NoOtherActivityAnswer> ||
                          std::is_same_v<Answer, OtherActivityAnswer> ||
                          std::is_same_v<Answer, SplitRatioAnswer> ||
                          std::is_same_v<Answer, SeparateEventsAnswer> ||
                          std::is_same_v<Answer, QuantityCorrectionAnswer> ||
                          std::is_same_v<Answer, PayerChoiceAnswer> ||
                          std::is_same_v<Answer, DepositInterestTotalAnswer> ||
                          std::is_same_v<Answer, IncomeTreatmentAnswer>);
            throw std::logic_error("Only a classification choice or a cancel can answer this.");
        }
    };

    return std::visit(choice, aAnswer);
}

bool isInYear(const ReportingYearLedger& aLedger, Date aDate) {
    return aDate >= aLedger.mYearStart && aDate <= aLedger.mYearEnd;
}

/// One diagnostic of the classification stage; it affects capital gains only.
ProcessingDiagnostic classificationDiagnostic(DiagnosticSeverity aSeverity,
                                              ProcessingDiagnosticCode aCode,
                                              DiagnosticMessage aMessage,
                                              std::vector<SourceReference> aSources,
                                              std::optional<Date> aTaxDate,
                                              const std::string& aName,
                                              const Isin& aIsin) {
    return ProcessingDiagnostic{
        .mSeverity = aSeverity,
        .mCode = aCode,
        .mContext =
            {
                .mStage = PipelineStage::FifoTax,
                .mAffectedReports = {ReportType::CapitalGains},
                .mSources = std::move(aSources),
                .mTaxDate = aTaxDate,
                .mInstrumentName = aName,
                .mIsin = aIsin,
            },
        .mMessage = std::move(aMessage.mEnglish),
        .mMessageKey = aMessage.mKey,
        .mMessageParams = std::move(aMessage.mParams),
    };
}

/// The sources and the first date of some events of one instrument.
struct EventFacts {
    std::vector<SourceReference> mSources;
    std::optional<Date> mFirstDate;

    void add(const EventMetadata& aMetadata) {
        mSources.insert(mSources.end(), aMetadata.mSources.begin(), aMetadata.mSources.end());

        if (!mFirstDate || aMetadata.mTaxDate < *mFirstDate)
        {
            mFirstDate = aMetadata.mTaxDate;
        }
    }

    [[nodiscard]] bool empty() const {
        return !mFirstDate.has_value();
    }
};

/// The trades and corporate actions of the instrument for which aSelect(date) holds.
template <typename Select> EventFacts eventsOf(const TradeInstrument& aInstrument, Select aSelect) {
    EventFacts facts;

    for (const auto& trade : aInstrument.mTransactions)
    {
        if (aSelect(trade.mMetadata.mTaxDate))
        {
            facts.add(trade.mMetadata);
        }
    }
    for (const auto& action : aInstrument.mCorporateActions)
    {
        if (aSelect(action.mMetadata.mTaxDate))
        {
            facts.add(action.mMetadata);
        }
    }

    return facts;
}

/// The class label of every row of the instrument as the files gave it, sorted, each once.
std::vector<std::string> classLabelsOf(const TradeInstrument& aInstrument) {
    std::set<std::string> labels;

    for (const auto& trade : aInstrument.mTransactions)
    {
        for (const auto& evidence : trade.mInstrumentEvidence)
        {
            labels.insert(evidence.mAssetClassLabel);
        }
    }
    for (const auto& action : aInstrument.mCorporateActions)
    {
        for (const auto& evidence : action.mInstrumentEvidence)
        {
            labels.insert(evidence.mAssetClassLabel);
        }
    }

    return {labels.begin(), labels.end()};
}

/// The (file, row) places of the rows an instrument was built from.
std::set<std::pair<std::size_t, std::size_t>> rowsOf(const TradeInstrument& aInstrument) {
    std::set<std::pair<std::size_t, std::size_t>> rows;
    const auto add = [&rows](const EventMetadata& aMetadata) {
        for (const auto& source : aMetadata.mSources)
        {
            rows.emplace(source.mInputSequence.mSourceIndex, source.mSourceRow);
        }
    };

    for (const auto& trade : aInstrument.mTransactions)
    {
        add(trade.mMetadata);
    }
    for (const auto& action : aInstrument.mCorporateActions)
    {
        add(action.mMetadata);
    }

    return rows;
}

bool isClassKey(const std::optional<MessageKey>& aKey) {
    return aKey == MessageKey::MissingFieldAssetClass ||
           aKey == MessageKey::UnsupportedAssetClassUnrecognized ||
           aKey == MessageKey::InconsistentValueAssetClass;
}

/// The positions of the parse and merge errors about the class of the unknown instruments: the
/// three parser class errors on a row the instrument was built from, and the merger's class
/// conflict of its ISIN.
std::vector<std::size_t>
takenOverClassErrors(const std::vector<TradeInstrument>& aInstruments,
                     const std::vector<StatementMergeDiagnostic>& aMergeDiagnostics) {
    std::set<std::pair<std::size_t, std::size_t>> rows;
    std::set<Isin> isins;

    for (const auto& instrument : aInstruments)
    {
        if (instrument.mAssetClass != AssetClass::Unknown)
        {
            continue;
        }

        isins.insert(instrument.mIsin);
        rows.merge(rowsOf(instrument));
    }

    std::vector<std::size_t> positions;

    for (std::size_t position = 0; position < aMergeDiagnostics.size(); ++position)
    {
        const auto isTaken = [&rows, &isins](const auto& aDiagnostic) {
            using Diagnostic = std::decay_t<decltype(aDiagnostic)>;

            if constexpr (std::is_same_v<Diagnostic, SourcedParseDiagnostic>)
            {
                const auto& parse = aDiagnostic.mDiagnostic;

                return isClassKey(parse.mMessageKey) && parse.mRowIndex &&
                       rows.contains({aDiagnostic.mSourceIndex, *parse.mRowIndex});
            }
            else
            {
                static_assert(std::is_same_v<Diagnostic, MergeDiagnostic>);

                return aDiagnostic.mCode == MergeDiagnosticCode::InstrumentAssetClassConflict &&
                       aDiagnostic.mIsin && isins.contains(*aDiagnostic.mIsin);
            }
        };

        if (std::visit(isTaken, aMergeDiagnostics[position]))
        {
            positions.push_back(position);
        }
    }

    return positions;
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

/// The excluded item of an instrument that a tax rule left out on purpose. It has no warning: the
/// summary lists it as information.
void addRuleExclusion(TaxClassificationResult& aResult,
                      ExcludedItem aItem,
                      const EventFacts& aFacts) {
    aResult.mExcludedItems.push_back(ExcludedItemInput{
        .mItem = std::move(aItem),
        .mSources = aFacts.mSources,
        .mProcessingDiagnostics = {},
    });
}

/// The warning and the excluded item of an instrument that the user left out.
void addExclusion(TaxClassificationResult& aResult,
                  ExcludedItem aItem,
                  MessageKey aKey,
                  const EventFacts& aFacts) {
    const auto position = aResult.mDiagnostics.size();
    const auto name = aItem.mName;
    const auto isin = aItem.mIsin;

    aResult.mDiagnostics.push_back(
        classificationDiagnostic(DiagnosticSeverity::Warning,
                                 ProcessingDiagnosticCode::TaxCategoryNotReported,
                                 makeDiagnosticMessage(aKey),
                                 aFacts.mSources,
                                 aFacts.mFirstDate,
                                 name,
                                 isin));
    aResult.mExcludedItems.push_back(ExcludedItemInput{
        .mItem = std::move(aItem),
        .mSources = aFacts.mSources,
        .mProcessingDiagnostics = {position},
    });
}

/// The question of an unknown instrument, the diagnostic that blocks capital gains until it is
/// answered, and what the answer changes.
void classifyUnknown(const TradeInstrument& aInstrument,
                     const ReportingYearLedger& aLedger,
                     RunDecisions& aDecisions,
                     TaxClassificationResult& aResult) {
    const auto facts =
        eventsOf(aInstrument, [&aLedger](Date aDate) { return aDate <= aLedger.mYearEnd; });

    // Nothing up to 31 December means nothing to report this year.
    if (facts.empty())
    {
        return;
    }

    auto labels = classLabelsOf(aInstrument);
    DecisionQuestion question{
        .mType = RequestType::TaxClassificationRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mSources = facts.mSources,
        .mTaxDate = facts.mFirstDate,
        .mIsin = aInstrument.mIsin,
        .mData = TaxClassificationQuestion{.mIsin = aInstrument.mIsin,
                                           .mName = aInstrument.mName,
                                           .mClassLabels = labels},
    };
    const auto* answer = aDecisions.find(question);

    if (answer == nullptr)
    {
        aResult.mRequests.push_back(
            RequestInput{.mQuestion = std::move(question),
                         .mProcessingDiagnostics = {aResult.mDiagnostics.size()}});
        aResult.mDiagnostics.push_back(classificationDiagnostic(
            DiagnosticSeverity::Error,
            ProcessingDiagnosticCode::TaxClassificationUnknown,
            makeDiagnosticMessage(MessageKey::TaxClassificationUnknownConfirmationRequired),
            facts.mSources,
            facts.mFirstDate,
            aInstrument.mName,
            aInstrument.mIsin));

        return;
    }

    switch (choiceOf(*answer))
    {
    case Choice::CapitalGains:
        break;
    case Choice::Cancel:
        aResult.mDiagnostics.push_back(classificationDiagnostic(
            DiagnosticSeverity::Error,
            ProcessingDiagnosticCode::TaxClassificationUnknown,
            makeDiagnosticMessage(MessageKey::TaxClassificationUnknownCancelled),
            facts.mSources,
            facts.mFirstDate,
            aInstrument.mName,
            aInstrument.mIsin));
        break;
    case Choice::Exclude:
        aResult.mLeftOutIsins.insert(aInstrument.mIsin);
        addExclusion(aResult,
                     ExcludedItem{.mIsin = aInstrument.mIsin,
                                  .mName = aInstrument.mName,
                                  .mCategory = ExcludedCategory::Unknown,
                                  .mReason = ExcludedReason::UserChoice,
                                  .mClassLabels = std::move(labels)},
                     MessageKey::TaxCategoryNotReportedUserChoice,
                     facts);
        break;
    }
}

/// A bond's purchases, sales and maturities in the selected year.
struct BondYear {
    std::string mName;
    EventFacts mFacts;
    BondExclusionFacts mUnits;
    /// The class labels of its trade rows; empty for a bond that only has maturity rows.
    std::vector<std::string> mClassLabels;
    bool mOverflow{};
};

/// The bonds with an event in the selected year, by ISIN: the purchases and sales of the trade
/// instruments of class Bond and the maturity rows. The units of a maturity are added up by sum
/// (units out minus units back), never by order.
std::map<Isin, BondYear> bondsOf(const MergedStatement& aStatement,
                                 const ReportingYearLedger& aLedger) {
    const auto& presentation = aStatement.mPresentation;
    std::map<Isin, BondYear> bonds;
    std::map<Isin, AssetClass> tradeClasses;
    std::map<Isin, std::vector<std::string>> tradeLabels;

    for (const auto& instrument : presentation.mTradeInstruments)
    {
        tradeClasses[instrument.mIsin] = instrument.mAssetClass;

        if (instrument.mAssetClass != AssetClass::Bond)
        {
            continue;
        }

        tradeLabels[instrument.mIsin] = classLabelsOf(instrument);

        for (const auto& trade : instrument.mTransactions)
        {
            if (!isInYear(aLedger, trade.mMetadata.mTaxDate))
            {
                continue;
            }

            auto& bond = bonds[instrument.mIsin];
            const bool isSale = trade.mTradeSide == TradeSide::Sell;

            bond.mName = instrument.mName;
            bond.mFacts.add(trade.mMetadata);
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
        bond.mFacts.add(redemption.mMetadata);

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

    for (auto& [isin, bond] : bonds)
    {
        const auto labels = tradeLabels.find(isin);

        if (labels != tradeLabels.end())
        {
            bond.mClassLabels = labels->second;
        }
    }

    return bonds;
}

} // namespace

TaxClassificationResult
TaxClassifier::classify(const MergedStatement& aStatement,
                        const ReportingYearLedger& aLedger,
                        const std::vector<StatementMergeDiagnostic>& aMergeDiagnostics,
                        RunDecisions& aDecisions) {
    const auto& instruments = aStatement.mPresentation.mTradeInstruments;
    TaxClassificationResult result;

    result.mResolvedMergeDiagnostics = takenOverClassErrors(instruments, aMergeDiagnostics);

    for (const auto& instrument : instruments)
    {
        const auto inYear = [&aLedger](Date aDate) { return isInYear(aLedger, aDate); };

        switch (instrument.mAssetClass)
        {
        case AssetClass::Stock:
        case AssetClass::Fund:
        // The capital gates block these two: no treatment exists yet.
        case AssetClass::Derivative:
        case AssetClass::PrivateFund:
            break;
        case AssetClass::Bond:
            // The bond list below has its item.
            result.mLeftOutIsins.insert(instrument.mIsin);
            break;
        case AssetClass::Crypto: {
            result.mLeftOutIsins.insert(instrument.mIsin);

            const auto facts = eventsOf(instrument, inYear);

            if (!facts.empty())
            {
                addRuleExclusion(result,
                                 ExcludedItem{.mIsin = instrument.mIsin,
                                              .mName = instrument.mName,
                                              .mCategory = ExcludedCategory::Crypto,
                                              .mReason = ExcludedReason::CryptoNotReported,
                                              .mClassLabels = classLabelsOf(instrument)},
                                 facts);
            }
            break;
        }
        case AssetClass::Unknown:
            classifyUnknown(instrument, aLedger, aDecisions, result);
            break;
        }
    }

    for (const auto& [isin, bond] : bondsOf(aStatement, aLedger))
    {
        addRuleExclusion(
            result,
            ExcludedItem{.mIsin = isin,
                         .mName = bond.mName,
                         .mCategory = ExcludedCategory::Bond,
                         .mReason = ExcludedReason::DebtSecurity,
                         .mClassLabels = bond.mClassLabels,
                         .mBond = bond.mOverflow ? std::nullopt
                                                 : std::optional<BondExclusionFacts>{bond.mUnits}},
            bond.mFacts);
    }

    return result;
}

void appendClassification(ApplicationResultInput& aInput, TaxClassificationResult&& aResult) {
    const auto offset = aInput.mProcessingDiagnostics.size();

    for (auto& item : aResult.mExcludedItems)
    {
        std::transform(item.mProcessingDiagnostics.begin(),
                       item.mProcessingDiagnostics.end(),
                       item.mProcessingDiagnostics.begin(),
                       [offset](std::size_t aPosition) { return aPosition + offset; });

        aInput.mExcludedItems.push_back(std::move(item));
    }

    aInput.mResolvedMergeDiagnostics.insert(aInput.mResolvedMergeDiagnostics.end(),
                                            aResult.mResolvedMergeDiagnostics.begin(),
                                            aResult.mResolvedMergeDiagnostics.end());
    appendStageOutput(aInput, std::move(aResult.mDiagnostics), std::move(aResult.mRequests));
}

} // namespace taxbroker
