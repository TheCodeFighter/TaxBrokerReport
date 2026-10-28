#include "parsers/traderepublic_parser.hpp"
#include "processors/capital_gains_report_builder.hpp"
#include "processors/fifo_matcher.hpp"
#include "processors/loss_eligibility.hpp"
#include "taxbroker/api/diagnostics_json.hpp"
#include "taxbroker/diagnostic_messages.hpp"
#include "taxbroker/statement_merger.hpp"
#include "taxbroker/types.hpp"
#include "utils/logger.hpp"
#include "xml/kdvp_xml_writer.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <numeric>
#include <ostream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace taxbroker;

std::uint64_t magnitude(std::int64_t aValue) {
    if (aValue >= 0)
    {
        return static_cast<std::uint64_t>(aValue);
    }

    return static_cast<std::uint64_t>(-(aValue + 1)) + 1;
}

int decimalPlaces(std::int64_t aScale) {
    int places = 0;
    while (aScale > 1)
    {
        aScale /= 10;
        ++places;
    }
    return places;
}

void writeFixedPoint(std::ostream& aOutput, std::int64_t aValue, std::int64_t aScale) {
    const auto unsignedScale = static_cast<std::uint64_t>(aScale);
    const auto unsignedValue = magnitude(aValue);

    if (aValue < 0)
    {
        aOutput << '-';
    }

    aOutput << unsignedValue / unsignedScale << '.' << std::setfill('0')
            << std::setw(decimalPlaces(aScale)) << unsignedValue % unsignedScale
            << std::setfill(' ');
}

void writeOptionalFixedPoint(std::ostream& aOutput,
                             std::optional<std::int64_t> aValue,
                             std::int64_t aScale) {
    if (aValue)
    {
        writeFixedPoint(aOutput, *aValue, aScale);
    }
    else
    {
        aOutput << "<unknown>";
    }
}

void writeCalendarDate(std::ostream& aOutput, std::chrono::year_month_day aDate) {
    aOutput << static_cast<int>(aDate.year()) << '-' << std::setfill('0') << std::setw(2)
            << static_cast<unsigned>(aDate.month()) << '-' << std::setw(2)
            << static_cast<unsigned>(aDate.day()) << std::setfill(' ');
}

void writeDate(std::ostream& aOutput, Date aDate) {
    writeCalendarDate(aOutput,
                      std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(aDate)});
}

void writeTimestamp(std::ostream& aOutput, SourceTimestamp aTimestamp) {
    const auto dayPoint = std::chrono::floor<std::chrono::days>(aTimestamp);
    const std::chrono::hh_mm_ss time{aTimestamp - dayPoint};

    writeCalendarDate(aOutput, std::chrono::year_month_day{dayPoint});
    aOutput << 'T' << std::setfill('0') << std::setw(2) << time.hours().count() << ':'
            << std::setw(2) << time.minutes().count() << ':' << std::setw(2)
            << time.seconds().count() << '.' << std::setw(3) << time.subseconds().count() << 'Z'
            << std::setfill(' ');
}

std::string_view toString(TradeSide aTradeSide) {
    switch (aTradeSide)
    {
    case TradeSide::Buy:
        return "Buy";
    case TradeSide::Sell:
        return "Sell";
    }

    return "Unknown";
}

std::string_view toString(Currency aCurrency) {
    switch (aCurrency)
    {
    case Currency::EUR:
        return "EUR";
    case Currency::USD:
        return "USD";
    case Currency::GBP:
        return "GBP";
    case Currency::CHF:
        return "CHF";
    case Currency::JPY:
        return "JPY";
    case Currency::Unknown:
        return "Unknown";
    }

    return "Unknown";
}

std::string_view toString(AssetClass aAssetClass) {
    switch (aAssetClass)
    {
    case AssetClass::Stock:
        return "Stock";
    case AssetClass::Fund:
        return "Fund";
    case AssetClass::Bond:
        return "Bond";
    case AssetClass::Derivative:
        return "Derivative";
    case AssetClass::Crypto:
        return "Crypto";
    case AssetClass::PrivateFund:
        return "PrivateFund";
    case AssetClass::Unknown:
        return "Unknown";
    }

    return "Unknown";
}

std::string_view toString(InterestType aInterestType) {
    switch (aInterestType)
    {
    case InterestType::BondInterest:
        return "BondInterest";
    case InterestType::BrokerInterest:
        return "BrokerInterest";
    case InterestType::OtherInterest:
        return "OtherInterest";
    case InterestType::UnknownInterest:
        return "UnknownInterest";
    }

    return "UnknownInterest";
}

std::string_view toString(CorporateActionType aCorporateActionType) {
    switch (aCorporateActionType)
    {
    case CorporateActionType::Split:
        return "Split";
    case CorporateActionType::ReverseSplit:
        return "ReverseSplit";
    case CorporateActionType::Merger:
        return "Merger";
    case CorporateActionType::UnresolvedSplit:
        return "UnresolvedSplit";
    }

    return "Unknown";
}

std::string_view toString(BenefitType aBenefitType) {
    switch (aBenefitType)
    {
    case BenefitType::Saveback:
        return "Saveback";
    case BenefitType::StockPerk:
        return "StockPerk";
    }

    return "Unknown";
}

std::string_view toString(PrivateMarketEventType aEventType) {
    switch (aEventType)
    {
    case PrivateMarketEventType::Buy:
        return "Buy";
    case PrivateMarketEventType::Sell:
        return "Sell";
    case PrivateMarketEventType::Bonus:
        return "Bonus";
    }

    return "Unknown";
}

std::string_view toString(Broker aBroker) {
    switch (aBroker)
    {
    case Broker::TradeRepublic:
        return "TradeRepublic";
    case Broker::InteractiveBrokers:
        return "InteractiveBrokers";
    case Broker::Unknown:
        return "Unknown";
    }

    return "Unknown";
}

void writeMetadata(std::ostream& aOutput,
                   const EventMetadata& aMetadata,
                   std::string_view aIndent) {
    aOutput << "tax_date: ";
    writeDate(aOutput, aMetadata.mTaxDate);
    aOutput << '\n' << aIndent << "source_timestamp: ";
    if (aMetadata.mSourceTimestamp)
    {
        writeTimestamp(aOutput, *aMetadata.mSourceTimestamp);
    }
    else
    {
        aOutput << "<none>";
    }

    if (aMetadata.mOrderingTimestamp)
    {
        aOutput << '\n' << aIndent << "ordering_timestamp (assumed): ";
        writeTimestamp(aOutput, *aMetadata.mOrderingTimestamp);
    }

    aOutput << '\n' << aIndent << "sources: " << aMetadata.mSources.size();
    for (const auto& source : aMetadata.mSources)
    {
        aOutput << '\n'
                << aIndent << "  - broker: " << toString(source.mBroker) << '\n'
                << aIndent << "    source_file: " << source.mFilename.value() << '\n'
                << aIndent << "    source_row: " << source.mSourceRow << '\n'
                << aIndent << "    transaction_id: " << source.mTransactionId.value_or("<none>")
                << '\n'
                << aIndent << "    source_index: " << source.mInputSequence.mSourceIndex << '\n'
                << aIndent << "    event_index: " << source.mInputSequence.mEventIndex;
    }
}

