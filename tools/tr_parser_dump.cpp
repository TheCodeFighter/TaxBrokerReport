#include "parsers/traderepublic_parser.hpp"
#include "taxbroker/api/diagnostics_json.hpp"
#include "taxbroker/statement_merger.hpp"
#include "taxbroker/types.hpp"
#include "utils/logger.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
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
            writeFixedPoint(aOutput, transaction.mUnitPrice, MONEY_SCALE);
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
            writeFixedPoint(aOutput, transaction.mExchangeRate, EXCHANGE_RATE_SCALE);
            aOutput << "\n      currency: " << toString(transaction.mCurrency) << '\n';
        }

        aOutput << "  corporate_actions: " << instrument.mCorporateActions.size() << '\n';

        for (const auto& action : instrument.mCorporateActions)
        {
            aOutput << "    - ";
            writeMetadata(aOutput, action.mMetadata, "      ");
            aOutput << "\n      type: " << toString(action.mType) << "\n      units_delta: ";
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
            writeFixedPoint(aOutput, transaction.mTaxPaid, MONEY_SCALE);
            aOutput << "\n      exchange_rate: ";
            writeFixedPoint(aOutput, transaction.mExchangeRate, EXCHANGE_RATE_SCALE);
            aOutput << "\n      currency: " << toString(transaction.mCurrency)
                    << "\n      tax_currency: " << toString(transaction.mTaxCurrency) << '\n';
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
            writeFixedPoint(aOutput, transaction.mTaxPaid, MONEY_SCALE);
            aOutput << "\n      exchange_rate: ";
            writeFixedPoint(aOutput, transaction.mExchangeRate, EXCHANGE_RATE_SCALE);
            aOutput << "\n      currency: " << toString(transaction.mCurrency)
                    << "\n      tax_currency: " << toString(transaction.mTaxCurrency) << '\n';
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
                << "\n  message: " << diagnostic.mMessage << "\n\n";
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

    for (const auto& diagnostic : aMergeResult.mDiagnostics)
    {
        if (const auto* parseDiagnostic = std::get_if<SourcedParseDiagnostic>(&diagnostic))
        {
            aOutput << "- stage: Parsing\n"
                    << "  source_index: " << parseDiagnostic->mSourceIndex
                    << "\n  severity: " << toString(parseDiagnostic->mDiagnostic.mSeverity)
                    << "\n  code: " << toString(parseDiagnostic->mDiagnostic.mCode)
                    << "\n  source: " << safeSourceFile(parseDiagnostic->mDiagnostic.mSourceFile)
                    << "\n  message: " << parseDiagnostic->mDiagnostic.mMessage << "\n\n";
            continue;
        }

        const auto& mergeDiagnostic = std::get<MergeDiagnostic>(diagnostic);

        aOutput << "- stage: Merging\n"
                << "  severity: " << toString(mergeDiagnostic.mSeverity)
                << "\n  code: " << toString(mergeDiagnostic.mCode)
                << "\n  message: " << mergeDiagnostic.mMessage
                << "\n  sources: " << mergeDiagnostic.mSources.size() << '\n';

        for (const auto& source : mergeDiagnostic.mSources)
        {
            aOutput << "    - source_index: " << source.mInputSequence.mSourceIndex
                    << "\n      event_index: " << source.mInputSequence.mEventIndex
                    << "\n      source_file: " << source.mFilename.value()
                    << "\n      source_row: " << source.mSourceRow << '\n';
        }

        aOutput << '\n';
    }
}

int writeMergedResult(std::span<char*> aArguments) {
    const std::filesystem::path outputPath{aArguments[0]};

    std::vector<StatementMergeInput> inputs;
    inputs.reserve(aArguments.size() - 1);

    taxbroker::tr::TradeRepublicParser parser;

    for (std::size_t index = 1; index < aArguments.size(); ++index)
    {
        inputs.push_back(StatementMergeInput{
            .mSourceIndex = index - 1,
            .mParseResult = parser.parse(aArguments[index], index - 1),
        });
    }

    const auto mergeResult = DeterministicStatementMerger{}.merge(inputs);

    logMergeDiagnostics(mergeResult);

    const auto parsedEventCount =
        std::accumulate(inputs.begin(),
                        inputs.end(),
                        std::size_t{},
                        [](std::size_t aCount, const StatementMergeInput& aInput) {
                            return aCount + eventCount(aInput.mParseResult.mStatement);
                        });
    const auto mergedEventCount = eventCount(mergeResult.mStatement.mPresentation);

    createParentDirectory(outputPath);
    std::ofstream output{outputPath};

    if (!output)
    {
        std::cerr << "Failed to open output file: " << outputPath << '\n';
        return 1;
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

} // namespace

int main(int aArgumentCount, char** aArguments) {
    if (aArgumentCount >= 4 && std::string_view{aArguments[1]} == "--merge")
    {
        try
        {
            return writeMergedResult(
                std::span{aArguments + 2, static_cast<std::size_t>(aArgumentCount - 2)});
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
                     "       taxbroker_tr_dump --merge <merged output> <CSV> <CSV> [...]\n";
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
