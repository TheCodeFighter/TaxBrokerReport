#include "parsers/ibkr_parser.hpp"
#include "taxbroker/diagnostic_messages.hpp"
#include "taxbroker/types.hpp"

#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <utility>

namespace {

using namespace taxbroker;

/// Counts of the events and rows a file gave, by kind. No value of any row is written.
struct EventCounts {
    std::size_t mTrades{};
    std::size_t mSplits{};
    std::size_t mUnsupportedActions{};
    std::size_t mBondRedemptions{};
    std::size_t mPaymentRows{};
    std::size_t mBrokerInterest{};
    std::size_t mBondInterest{};
    std::size_t mLeftOutRows{};
};

EventCounts countEvents(const BrokerStatement& aStatement) {
    EventCounts counts;

    for (const auto& instrument : aStatement.mTradeInstruments)
    {
        counts.mTrades += instrument.mTransactions.size();

        for (const auto& action : instrument.mCorporateActions)
        {
            if (action.mType == CorporateActionType::UnresolvedSplit)
            {
                ++counts.mSplits;
            }
            else
            {
                ++counts.mUnsupportedActions;
            }
        }
    }

    for (const auto& instrument : aStatement.mInterestInstruments)
    {
        const bool isBond = instrument.mInterestType == InterestType::BondInterest;

        (isBond ? counts.mBondInterest : counts.mBrokerInterest) += instrument.mTransactions.size();
    }

    counts.mBondRedemptions = aStatement.mBondRedemptions.size();
    counts.mPaymentRows = aStatement.mIncomePaymentRows.size();
    counts.mLeftOutRows = aStatement.mLeftOutRows.size();

    return counts;
}

/// Writes the event counts and the number of diagnostics per severity and message key.
void writeFile(std::ostream& aOutput, const std::string& aName, const ParseResult& aResult) {
    const auto counts = countEvents(aResult.mStatement);
    std::map<std::string, std::size_t> diagnostics;

    for (const auto& diagnostic : aResult.mDiagnostics)
    {
        std::string severity = "warning";

        if (diagnostic.mSeverity == DiagnosticSeverity::Error)
        {
            severity = "error";
        }
        else if (diagnostic.mSeverity == DiagnosticSeverity::Info)
        {
            severity = "info";
        }

        const std::string key = diagnostic.mMessageKey
                                    ? std::string{messageDefinition(*diagnostic.mMessageKey).mId}
                                    : "no_message_key";

        ++diagnostics[severity + " " + key];
    }

    aOutput << "file " << aName << '\n'
            << "  trades " << counts.mTrades << '\n'
            << "  splits " << counts.mSplits << '\n'
            << "  unsupported_actions " << counts.mUnsupportedActions << '\n'
            << "  bond_redemptions " << counts.mBondRedemptions << '\n'
            << "  payment_rows " << counts.mPaymentRows << '\n'
            << "  broker_interest " << counts.mBrokerInterest << '\n'
            << "  bond_interest " << counts.mBondInterest << '\n'
            << "  left_out_rows " << counts.mLeftOutRows << '\n';

    for (const auto& [name, count] : diagnostics)
    {
        aOutput << "  diagnostic " << name << " " << count << '\n';
    }
}

} // namespace

/// Usage: taxbroker_ibkr_dump <output-file> <flex-query-xml>...
/// Parses each file with the Interactive Brokers parser and writes only counts, never a value.
int main(int aArgc, char** aArgv) {
    if (aArgc < 3)
    {
        std::cerr << "Usage: taxbroker_ibkr_dump <output-file> <flex-query-xml>...\n";
        return 1;
    }

    try
    {
        std::ofstream output{aArgv[1]};

        if (!output)
        {
            std::cerr << "The output file could not be opened.\n";
            return 1;
        }

        ibkr::IbkrParser parser;

        for (int index = 2; index < aArgc; ++index)
        {
            const std::filesystem::path path{aArgv[index]};
            const auto result = parser.parseFile(path, static_cast<std::size_t>(index - 2));

            writeFile(output, path.filename().string(), result);
        }
    } catch (const std::exception& aError)
    {
        std::cerr << "The dump failed: " << aError.what() << '\n';
        return 1;
    }

    return 0;
}