std::string safeSourceFile(std::string_view aSourceFile) {
    if (aSourceFile.empty())
    {
        return "<none>";
    }
    return SourceFilename::fromPath(aSourceFile).value();
}

std::string_view toString(DiagnosticSeverity aSeverity) {
    switch (aSeverity)
    {
    case DiagnosticSeverity::Warning:
        return "Warning";
    case DiagnosticSeverity::Error:
        return "Error";
    }

    return "Error";
}

std::string_view toString(DiagnosticCode aDiagnosticCode) {
    switch (aDiagnosticCode)
    {
    case DiagnosticCode::UnknownRowType:
        return "UnknownRowType";
    case DiagnosticCode::UnsupportedRowType:
        return "UnsupportedRowType";
    case DiagnosticCode::UnsupportedAssetClass:
        return "UnsupportedAssetClass";
    case DiagnosticCode::MissingField:
        return "MissingField";
    case DiagnosticCode::InvalidValue:
        return "InvalidValue";
    case DiagnosticCode::InconsistentValue:
        return "InconsistentValue";
    case DiagnosticCode::FieldCountMismatch:
        return "FieldCountMismatch";
    case DiagnosticCode::LocalizedNumberFormat:
        return "LocalizedNumberFormat";
    case DiagnosticCode::InvalidHeader:
        return "InvalidHeader";
    case DiagnosticCode::ParseError:
        return "ParseError";
    }

    return "Unknown";
}

std::string_view toString(MergeDiagnosticCode aDiagnosticCode) {
    switch (aDiagnosticCode)
    {
    case MergeDiagnosticCode::DuplicateSourceIndex:
        return "DuplicateSourceIndex";
    case MergeDiagnosticCode::InconsistentSourceIndex:
        return "InconsistentSourceIndex";
    case MergeDiagnosticCode::ConflictingDuplicate:
        return "ConflictingDuplicate";
    case MergeDiagnosticCode::InstrumentNameConflict:
        return "InstrumentNameConflict";
    case MergeDiagnosticCode::InstrumentAssetClassConflict:
        return "InstrumentAssetClassConflict";
    case MergeDiagnosticCode::DuplicateTransactionInFile:
        return "DuplicateTransactionInFile";
    }

    return "Unknown";
}

std::string diagnosticLocations(const MergeDiagnostic& aDiagnostic) {
    std::ostringstream locations;

    if (aDiagnostic.mSources.empty())
    {
        if (aDiagnostic.mSourceIndex)
        {
            locations << "source " << *aDiagnostic.mSourceIndex;
        }
        else
        {
            locations << "no source location";
        }

        return locations.str();
    }

    for (std::size_t index = 0; index < aDiagnostic.mSources.size(); ++index)
    {
        if (index > 0)
        {
            locations << "; ";
        }

        const auto& source = aDiagnostic.mSources[index];
        locations << "source " << source.mInputSequence.mSourceIndex << " row "
                  << source.mSourceRow;
    }

    return locations.str();
}

void logMergeDiagnostics(const StatementMergeResult& aMergeResult) {
    for (const auto& diagnostic : aMergeResult.mDiagnostics)
    {
        const auto* mergeDiagnostic = std::get_if<MergeDiagnostic>(&diagnostic);

        if (!mergeDiagnostic)
        {
            continue;
        }

        const auto locations = diagnosticLocations(*mergeDiagnostic);

        if (mergeDiagnostic->mSeverity == DiagnosticSeverity::Warning)
        {
            LOG_WARNING("Statement merge {} at {}: {}",
                        toString(mergeDiagnostic->mCode),
                        locations,
                        mergeDiagnostic->mMessage);
        }
        else
        {
            LOG_ERROR("Statement merge {} at {}: {}",
                      toString(mergeDiagnostic->mCode),
                      locations,
                      mergeDiagnostic->mMessage);
        }
    }
}

void writeUnitEvidence(std::ostream& aOutput, const std::vector<UnitSourceEvidence>& aEvidence) {
    for (const auto& evidence : aEvidence)
    {
        aOutput << "      unit_source: " << evidence.mSource.mFilename.value() << ':'
                << evidence.mSource.mSourceRow
                << "\n        original_text: " << std::quoted(evidence.mSourceText)
                << "\n        discarded_digits: " << std::quoted(evidence.mDiscardedDigits)
                << "\n        canonical_value: " << evidence.mCanonicalValue
                << "\n        rounded_away_from_zero: "
                << (evidence.mRoundedAwayFromZero ? "true" : "false") << '\n';
    }
}

/// Writes original instrument names and class labels with their sources for local inspection.
void writeInstrumentEvidence(std::ostream& aOutput,
                             const std::vector<InstrumentSourceEvidence>& aEvidence) {
    for (const auto& evidence : aEvidence)
    {
        aOutput << "      instrument_source: " << evidence.mSource.mFilename.value() << ':'
                << evidence.mSource.mSourceRow
                << "\n        original_name: " << std::quoted(evidence.mName)
                << "\n        original_class: " << std::quoted(evidence.mAssetClassLabel)
                << "\n        mapped_class: " << toString(evidence.mAssetClass) << '\n';
    }
}

void writeTrades(std::ostream& aOutput, const BrokerStatement& aStatement) {
    aOutput << "TRADE INSTRUMENTS: " << aStatement.mTradeInstruments.size() << "\n\n";

    for (const auto& instrument : aStatement.mTradeInstruments)
    {
        aOutput << "Instrument\n"
                << "  name: " << instrument.mName << '\n'
                << "  isin: " << instrument.mIsin << '\n'
                << "  asset_class: " << toString(instrument.mAssetClass) << '\n'
                << "  transactions: " << instrument.mTransactions.size() << '\n';

        for (const auto& transaction : instrument.mTransactions)
        {
            aOutput << "    - ";
            writeMetadata(aOutput, transaction.mMetadata, "      ");
            aOutput << "\n      side: " << toString(transaction.mTradeSide)
                    << "\n      unit_price: ";
            writeFixedPoint(aOutput, transaction.mUnitPrice, UNIT_PRICE_SCALE);
            aOutput << "\n      units: ";
            writeFixedPoint(aOutput, transaction.mUnits, UNITS_SCALE);
            aOutput << "\n      amount: ";
            if (transaction.mAmount)
            {
                writeFixedPoint(aOutput, *transaction.mAmount, MONEY_SCALE);
            }
            else
            {
                aOutput << "<none>";
            }
            aOutput << "\n      fee_paid: ";
            writeFixedPoint(aOutput, transaction.mFeePaid, MONEY_SCALE);
            aOutput << "\n      exchange_rate: ";
            writeOptionalFixedPoint(aOutput, transaction.mExchangeRate, EXCHANGE_RATE_SCALE);
            aOutput << "\n      currency: " << toString(transaction.mCurrency)
                    << "\n      fee_currency: " << toString(transaction.mFeeCurrency) << '\n';
            writeUnitEvidence(aOutput, transaction.mUnitEvidence);
            writeInstrumentEvidence(aOutput, transaction.mInstrumentEvidence);
        }

        aOutput << "  corporate_actions: " << instrument.mCorporateActions.size() << '\n';

        for (const auto& action : instrument.mCorporateActions)
        {
            aOutput << "    - ";
            writeMetadata(aOutput, action.mMetadata, "      ");
            aOutput << "\n      type: " << toString(action.mType)
                    << (action.mType == CorporateActionType::UnresolvedSplit
                            ? "\n      source_quantity: "
                            : "\n      units_delta: ");
            writeFixedPoint(aOutput, action.mUnitsDelta, UNITS_SCALE);
            aOutput << "\n      ratio: ";
            if (action.mRatio)
            {
                writeFixedPoint(aOutput, *action.mRatio, CORP_RATIO_SCALE);
            }
            else
            {
                aOutput << "<unresolved>";
            }
            aOutput << '\n';
            writeUnitEvidence(aOutput, action.mUnitEvidence);
            writeInstrumentEvidence(aOutput, action.mInstrumentEvidence);
        }

        aOutput << '\n';
    }
}

