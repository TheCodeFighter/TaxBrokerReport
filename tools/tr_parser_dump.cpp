#include "parsers/traderepublic_parser.hpp"
#include "taxbroker/types.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <ostream>
#include <string_view>

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

void writeDate(std::ostream& aOutput, Date aDate) {
    const auto dayPoint = std::chrono::floor<std::chrono::days>(aDate);
    const std::chrono::year_month_day calendarDate{dayPoint};

    aOutput << static_cast<int>(calendarDate.year()) << '-' << std::setfill('0') << std::setw(2)
            << static_cast<unsigned>(calendarDate.month()) << '-' << std::setw(2)
            << static_cast<unsigned>(calendarDate.day()) << std::setfill(' ');
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
    case BenefitType::Stockperk:
        return "Stockperk";
    }

    return "Unknown";
}

std::string_view toString(WarningCode aWarningCode) {
    switch (aWarningCode)
    {
    case WarningCode::UnknownRowType:
        return "UnknownRowType";
    case WarningCode::UnsupportedRowType:
        return "UnsupportedRowType";
    case WarningCode::UnsupportedAssetClass:
        return "UnsupportedAssetClass";
    case WarningCode::MissingField:
        return "MissingField";
    case WarningCode::InvalidValue:
        return "InvalidValue";
    case WarningCode::ParseError:
        return "ParseError";
    }

    return "Unknown";
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
            aOutput << "    - date: ";
            writeDate(aOutput, transaction.mDate);
            aOutput << "\n      side: " << toString(transaction.mTradeSide)
                    << "\n      unit_price: ";
            writeFixedPoint(aOutput, transaction.mUnitPrice, MONEY_SCALE);
            aOutput << "\n      units: ";
            writeFixedPoint(aOutput, transaction.mUnits, UNITS_SCALE);
            aOutput << "\n      exchange_rate: ";
            writeFixedPoint(aOutput, transaction.mExchangeRate, MONEY_SCALE);
            aOutput << "\n      currency: " << toString(transaction.mCurrency) << '\n';
        }

        aOutput << "  corporate_actions: " << instrument.mCorporateActions.size() << '\n';

        for (const auto& action : instrument.mCorporateActions)
        {
            aOutput << "    - date: ";
            writeDate(aOutput, action.mDate);
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
            aOutput << "    - date: ";
            writeDate(aOutput, transaction.mDate);
            aOutput << "\n      gross_amount: ";
            writeFixedPoint(aOutput, transaction.mGrossAmount, MONEY_SCALE);
            aOutput << "\n      tax_paid: ";
            writeFixedPoint(aOutput, transaction.mTaxPaid, MONEY_SCALE);
            aOutput << "\n      exchange_rate: ";
            writeFixedPoint(aOutput, transaction.mExchangeRate, MONEY_SCALE);
            aOutput << "\n      currency: " << toString(transaction.mCurrency) << '\n';
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
            aOutput << "    - date: ";
            writeDate(aOutput, transaction.mDate);
            aOutput << "\n      gross_amount: ";
            writeFixedPoint(aOutput, transaction.mGrossAmount, MONEY_SCALE);
            aOutput << "\n      tax_paid: ";
            writeFixedPoint(aOutput, transaction.mTaxPaid, MONEY_SCALE);
            aOutput << "\n      exchange_rate: ";
            writeFixedPoint(aOutput, transaction.mExchangeRate, MONEY_SCALE);
            aOutput << "\n      currency: " << toString(transaction.mCurrency) << '\n';
        }

        aOutput << '\n';
    }
}

void writeBenefits(std::ostream& aOutput, const BrokerStatement& aStatement) {
    aOutput << "BENEFIT EVENTS: " << aStatement.mBenefitEvents.size() << "\n\n";

    for (const auto& benefit : aStatement.mBenefitEvents)
    {
        aOutput << "- date: ";
        writeDate(aOutput, benefit.mDate);
        aOutput << "\n  datetime: " << benefit.mDateTime << "\n  type: " << toString(benefit.mType)
                << "\n  name: " << (benefit.mName.empty() ? "<none>" : benefit.mName)
                << "\n  isin: " << benefit.mIsin.value_or("<none>")
                << "\n  asset_class: " << toString(benefit.mAssetClass) << "\n  amount: ";
        writeFixedPoint(aOutput, benefit.mAmount, MONEY_SCALE);
        aOutput << "\n  currency: " << toString(benefit.mCurrency)
                << "\n  transaction_id: " << benefit.mTransactionId << "\n\n";
    }
}

void writeWarnings(std::ostream& aOutput, const ParseResult& aParseResult) {
    aOutput << "WARNINGS: " << aParseResult.mWarnings.size() << "\n\n";

    for (const auto& warning : aParseResult.mWarnings)
    {
        aOutput << "- code: " << toString(warning.mCode) << "\n  source: " << warning.mSourceFile
                << "\n  row: " << warning.mRowIndex << "\n  message: " << warning.mMessage
                << "\n\n";
    }
}

void writeParseResult(std::ostream& aOutput, const ParseResult& aParseResult) {
    writeTrades(aOutput, aParseResult.mStatement);
    writeDividends(aOutput, aParseResult.mStatement);
    writeInterests(aOutput, aParseResult.mStatement);
    writeBenefits(aOutput, aParseResult.mStatement);
    writeWarnings(aOutput, aParseResult);
}

} // namespace

int main(int aArgumentCount, char** aArguments) {
    if (aArgumentCount != 3)
    {
        std::cerr << "Usage: taxbroker_tr_dump <Trade Republic CSV> <output file>\n";
        return 2;
    }

    const std::filesystem::path csvPath{aArguments[1]};
    const std::filesystem::path outputPath{aArguments[2]};

    try
    {
        taxbroker::tr::TradeRepublicParser parser;
        const taxbroker::ParseResult parseResult = parser.parse(csvPath);

        std::ofstream output{outputPath};
        if (!output)
        {
            std::cerr << "Failed to open output file: " << outputPath << '\n';
            return 1;
        }

        writeParseResult(output, parseResult);
        std::cout << "Wrote parsed Trade Republic data to " << outputPath << '\n';
    } catch (const std::exception& exception)
    {
        std::cerr << "Failed to parse Trade Republic CSV: " << exception.what() << '\n';
        return 1;
    }

    return 0;
}