void writeDividends(std::ostream& aOutput, const BrokerStatement& aStatement) {
    aOutput << "DIVIDEND INSTRUMENTS: " << aStatement.mDividendInstruments.size() << "\n\n";

    for (const auto& instrument : aStatement.mDividendInstruments)
    {
        aOutput << "Instrument\n"
                << "  name: " << instrument.mName << '\n'
                << "  isin: " << instrument.mIsin << '\n'
                << "  transactions: " << instrument.mTransactions.size() << '\n';

        for (const auto& transaction : instrument.mTransactions)
        {
            aOutput << "    - ";
            writeMetadata(aOutput, transaction.mMetadata, "      ");
            aOutput << "\n      gross_amount: ";
            writeFixedPoint(aOutput, transaction.mGrossAmount, MONEY_SCALE);
            aOutput << "\n      tax_paid: ";
            writeOptionalFixedPoint(aOutput, transaction.mTaxPaid, MONEY_SCALE);
            aOutput << "\n      exchange_rate: ";
            writeOptionalFixedPoint(aOutput, transaction.mExchangeRate, EXCHANGE_RATE_SCALE);
            aOutput << "\n      currency: " << toString(transaction.mCurrency)
                    << "\n      tax_currency: " << toString(transaction.mTaxCurrency) << '\n';
            writeInstrumentEvidence(aOutput, transaction.mInstrumentEvidence);
        }

        aOutput << '\n';
    }
}

void writeInterests(std::ostream& aOutput, const BrokerStatement& aStatement) {
    aOutput << "INTEREST INSTRUMENTS: " << aStatement.mInterestInstruments.size() << "\n\n";

    for (const auto& instrument : aStatement.mInterestInstruments)
    {
        aOutput << "Instrument\n"
                << "  name: " << instrument.mName << '\n'
                << "  isin: " << instrument.mIsin.value_or("<none>") << '\n'
                << "  interest_type: " << toString(instrument.mInterestType) << '\n'
                << "  transactions: " << instrument.mTransactions.size() << '\n';

        for (const auto& transaction : instrument.mTransactions)
        {
            aOutput << "    - ";
            writeMetadata(aOutput, transaction.mMetadata, "      ");
            aOutput << "\n      gross_amount: ";
            writeFixedPoint(aOutput, transaction.mGrossAmount, MONEY_SCALE);
            aOutput << "\n      tax_paid: ";
            writeOptionalFixedPoint(aOutput, transaction.mTaxPaid, MONEY_SCALE);
            aOutput << "\n      exchange_rate: ";
            writeOptionalFixedPoint(aOutput, transaction.mExchangeRate, EXCHANGE_RATE_SCALE);
            aOutput << "\n      currency: " << toString(transaction.mCurrency)
                    << "\n      tax_currency: " << toString(transaction.mTaxCurrency) << '\n';
            writeInstrumentEvidence(aOutput, transaction.mInstrumentEvidence);
        }

        aOutput << '\n';
    }
}

void writeBenefits(std::ostream& aOutput, const BrokerStatement& aStatement) {
    aOutput << "BENEFIT EVENTS: " << aStatement.mBenefitEvents.size() << "\n\n";

    for (const auto& benefit : aStatement.mBenefitEvents)
    {
        aOutput << "- ";
        writeMetadata(aOutput, benefit.mMetadata, "  ");
        aOutput << "\n  type: " << toString(benefit.mType)
                << "\n  name: " << (benefit.mName.empty() ? "<none>" : benefit.mName)
                << "\n  isin: " << benefit.mIsin.value_or("<none>")
                << "\n  asset_class: " << toString(benefit.mAssetClass) << "\n  amount: ";
        writeFixedPoint(aOutput, benefit.mAmount, MONEY_SCALE);
        aOutput << "\n  currency: " << toString(benefit.mCurrency) << "\n\n";
    }
}

void writePrivateMarketEvents(std::ostream& aOutput, const BrokerStatement& aStatement) {
    aOutput << "PRIVATE MARKET EVENTS: " << aStatement.mPrivateMarketEvents.size() << "\n\n";

    for (const auto& event : aStatement.mPrivateMarketEvents)
    {
        aOutput << "- ";
        writeMetadata(aOutput, event.mMetadata, "  ");
        aOutput << "\n  type: " << toString(event.mType)
                << "\n  name: " << (event.mName.empty() ? "<none>" : event.mName)
                << "\n  isin: " << event.mIsin.value_or("<none>")
                << "\n  asset_class: " << toString(event.mAssetClass) << "\n  amount: ";
        writeFixedPoint(aOutput, event.mAmount, MONEY_SCALE);
        aOutput << "\n  fee_paid: ";
        writeFixedPoint(aOutput, event.mFeePaid, MONEY_SCALE);
        aOutput << "\n  currency: " << toString(event.mCurrency)
                << "\n  description: " << event.mDescription << "\n\n";
    }
}

/// Writes the message key and parameters of a diagnostic, or `<none>` for a bare diagnostic.
void writeMessageKey(std::ostream& aOutput,
                     const std::optional<MessageKey>& aKey,
                     const std::vector<MessageParam>& aParams) {
    aOutput << "\n  message_key: "
            << (aKey ? std::string{messageDefinition(*aKey).mId} : std::string{"<none>"})
            << "\n  message_params: ";

    if (aParams.empty())
    {
        aOutput << "<none>";
    }

    for (std::size_t index = 0; index < aParams.size(); ++index)
    {
        const auto writeValue = [&aOutput](const auto& aValue) { aOutput << aValue; };

        aOutput << (index == 0 ? "" : ", ") << aParams[index].mName << '=';
        std::visit(writeValue, aParams[index].mValue);
    }
}

void writeDiagnostics(std::ostream& aOutput, const ParseResult& aParseResult) {
    aOutput << "DIAGNOSTICS: " << aParseResult.mDiagnostics.size() << "\n\n";

    for (const auto& diagnostic : aParseResult.mDiagnostics)
    {
        aOutput << "- severity: " << toString(diagnostic.mSeverity)
                << "\n  code: " << toString(diagnostic.mCode)
                << "\n  source: " << safeSourceFile(diagnostic.mSourceFile) << "\n  row: ";
        if (diagnostic.mRowIndex)
        {
            aOutput << *diagnostic.mRowIndex;
        }
        else
        {
            aOutput << "<none>";
        }
        aOutput << "\n  transaction_id: " << diagnostic.mTransactionId.value_or("<none>")
                << "\n  field: " << diagnostic.mField.value_or("<none>")
                << "\n  message: " << diagnostic.mMessage;
        writeMessageKey(aOutput, diagnostic.mMessageKey, diagnostic.mMessageParams);
        aOutput << "\n\n";
    }
}

void writeParseResult(std::ostream& aOutput, const ParseResult& aParseResult) {
    writeTrades(aOutput, aParseResult.mStatement);
    writeDividends(aOutput, aParseResult.mStatement);
    writeInterests(aOutput, aParseResult.mStatement);
    writeBenefits(aOutput, aParseResult.mStatement);
    writePrivateMarketEvents(aOutput, aParseResult.mStatement);
    writeDiagnostics(aOutput, aParseResult);
}

std::size_t eventCount(const BrokerStatement& aStatement) {
    const auto tradeCount = std::accumulate(
        aStatement.mTradeInstruments.begin(),
        aStatement.mTradeInstruments.end(),
        std::size_t{},
        [](std::size_t aCount, const TradeInstrument& aInstrument) {
            return aCount + aInstrument.mTransactions.size() + aInstrument.mCorporateActions.size();
        });

    const auto dividendCount =
        std::accumulate(aStatement.mDividendInstruments.begin(),
                        aStatement.mDividendInstruments.end(),
                        std::size_t{},
                        [](std::size_t aCount, const DividendInstrument& aInstrument) {
                            return aCount + aInstrument.mTransactions.size();
                        });

    const auto interestCount =
        std::accumulate(aStatement.mInterestInstruments.begin(),
                        aStatement.mInterestInstruments.end(),
                        std::size_t{},
                        [](std::size_t aCount, const InterestInstrument& aInstrument) {
                            return aCount + aInstrument.mTransactions.size();
                        });

    return aStatement.mBenefitEvents.size() + aStatement.mPrivateMarketEvents.size() + tradeCount +
           dividendCount + interestCount;
}

void createParentDirectory(const std::filesystem::path& aPath) {
    if (aPath.has_parent_path())
    {
        std::filesystem::create_directories(aPath.parent_path());
    }
}

void writeMergeDiagnostics(std::ostream& aOutput, const StatementMergeResult& aMergeResult) {
    aOutput << "DIAGNOSTICS: " << aMergeResult.mDiagnostics.size() << "\n\n";

    const auto writeDiagnostic = [&aOutput](const auto& aDiagnostic) {
        using Diagnostic = std::decay_t<decltype(aDiagnostic)>;

        if constexpr (std::is_same_v<Diagnostic, SourcedParseDiagnostic>)
        {
            const auto& diagnostic = aDiagnostic.mDiagnostic;

            aOutput << "- stage: Parsing\n"
                    << "  source_index: " << aDiagnostic.mSourceIndex
                    << "\n  broker: " << toString(aDiagnostic.mBroker)
                    << "\n  severity: " << toString(diagnostic.mSeverity)
                    << "\n  code: " << toString(diagnostic.mCode)
                    << "\n  source: " << safeSourceFile(diagnostic.mSourceFile) << "\n  row: ";

            if (diagnostic.mRowIndex)
            {
                aOutput << *diagnostic.mRowIndex;
            }
            else
            {
                aOutput << "<none>";
            }

            aOutput << "\n  transaction_id: " << diagnostic.mTransactionId.value_or("<none>")
                    << "\n  field: " << diagnostic.mField.value_or("<none>")
                    << "\n  message: " << diagnostic.mMessage;
            writeMessageKey(aOutput, diagnostic.mMessageKey, diagnostic.mMessageParams);
            aOutput << "\n\n";
        }
        else if constexpr (std::is_same_v<Diagnostic, MergeDiagnostic>)
        {
            aOutput << "- stage: Merging\n"
                    << "  severity: " << toString(aDiagnostic.mSeverity)
                    << "\n  code: " << toString(aDiagnostic.mCode)
                    << "\n  message: " << aDiagnostic.mMessage;
            writeMessageKey(aOutput, aDiagnostic.mMessageKey, aDiagnostic.mMessageParams);
            aOutput << "\n  instrument: " << aDiagnostic.mInstrumentName.value_or("<none>")
                    << "\n  isin: " << aDiagnostic.mIsin.value_or("<none>") << "\n  tax_date: ";

            if (aDiagnostic.mTaxDate)
            {
                writeDate(aOutput, *aDiagnostic.mTaxDate);
            }
            else
            {
                aOutput << "<none>";
            }

            aOutput << "\n  sources: " << aDiagnostic.mSources.size() << '\n';

            for (const auto& source : aDiagnostic.mSources)
            {
                aOutput << "    - source_index: " << source.mInputSequence.mSourceIndex
                        << "\n      event_index: " << source.mInputSequence.mEventIndex
                        << "\n      broker: " << toString(source.mBroker)
                        << "\n      source_file: " << source.mFilename.value()
                        << "\n      source_row: " << source.mSourceRow
                        << "\n      transaction_id: " << source.mTransactionId.value_or("<none>")
                        << '\n';
            }

            aOutput << '\n';
        }
        else
        {
            static_assert(std::is_same_v<Diagnostic, void>,
                          "Missing diagnostic text handling for this diagnostic type.");
        }
    };

    for (const auto& diagnostic : aMergeResult.mDiagnostics)
    {
        std::visit(writeDiagnostic, diagnostic);
    }
}

/// Parses each CSV path as one merge input; the source index is the path's position.
std::vector<StatementMergeInput> parseInputs(std::span<char*> aCsvPaths) {
    std::vector<StatementMergeInput> inputs;
    inputs.reserve(aCsvPaths.size());

    taxbroker::tr::TradeRepublicParser parser;

    for (std::size_t index = 0; index < aCsvPaths.size(); ++index)
    {
        inputs.push_back(StatementMergeInput{
            .mSourceIndex = index,
            .mParseResult = parser.parse(aCsvPaths[index], index),
        });
    }

    return inputs;
}

int writeMergedResult(std::span<char*> aArguments, bool aDiagnosticsOnly = false) {
    const std::filesystem::path outputPath{aArguments[0]};
    auto inputs = parseInputs(aArguments.subspan(1));
    const auto parsedEventCount =
        std::accumulate(inputs.begin(),
                        inputs.end(),
                        std::size_t{},
                        [](std::size_t aCount, const StatementMergeInput& aInput) {
                            return aCount + eventCount(aInput.mParseResult.mStatement);
                        });

    const auto mergeResult = DeterministicStatementMerger{}.merge(std::move(inputs));

    logMergeDiagnostics(mergeResult);

    const auto mergedEventCount = eventCount(mergeResult.mStatement.mPresentation);

    createParentDirectory(outputPath);
    std::ofstream output{outputPath};

    if (!output)
    {
        std::cerr << "Failed to open output file: " << outputPath << '\n';
        return 1;
    }

    if (aDiagnosticsOnly)
    {
        writeMergeDiagnostics(output, mergeResult);

        if (!output)
        {
            std::cerr << "Failed to write diagnostics file: " << outputPath << '\n';
            return 1;
        }

        std::cout << "Wrote Trade Republic diagnostics to " << outputPath << '\n';

        return 0;
    }

    output << "MERGE SUMMARY\n"
           << "  input_files: " << inputs.size() << "\n  parsed_events: " << parsedEventCount
           << "\n  merged_events: " << mergedEventCount << "\n  all_parsed_events_retained: "
           << (parsedEventCount == mergedEventCount ? "yes" : "no")
           << "\n  diagnostics: " << mergeResult.mDiagnostics.size() << "\n\n"
           << "INPUT FILES: " << inputs.size() << "\n\n";

    for (const auto& input : inputs)
    {
        output << "- source_index: " << input.mSourceIndex << "\n  source_file: "
               << SourceFilename::fromPath(aArguments[input.mSourceIndex + 1]).value() << '\n';
    }

    output << '\n';

    writeTrades(output, mergeResult.mStatement.mPresentation);
    writeDividends(output, mergeResult.mStatement.mPresentation);
    writeInterests(output, mergeResult.mStatement.mPresentation);
    writeBenefits(output, mergeResult.mStatement.mPresentation);
    writePrivateMarketEvents(output, mergeResult.mStatement.mPresentation);
    writeMergeDiagnostics(output, mergeResult);

    if (!output)
    {
        std::cerr << "Failed to write merged output file: " << outputPath << '\n';
        return 1;
    }

    std::cout << "Wrote merged Trade Republic data to " << outputPath << '\n';

    return 0;
}

std::string_view toString(FifoIsinStatus aStatus) {
    switch (aStatus)
    {
    case FifoIsinStatus::Matched:
        return "Matched";
    case FifoIsinStatus::CorporateAction:
        return "CorporateAction";
    case FifoIsinStatus::NonEurTrade:
        return "NonEurTrade";
    case FifoIsinStatus::InsufficientUnits:
        return "InsufficientUnits";
    case FifoIsinStatus::NumericFailure:
        return "NumericFailure";
    }

    return "Unknown";
}

std::string_view toString(ProcessingDiagnosticCode aCode) {
    switch (aCode)
    {
    case ProcessingDiagnosticCode::UnrepresentableValue:
        return "UnrepresentableValue";
    case ProcessingDiagnosticCode::ArithmeticOverflow:
        return "ArithmeticOverflow";
    case ProcessingDiagnosticCode::InvalidExchangeRate:
        return "InvalidExchangeRate";
    case ProcessingDiagnosticCode::CorporateActionUnrepresentable:
        return "CorporateActionUnrepresentable";
    case ProcessingDiagnosticCode::InvalidReportingYear:
        return "InvalidReportingYear";
    case ProcessingDiagnosticCode::IncompleteHistory:
        return "IncompleteHistory";
    case ProcessingDiagnosticCode::XmlValueOutOfRange:
        return "XmlValueOutOfRange";
    case ProcessingDiagnosticCode::UnsupportedTaxTreatment:
        return "UnsupportedTaxTreatment";
    case ProcessingDiagnosticCode::TaxClassificationUnknown:
        return "TaxClassificationUnknown";
    case ProcessingDiagnosticCode::CorporateActionNotApplied:
        return "CorporateActionNotApplied";
    case ProcessingDiagnosticCode::MissingExchangeRate:
        return "MissingExchangeRate";
    case ProcessingDiagnosticCode::LossEligibilityUnresolved:
        return "LossEligibilityUnresolved";
    }

    return "Unknown";
}

/// Writes exact basis ticks as EUR rounded to eight decimals for reading; nothing is reported.
void writeTicksInEur(std::ostream& aOutput, const ExactRational& aTicks) {
    const auto eur = basisInEur(ExactBasis{aTicks});

    if (const auto* exactEur = std::get_if<ExactRational>(&eur))
    {
        const auto scaled = roundToScaled(*exactEur, UNITS_SCALE);

        if (const auto* value = std::get_if<std::int64_t>(&scaled))
        {
            writeFixedPoint(aOutput, *value, UNITS_SCALE);
            return;
        }
    }

    aOutput << "<out of display range>";
}

/// Writes a ledger event's tax date and source rows; FIFO events are trades or corporate actions.
void writeCapitalEvent(std::ostream& aOutput,
                       const BrokerStatement& aStatement,
                       const StatementEventReference& aEvent) {
    const auto& instrument = aStatement.mTradeInstruments.at(aEvent.mInstrumentIndex.value());
    const auto& metadata = aEvent.mKind == StatementEventKind::CorporateAction
                               ? instrument.mCorporateActions.at(aEvent.mEventIndex).mMetadata
                               : instrument.mTransactions.at(aEvent.mEventIndex).mMetadata;

    writeDate(aOutput, metadata.mTaxDate);

    for (std::size_t index = 0; index < metadata.mSources.size(); ++index)
    {
        const auto& source = metadata.mSources[index];
        aOutput << (index == 0 ? " " : "; ") << source.mFilename.value() << ':'
                << source.mSourceRow;
    }
}

void writeFifoIsin(std::ostream& aOutput,
                   const BrokerStatement& aStatement,
                   const FifoIsinResult& aIsin) {
    aOutput << "Instrument\n"
            << "  name: " << aIsin.mName << "\n  isin: " << aIsin.mIsin
            << "\n  status: " << toString(aIsin.mStatus);

    if (aIsin.mStoppedAt)
    {
        aOutput << "\n  stopped_at: ";
        writeCapitalEvent(aOutput, aStatement, *aIsin.mStoppedAt);
    }

    if (aIsin.mAvailableUnits)
    {
        aOutput << "\n  available_units: ";
        writeFixedPoint(aOutput, *aIsin.mAvailableUnits, UNITS_SCALE);
    }

    aOutput << "\n  matches: " << aIsin.mMatches.size() << '\n';

    for (const auto& match : aIsin.mMatches)
    {
        aOutput << "    - sale: ";
        writeCapitalEvent(aOutput, aStatement, match.mSale);
        aOutput << "\n      purchase: ";
        writeCapitalEvent(aOutput, aStatement, match.mPurchase);
        aOutput << "\n      units: ";
        writeFixedPoint(aOutput, match.mUnits, UNITS_SCALE);
        aOutput << "\n      basis_eur: ";
        writeTicksInEur(aOutput, match.mBasis.mTicks);
        aOutput << "\n      sale_value_eur: ";
        writeTicksInEur(aOutput, match.mSaleValueTicks);
        aOutput << "\n      gain_eur: ";
        writeTicksInEur(aOutput, match.mGainTicks);
        aOutput << (match.mGainTicks.numerator() < 0 ? "  (loss-making portion)" : "");
        aOutput << '\n';
    }

    aOutput << "  open_lots: " << aIsin.mOpenLots.size() << '\n';

    for (const auto& lot : aIsin.mOpenLots)
    {
        aOutput << "    - purchase: ";
        writeCapitalEvent(aOutput, aStatement, lot.mPurchase);
        aOutput << "\n      units: ";
        writeFixedPoint(aOutput, lot.mState.mRemainingUnits, UNITS_SCALE);
        aOutput << "\n      basis_eur: ";
        writeTicksInEur(aOutput, lot.mState.mRemainingBasis.mTicks);
        aOutput << '\n';
    }

    aOutput << '\n';
}

std::string_view toString(KdvpBlockReason aReason) {
    switch (aReason)
    {
    case KdvpBlockReason::FifoStopped:
        return "FifoStopped";
    case KdvpBlockReason::LossNotReportable:
        return "LossNotReportable";
    }

    return "Unknown";
}

std::string_view toString(LossSaleState aState) {
    switch (aState)
    {
    case LossSaleState::Decided:
        return "Decided";
    case LossSaleState::WaitingForAnswer:
        return "WaitingForAnswer";
    case LossSaleState::Cancelled:
        return "Cancelled";
    case LossSaleState::CorporateActionInWindow:
        return "CorporateActionInWindow";
    }

    return "Unknown";
}

/// Writes the 30-day loss rule result of each sale of the year with a loss: the investment, the
/// sale's date, file and row, and units and dates. The tool has no answers, so it treats the
/// open units as blocked, like the CLI.
void writeLossEligibility(std::ostream& aOutput,
                          const BrokerStatement& aStatement,
                          const LossEligibilityResult& aLoss) {
    const auto saleCount = std::accumulate(aLoss.mIsins.begin(),
                                           aLoss.mIsins.end(),
                                           std::size_t{0},
                                           [](std::size_t aSum, const LossEligibilityIsin& aIsin) {
                                               return aSum + aIsin.mSales.size();
                                           });

    aOutput << "LOSS RULE (30 days)\n  loss_sales: " << saleCount
            << "\n  open_questions: " << aLoss.mRequests.size() << '\n';

    for (const auto state : {LossSaleState::Decided,
                             LossSaleState::WaitingForAnswer,
                             LossSaleState::Cancelled,
                             LossSaleState::CorporateActionInWindow})
    {
        std::size_t count{};

        for (const auto& isin : aLoss.mIsins)
        {
            count += static_cast<std::size_t>(std::count_if(
                isin.mSales.begin(),
                isin.mSales.end(),
                [state](const LossSaleEligibility& aSale) { return aSale.mState == state; }));
        }

        aOutput << "  state_" << toString(state) << ": " << count << '\n';
    }

    aOutput << '\n';

    for (const auto& isin : aLoss.mIsins)
    {
        for (const auto& sale : isin.mSales)
        {
            aOutput << "- name: " << isin.mName << "\n  isin: " << isin.mIsin << "\n  sale: ";
            writeCapitalEvent(aOutput, aStatement, sale.mSale);
            aOutput << "\n  window: ";
            writeDate(aOutput, sale.mWindowStart);
            aOutput << " to ";
            writeDate(aOutput, sale.mWindowEnd);
            aOutput << "\n  loss_units: ";
            writeFixedPoint(aOutput, sale.mLossUnits, UNITS_SCALE);
            aOutput << "\n  known_ineligible_units: ";
            writeFixedPoint(aOutput, sale.mKnownIneligibleUnits, UNITS_SCALE);
            aOutput << "\n  open_units: ";
            writeFixedPoint(aOutput, sale.mLossUnits - sale.mKnownIneligibleUnits, UNITS_SCALE);
            aOutput << "\n  assumed_blocked_units: ";
            writeFixedPoint(aOutput,
                            sale.mAssumedBlocked ? sale.mDeclaredIneligibleUnits : Units{0},
                            UNITS_SCALE);
            aOutput << "\n  state: " << toString(sale.mState)
                    << "\n  replacements: " << sale.mReplacements.size() << '\n';

            for (const auto& replacement : sale.mReplacements)
            {
                aOutput << "    - ";
                writeDate(aOutput, replacement.mDate);
                aOutput << ": ";
                writeFixedPoint(aOutput, replacement.mUnits, UNITS_SCALE);
                aOutput << '\n';
            }
        }
    }

    aOutput << '\n';
}

std::string_view toString(KdvpLossRule aLossRule) {
    switch (aLossRule)
    {
    case KdvpLossRule::NotBlocked:
        return "NotBlocked";
    case KdvpLossRule::Blocked:
        return "Blocked";
    }

    return "Unknown";
}

/// Writes one *popisni list* with its rows; each row names its source trade.
void writeKdvpList(std::ostream& aOutput,
                   const BrokerStatement& aStatement,
                   const KdvpInventoryList& aList) {
    aOutput << "List\n"
            << "  name: " << aList.mName << "\n  isin: " << aList.mIsin
            << "\n  is_fund: " << (aList.mIsFund ? "true" : "false")
            << "\n  rows: " << aList.mRows.size() << '\n';

    for (const auto& row : aList.mRows)
    {
        if (const auto* purchase = std::get_if<KdvpPurchaseRow>(&row.mEntry))
        {
            aOutput << "    - purchase (F1): ";
            writeCapitalEvent(aOutput, aStatement, purchase->mPurchase);
            aOutput << "\n      F3_units: ";
            writeFixedPoint(aOutput, purchase->mUnits, UNITS_SCALE);
            aOutput << "\n      F4_unit_price: ";
            writeFixedPoint(aOutput, purchase->mUnitPrice, UNIT_PRICE_SCALE);
        }
        else
        {
            const auto& sale = std::get<KdvpSaleRow>(row.mEntry);

            aOutput << "    - sale (F6): ";
            writeCapitalEvent(aOutput, aStatement, sale.mSale);
            aOutput << "\n      F7_units: ";
            writeFixedPoint(aOutput, sale.mUnits, UNITS_SCALE);
            aOutput << "\n      F9_unit_price: ";
            writeFixedPoint(aOutput, sale.mUnitPrice, UNIT_PRICE_SCALE);
            aOutput << "\n      F10: " << toString(sale.mLossRule);
        }

        aOutput << "\n      F8_stock: ";
        writeFixedPoint(aOutput, row.mStock, UNITS_SCALE);
        aOutput << '\n';
    }

    aOutput << '\n';
}

/// Writes one blocked ISIN with its reason and the sales of the year it names.
void writeKdvpBlockedIsin(std::ostream& aOutput,
                          const BrokerStatement& aStatement,
                          const KdvpBlockedIsin& aBlocked) {
    aOutput << "Blocked\n"
            << "  name: " << aBlocked.mName << "\n  isin: " << aBlocked.mIsin
            << "\n  reason: " << toString(aBlocked.mReason)
            << "\n  sales: " << aBlocked.mSales.size() << '\n';

    for (const auto& sale : aBlocked.mSales)
    {
        aOutput << "    - ";
        writeCapitalEvent(aOutput, aStatement, sale);
        aOutput << '\n';
    }

    aOutput << '\n';
}

/// Writes the Doh-KDVP filing rows. The builder gives no report while any ISIN is blocked, so the
/// lists come from a second build that leaves out every blocked ISIN; that build is a preview for
/// this tool only, not a choice the user made.
void writeKdvpReport(std::ostream& aOutput,
                     const MergedStatement& aStatement,
                     const ReportingYearLedger& aLedger,
                     const FifoMatchingResult& aFifo,
                     const LossEligibilityResult& aLoss) {
    const CapitalGainsReportBuilder builder;
    const auto withoutExclusions = builder.build(aStatement, aLedger, aFifo, aLoss, {});
    std::vector<Isin> blockedIsins;

    std::transform(withoutExclusions.mBlocked.begin(),
                   withoutExclusions.mBlocked.end(),
                   std::back_inserter(blockedIsins),
                   [](const KdvpBlockedIsin& aBlocked) { return aBlocked.mIsin; });

    const auto preview = builder.build(aStatement, aLedger, aFifo, aLoss, blockedIsins);
    const auto& lists = preview.mReport.value().mLists;

    aOutput << "DOH-KDVP SUMMARY\n"
            << "  report_without_exclusions: " << (withoutExclusions.mReport ? "present" : "absent")
            << "\n  lists: " << lists.size()
            << "\n  blocked_isins: " << withoutExclusions.mBlocked.size()
            << "\n  long_held_sales: " << withoutExclusions.mLongHeld.size() << '\n';

    for (const auto reason : {KdvpBlockReason::FifoStopped, KdvpBlockReason::LossNotReportable})
    {
        aOutput << "  blocked_" << toString(reason) << ": "
                << std::count_if(withoutExclusions.mBlocked.begin(),
                                 withoutExclusions.mBlocked.end(),
                                 [reason](const KdvpBlockedIsin& aBlocked) {
                                     return aBlocked.mReason == reason;
                                 })
                << '\n';
    }

    aOutput << "\nWhen an ISIN is blocked there is no report. The lists below leave out every "
               "blocked ISIN\nas a preview of this tool; the user has not agreed to that.\n\n";

    aOutput << "DOH-KDVP LISTS: " << lists.size() << "\n\n";

    for (const auto& list : lists)
    {
        writeKdvpList(aOutput, aStatement.mPresentation, list);
    }

    aOutput << "DOH-KDVP BLOCKED: " << withoutExclusions.mBlocked.size() << "\n\n";

    for (const auto& blocked : withoutExclusions.mBlocked)
    {
        writeKdvpBlockedIsin(aOutput, aStatement.mPresentation, blocked);
    }

    aOutput << "DOH-KDVP LONG-HELD (tax free, not listed): " << withoutExclusions.mLongHeld.size()
            << "\n\n";

    for (const auto& longHeld : withoutExclusions.mLongHeld)
    {
        aOutput << "- name: " << longHeld.mName << "\n  isin: " << longHeld.mIsin << "\n  sale: ";
        writeCapitalEvent(aOutput, aStatement.mPresentation, longHeld.mSale);
        aOutput << "\n  units_left_out: ";
        writeFixedPoint(aOutput, longHeld.mUnits, UNITS_SCALE);
        aOutput << "\n\n";
    }
}

void writeProcessingDiagnostics(std::ostream& aOutput,
                                const std::vector<ProcessingDiagnostic>& aDiagnostics) {
    aOutput << "PROCESSING DIAGNOSTICS: " << aDiagnostics.size() << "\n\n";

    for (const auto& diagnostic : aDiagnostics)
    {
        aOutput << "- severity: " << toString(diagnostic.mSeverity)
                << "\n  code: " << toString(diagnostic.mCode)
                << "\n  isin: " << diagnostic.mContext.mIsin.value_or("<none>")
                << "\n  message: " << diagnostic.mMessage;
        writeMessageKey(aOutput, diagnostic.mMessageKey, diagnostic.mMessageParams);
        aOutput << "\n\n";
    }
}

/// The reporting year argument, or nothing after an error message when it is not a whole number.
std::optional<int> parseYearArgument(std::string_view aYearText) {
    int year{};
    const auto parsedYear =
        std::from_chars(aYearText.data(), aYearText.data() + aYearText.size(), year);

    if (parsedYear.ec != std::errc{} || parsedYear.ptr != aYearText.data() + aYearText.size())
    {
        std::cerr << "The reporting year must be a whole number.\n";
        return std::nullopt;
    }

    return year;
}

/// Parses, merges and prepares the selected year, then writes the FIFO pools and the Doh-KDVP
/// filing rows for inspection.
int writeFifoResult(std::span<char*> aArguments) {
    const std::filesystem::path outputPath{aArguments[1]};
    const auto parsedYear = parseYearArgument(aArguments[0]);

    if (!parsedYear)
    {
        return 2;
    }

    const int year = *parsedYear;
    const auto prepared = TaxProcessor{}.prepareReportingYear(TaxProcessingRequest{
        .mReportingYear = year,
        .mMerged = DeterministicStatementMerger{}.merge(parseInputs(aArguments.subspan(2))),
    });

    logMergeDiagnostics(prepared.mMerged);

    if (!prepared.mLedger)
    {
        writeProcessingDiagnostics(std::cerr, prepared.mDiagnostics);
        return 1;
    }

    const auto& statement = prepared.mMerged.mStatement;
    const auto fifo = FifoMatcher{}.match(statement, *prepared.mLedger);

    std::size_t matchCount{};
    std::size_t lossMakingCount{};

    for (const auto& isin : fifo.mIsins)
    {
        matchCount += isin.mMatches.size();
        lossMakingCount += static_cast<std::size_t>(
            std::count_if(isin.mMatches.begin(), isin.mMatches.end(), [](const FifoMatch& aMatch) {
                return aMatch.mGainTicks.numerator() < 0;
            }));
    }

    createParentDirectory(outputPath);
    std::ofstream output{outputPath};

    if (!output)
    {
        std::cerr << "Failed to open output file: " << outputPath << '\n';
        return 1;
    }

    output << "FIFO SUMMARY\n"
           << "  reporting_year: " << year << "\n  input_files: " << aArguments.size() - 2
           << "\n  instruments: " << fifo.mIsins.size() << '\n';

    for (const auto status : {FifoIsinStatus::Matched,
                              FifoIsinStatus::CorporateAction,
                              FifoIsinStatus::NonEurTrade,
                              FifoIsinStatus::InsufficientUnits,
                              FifoIsinStatus::NumericFailure})
    {
        output << "  status_" << toString(status) << ": "
               << std::count_if(
                      fifo.mIsins.begin(),
                      fifo.mIsins.end(),
                      [status](const FifoIsinResult& aIsin) { return aIsin.mStatus == status; })
               << '\n';
    }

    output << "  in_year_matches: " << matchCount << "\n  loss_making_matches: " << lossMakingCount
           << "\n  upstream_diagnostics: " << prepared.mMerged.mDiagnostics.size()
           << "\n  processing_diagnostics: "
           << prepared.mDiagnostics.size() + fifo.mDiagnostics.size()
           << "\n\nAmounts are exact values shown rounded to eight decimals. Fees are not "
              "included.\n\n";

    output << "INSTRUMENTS: " << fifo.mIsins.size() << "\n\n";

    for (const auto& isin : fifo.mIsins)
    {
        writeFifoIsin(output, statement.mPresentation, isin);
    }

    // No answers: the open loss units are treated as blocked, like in the CLI.
    RunDecisions decisions;
    const auto loss = LossEligibilityChecker::check(statement,
                                                    *prepared.mLedger,
                                                    fifo,
                                                    decisions,
                                                    OpenLossQuestions::AssumeBlocked);

    writeLossEligibility(output, statement.mPresentation, loss);
    writeKdvpReport(output, statement, *prepared.mLedger, fifo, loss);

    auto diagnostics = prepared.mDiagnostics;
    diagnostics.insert(diagnostics.end(), fifo.mDiagnostics.begin(), fifo.mDiagnostics.end());
    diagnostics.insert(diagnostics.end(), loss.mDiagnostics.begin(), loss.mDiagnostics.end());
    writeProcessingDiagnostics(output, diagnostics);

    if (!output)
    {
        std::cerr << "Failed to write FIFO output file: " << outputPath << '\n';
        return 1;
    }

    std::cout << "Wrote FIFO matching for " << year << " to " << outputPath << '\n';

    return 0;
}

/// Parses, merges and prepares the selected year, matches FIFO and writes the Doh-KDVP XML of the
/// filing rows for a resident taxpayer. Like writeKdvpReport, it leaves out every blocked ISIN as
/// a preview for this tool only; with a blocked ISIN the file is not a complete return, and the
/// tool says so.
int writeKdvpXml(std::span<char*> aArguments) {
    const std::filesystem::path outputPath{aArguments[1]};
    const std::string_view filingTypeText{aArguments[3]};
    const auto parsedYear = parseYearArgument(aArguments[0]);

    if (!parsedYear)
    {
        return 2;
    }

    if (filingTypeText != "original" && filingTypeText != "self-report")
    {
        std::cerr << "The filing type must be original or self-report.\n";
        return 2;
    }

    const int year = *parsedYear;
    const auto prepared = TaxProcessor{}.prepareReportingYear(TaxProcessingRequest{
        .mReportingYear = year,
        .mMerged = DeterministicStatementMerger{}.merge(parseInputs(aArguments.subspan(4))),
    });

    logMergeDiagnostics(prepared.mMerged);

    if (!prepared.mLedger)
    {
        writeProcessingDiagnostics(std::cerr, prepared.mDiagnostics);
        return 1;
    }

    const auto& statement = prepared.mMerged.mStatement;
    const auto fifo = FifoMatcher{}.match(statement, *prepared.mLedger);
    // No answers: the open loss units are treated as blocked, like in the CLI.
    RunDecisions decisions;
    const auto loss = LossEligibilityChecker::check(statement,
                                                    *prepared.mLedger,
                                                    fifo,
                                                    decisions,
                                                    OpenLossQuestions::AssumeBlocked);
    const auto withoutExclusions =
        CapitalGainsReportBuilder::build(statement, *prepared.mLedger, fifo, loss, {});
    std::vector<Isin> blockedIsins;

    std::transform(withoutExclusions.mBlocked.begin(),
                   withoutExclusions.mBlocked.end(),
                   std::back_inserter(blockedIsins),
                   [](const KdvpBlockedIsin& aBlocked) { return aBlocked.mIsin; });

    const auto preview =
        CapitalGainsReportBuilder::build(statement, *prepared.mLedger, fifo, loss, blockedIsins);
    const ReportMetadata metadata{
        .mTaxpayer = {.mTaxNumber = aArguments[2], .mIsResident = true},
        .mReportingYear = year,
        .mFilingType = filingTypeText == "original" ? FilingType::Original : FilingType::SelfReport,
    };
    const auto written = KdvpXmlWriter::write(metadata, preview.mReport.value());

    if (!written.mXml)
    {
        writeProcessingDiagnostics(std::cerr, written.mDiagnostics);
        return 1;
    }

    createParentDirectory(outputPath);
    std::ofstream output{outputPath, std::ios::binary};
    output << *written.mXml;

    if (!output)
    {
        std::cerr << "Failed to write Doh-KDVP XML file: " << outputPath << '\n';
        return 1;
    }

    std::cout << "Wrote Doh-KDVP XML for " << year << " to " << outputPath
              << "\n  lists: " << preview.mReport->mLists.size()
              << "\n  blocked ISINs left out: " << withoutExclusions.mBlocked.size()
              << "\n  sales with units past the holding period (left out of the rows): "
              << withoutExclusions.mLongHeld.size() << '\n';

    if (!withoutExclusions.mBlocked.empty())
    {
        std::cout << "WARNING: blocked ISINs are left out, so this file is not a complete "
                     "return. dump_tr_fifo.sh shows them with their reasons.\n";
    }

    return 0;
}

} // namespace

int main(int aArgumentCount, char** aArguments) {
    if (aArgumentCount >= 7 && std::string_view{aArguments[1]} == "--kdvp")
    {
        try
        {
            return writeKdvpXml(
                std::span{aArguments + 2, static_cast<std::size_t>(aArgumentCount - 2)});
        } catch (const std::exception& exception)
        {
            std::cerr << "Failed to write Doh-KDVP XML: " << exception.what() << '\n';
            return 1;
        }
    }

    if (aArgumentCount >= 5 && std::string_view{aArguments[1]} == "--fifo")
    {
        try
        {
            return writeFifoResult(
                std::span{aArguments + 2, static_cast<std::size_t>(aArgumentCount - 2)});
        } catch (const std::exception& exception)
        {
            std::cerr << "Failed to match FIFO lots: " << exception.what() << '\n';
            return 1;
        }
    }

    if (aArgumentCount >= 4 && (std::string_view{aArguments[1]} == "--merge" ||
                                std::string_view{aArguments[1]} == "--diagnostics"))
    {
        try
        {
            return writeMergedResult(
                std::span{aArguments + 2, static_cast<std::size_t>(aArgumentCount - 2)},
                std::string_view{aArguments[1]} == "--diagnostics");
        } catch (const std::exception& exception)
        {
            std::cerr << "Failed to merge Trade Republic CSVs: " << exception.what() << '\n';
            return 1;
        }
    }

    if (aArgumentCount != 4)
    {
        std::cerr << "Usage: taxbroker_tr_dump <Trade Republic CSV> <parsed output> "
                     "<diagnostics JSON>\n"
                     "       taxbroker_tr_dump --merge <merged output> <CSV> <CSV> [...]\n"
                     "       taxbroker_tr_dump --diagnostics <diagnostics text> <CSV> [...]\n"
                     "       taxbroker_tr_dump --fifo <year> <FIFO output> <CSV> [...]\n"
                     "       taxbroker_tr_dump --kdvp <year> <XML output> <tax number> "
                     "<original|self-report> <CSV> [...]\n";
        return 2;
    }

    const std::filesystem::path csvPath{aArguments[1]};
    const std::filesystem::path outputPath{aArguments[2]};
    const std::filesystem::path diagnosticsPath{aArguments[3]};

    try
    {
        taxbroker::tr::TradeRepublicParser parser;
        const taxbroker::ParseResult parseResult = parser.parse(csvPath);

        createParentDirectory(outputPath);
        std::ofstream output{outputPath};
        if (!output)
        {
            std::cerr << "Failed to open output file: " << outputPath << '\n';
            return 1;
        }

        writeParseResult(output, parseResult);
        if (!output)
        {
            std::cerr << "Failed to write parsed output file: " << outputPath << '\n';
            return 1;
        }

        createParentDirectory(diagnosticsPath);
        std::ofstream diagnosticsOutput{diagnosticsPath};
        if (!diagnosticsOutput)
        {
            std::cerr << "Failed to open diagnostics file: " << diagnosticsPath << '\n';
            return 1;
        }
        diagnosticsOutput << taxbroker::api::serializeDiagnosticsJson(parseResult, 2) << '\n';
        if (!diagnosticsOutput)
        {
            std::cerr << "Failed to write diagnostics file: " << diagnosticsPath << '\n';
            return 1;
        }

        std::cout << "Wrote parsed Trade Republic data to " << outputPath << '\n';
        std::cout << "Wrote parser diagnostics to " << diagnosticsPath << '\n';
    } catch (const std::exception& exception)
    {
        std::cerr << "Failed to parse Trade Republic CSV: " << exception.what() << '\n';
        return 1;
    }

    return 0;
}
