#include "parsers/traderepublic_parser.hpp"
#include "taxbroker/fifo_basis.hpp"
#include "taxbroker/statement_merger.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <numeric>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace {
using namespace taxbroker;

struct SplitFactor {
    std::int64_t mNewShares;
    std::int64_t mOldShares;
};

struct OpenLot {
    Date mPurchaseDate;
    LotBasisState mState;
};

template <typename T> T requireValue(const NumericResult<T>& aResult) {
    if (const auto* value = std::get_if<T>(&aResult))
    {
        return *value;
    }

    throw std::runtime_error{"Value is invalid or exceeds the supported arithmetic range"};
}

std::int64_t positiveInteger(std::string_view aText) {
    std::int64_t value{};
    const auto result = std::from_chars(aText.data(), aText.data() + aText.size(), value);

    if (result.ec != std::errc{} || result.ptr != aText.data() + aText.size() || value <= 0)
    {
        throw std::runtime_error{"Split row and share counts must be positive whole numbers"};
    }

    return value;
}

void writeDate(std::ostream& aOutput, Date aDate) {
    const std::chrono::year_month_day date{std::chrono::floor<std::chrono::days>(aDate)};

    aOutput << static_cast<int>(date.year()) << '-' << std::setfill('0') << std::setw(2)
            << static_cast<unsigned>(date.month()) << '-' << std::setw(2)
            << static_cast<unsigned>(date.day()) << std::setfill(' ');
}

void writeScaled(std::ostream& aOutput,
                 std::string_view aLabel,
                 std::int64_t aValue,
                 std::int64_t aScale) {
    // Widen before negating to handle INT64_MIN. Scales here are powers of ten.
    const WideInteger magnitude = aValue < 0 ? -WideInteger{aValue} : WideInteger{aValue};
    const auto width = std::to_string(aScale).size() - 1;
    const auto digits = (magnitude % aScale).convert_to<std::string>();

    aOutput << aLabel << ": " << (aValue < 0 ? "-" : "") << magnitude / aScale << '.'
            << std::string(width - digits.size(), '0') << digits << '\n';
}

void writeExact(std::ostream& aOutput, std::string_view aLabel, const ExactRational& aValue) {
    aOutput << aLabel << " (exact): " << aValue.numerator() << '/' << aValue.denominator() << '\n';
    writeScaled(aOutput,
                std::string{aLabel} + " (8-decimal display)",
                requireValue(roundToScaled(aValue, UNITS_SCALE)),
                UNITS_SCALE);
}

Units totalUnits(const std::vector<OpenLot>& aLots) {
    return std::accumulate(aLots.begin(),
                           aLots.end(),
                           Units{},
                           [](Units aTotal, const OpenLot& aLot) {
                               return requireValue(checkedAdd(aTotal, aLot.mState.mRemainingUnits));
                           });
}

void writeLots(std::ostream& aOutput, const std::vector<OpenLot>& aLots) {
    for (std::size_t index = 0; index < aLots.size(); ++index)
    {
        const auto& lot = aLots[index];

        aOutput << "  Open purchase lot " << index + 1 << " from ";
        writeDate(aOutput, lot.mPurchaseDate);
        aOutput << '\n';
        writeScaled(aOutput, "    Units", lot.mState.mRemainingUnits, UNITS_SCALE);
        writeExact(aOutput,
                   "    Purchase cost EUR",
                   requireValue(basisInEur(lot.mState.mRemainingBasis)));
        writeExact(aOutput,
                   "    Purchase cost per unit EUR",
                   requireValue(exactLotUnitValue(lot.mState)));
    }
}

// Each comparison starts from a copy. Guessed factors never alter the inspected position.
void writeSplitPreview(std::ostream& aOutput,
                       std::string_view aLabel,
                       const std::vector<OpenLot>& aLots,
                       SplitFactor aFactor) {
    aOutput << aLabel << ": " << aFactor.mNewShares << " new shares / " << aFactor.mOldShares
            << " old shares\n";
    if (aFactor.mNewShares == aFactor.mOldShares)
    {
        aOutput << "  This factor leaves units and purchase cost per unit unchanged.\n";
        writeScaled(aOutput, "  Preview units after split", totalUnits(aLots), UNITS_SCALE);
        writeLots(aOutput, aLots);
        return;
    }

    std::vector<UnitAllocationPart> quantities;

    for (std::size_t index = 0; index < aLots.size(); ++index)
    {
        quantities.push_back({aLots[index].mState.mRemainingUnits, index});
    }

    const auto result = allocateAdjustedUnits(quantities, aFactor.mNewShares, aFactor.mOldShares);

    if (std::holds_alternative<NumericError>(result))
    {
        aOutput << "  Preview cannot represent these adjusted quantities.\n";
        return;
    }

    const auto& allocation = std::get<RoundedAllocation>(result);
    auto previewLots = aLots;

    for (std::size_t index = 0; index < previewLots.size(); ++index)
    {
        previewLots[index].mState =
            requireValue(adjustLotUnits(previewLots[index].mState, allocation.mParts[index]));
    }

    writeScaled(aOutput, "  Preview units after split", totalUnits(previewLots), UNITS_SCALE);
    writeLots(aOutput, previewLots);
}

void writeFactorComparisons(std::ostream& aOutput,
                            const std::vector<OpenLot>& aLots,
                            const CorporateAction& aAction) {
    const auto before = totalUnits(aLots);

    if (before <= 0)
    {
        aOutput << "Factor comparisons unavailable: no reliable open position.\n";
        return;
    }

    writeScaled(aOutput, "Units before split", before, UNITS_SCALE);
    aOutput << "Purchase lots before split:\n";
    writeLots(aOutput, aLots);
    aOutput << "The following are separate what-if previews; neither proves the real factor.\n";

    // TR's quantity field is unverified: show both plausible readings, without selecting one.
    const auto afterIfDelta = checkedAdd(before, aAction.mUnitsDelta);

    if (const auto* after = std::get_if<std::int64_t>(&afterIfDelta); after && *after > 0)
    {
        const auto factor = requireValue(ExactRational::create(*after, before));

        writeSplitPreview(aOutput,
                          "Data guess if exported quantity means ADDED units",
                          aLots,
                          {factor.numerator().convert_to<std::int64_t>(),
                           factor.denominator().convert_to<std::int64_t>()});
    }

    if (aAction.mUnitsDelta > 0)
    {
        const auto factor = requireValue(ExactRational::create(aAction.mUnitsDelta, before));

        writeSplitPreview(aOutput,
                          "Data guess if exported quantity means TOTAL units after split",
                          aLots,
                          {factor.numerator().convert_to<std::int64_t>(),
                           factor.denominator().convert_to<std::int64_t>()});
    }

    writeSplitPreview(aOutput,
                      "Imaginary comparison (new/old = 1/2; quantity halved)",
                      aLots,
                      {1, 2});
}

void writeInstrument(std::ostream& aOutput,
                     const TradeInstrument& aInstrument,
                     const std::map<std::size_t, SplitFactor>& aFactors,
                     bool aParseErrors) {
    aOutput << "\nInstrument: " << aInstrument.mName << "\nISIN: " << aInstrument.mIsin << '\n';
    using Event = std::variant<const TradeTransaction*, const CorporateAction*>;
    std::vector<Event> events;

    std::transform(aInstrument.mTransactions.begin(),
                   aInstrument.mTransactions.end(),
                   std::back_inserter(events),
                   [](const TradeTransaction& aTrade) -> Event { return &aTrade; });
    std::transform(aInstrument.mCorporateActions.begin(),
                   aInstrument.mCorporateActions.end(),
                   std::back_inserter(events),
                   [](const CorporateAction& aAction) -> Event { return &aAction; });

    const auto metadata = [](const Event& aEvent) -> const EventMetadata& {
        const auto readMetadata = [](const auto* aValue) -> const EventMetadata& {
            return aValue->mMetadata;
        };

        return std::visit(readMetadata, aEvent);
    };

    // A split precedes trades on the same tax date. Other ties use the shared stable ordering.
    std::stable_sort(events.begin(), events.end(), [&](const Event& aLeft, const Event& aRight) {
        if (metadata(aLeft).mTaxDate == metadata(aRight).mTaxDate &&
            aLeft.index() != aRight.index())
        {
            return aLeft.index() == 1;
        }

        return ChronologicalEventOrder{}(metadata(aLeft), metadata(aRight));
    });

    std::vector<OpenLot> lots;
    bool reliable = !aParseErrors && !aInstrument.mIsin.empty();

    for (const auto& event : events)
    {
        const auto& eventMetadata = metadata(event);
        const auto row = primarySource(eventMetadata).mSourceRow;

        if (const auto* actionPointer = std::get_if<const CorporateAction*>(&event))
        {
            const auto& action = **actionPointer;

            aOutput << "\nSplit date: ";
            writeDate(aOutput, eventMetadata.mTaxDate);
            aOutput << "\nCSV row: " << row << '\n';

            aOutput << "Event: exported corporate action\n";
            writeScaled(aOutput,
                        "Exported split quantity field (meaning not verified)",
                        action.mUnitsDelta,
                        UNITS_SCALE);
            if (reliable && !lots.empty())
            {
                writeFactorComparisons(aOutput, lots, action);
            }

            const auto factor = aFactors.find(row);

            if (factor == aFactors.end())
            {
                aOutput << "Real split factor: not verified from this CSV. Supply --split-row "
                        << row << " NEW_SHARES OLD_SHARES.\n"
                        << "Units and price after this action are unresolved.\n";
                reliable = false;
                continue;
            }

            aOutput << "Chosen split factor (supplied locally; verify against issuer source): "
                    << factor->second.mNewShares << " new shares / " << factor->second.mOldShares
                    << " old shares\n";

            if (!reliable || lots.empty())
            {
                aOutput << "Cannot preview this split: earlier history is incomplete, contains "
                           "unconverted foreign trades, or has an unresolved action.\n";
                reliable = false;
                continue;
            }

            writeScaled(aOutput, "Units before split", totalUnits(lots), UNITS_SCALE);
            aOutput << "Purchase lots before split:\n";
            writeLots(aOutput, lots);
            std::vector<UnitAllocationPart> quantities;

            for (std::size_t index = 0; index < lots.size(); ++index)
            {
                quantities.push_back({lots[index].mState.mRemainingUnits, index});
            }

            // The local factor applies to every open lot; the exported delta is never a ratio.
            const auto allocation = requireValue(allocateAdjustedUnits(quantities,
                                                                       factor->second.mNewShares,
                                                                       factor->second.mOldShares));

            for (std::size_t index = 0; index < lots.size(); ++index)
            {
                lots[index].mState =
                    requireValue(adjustLotUnits(lots[index].mState, allocation.mParts[index]));
            }

            writeScaled(aOutput, "Units after split", totalUnits(lots), UNITS_SCALE);
            aOutput << "Purchase lots after split (total purchase cost preserved):\n";
            writeLots(aOutput, lots);
            continue;
        }

        const auto& trade = *std::get<const TradeTransaction*>(event);
        const bool buy = trade.mTradeSide == TradeSide::Buy;

        if (trade.mCurrency != Currency::EUR)
        {
            reliable = false;
        }

        if (!reliable)
        {
            continue;
        }

        if (buy)
        {
            lots.push_back({eventMetadata.mTaxDate,
                            requireValue(createLotBasis(trade.mUnitPrice, trade.mUnits))});
        }
        else
        {
            if (trade.mUnits > totalUnits(lots))
            {
                reliable = false;
                continue;
            }

            Units remainingToSell = trade.mUnits;

            // Consume only the actual exported sale, oldest purchase first; never close a lot
            // artificially.
            for (auto& lot : lots)
            {
                if (remainingToSell == 0)
                {
                    break;
                }

                const auto matched = std::min(remainingToSell, lot.mState.mRemainingUnits);
                const auto consumed = requireValue(consumeLotBasis(lot.mState, matched));
                lot.mState = consumed.mRemaining;
                remainingToSell -= matched;
            }

            std::erase_if(lots,
                          [](const OpenLot& aLot) { return aLot.mState.mRemainingUnits == 0; });
        }
    }
}

} // namespace

int main(int aArgumentCount, char** aArguments) {
    if (aArgumentCount < 3 || (aArgumentCount - 3) % 4 != 0)
    {
        std::cerr << "Usage: taxbroker_exact_dump CSV OUTPUT [--split-row ROW NEW_SHARES "
                     "OLD_SHARES]...\n";
        return 2;
    }

    try
    {
        std::map<std::size_t, SplitFactor> factors;

        for (int index = 3; index < aArgumentCount; index += 4)
        {
            if (std::string_view{aArguments[index]} != "--split-row")
            {
                throw std::runtime_error{"Expected --split-row ROW NEW_SHARES OLD_SHARES"};
            }

            const auto row = static_cast<std::size_t>(positiveInteger(aArguments[index + 1]));
            const SplitFactor factor{positiveInteger(aArguments[index + 2]),
                                     positiveInteger(aArguments[index + 3])};

            if (!factors.emplace(row, factor).second)
            {
                throw std::runtime_error{"Split row supplied more than once"};
            }
        }

        const auto parsed = tr::TradeRepublicParser{}.parse(aArguments[1]);
        const bool parseErrors =
            std::any_of(parsed.mDiagnostics.begin(),
                        parsed.mDiagnostics.end(),
                        [](const ParseDiagnostic& aDiagnostic) {
                            return aDiagnostic.mSeverity == DiagnosticSeverity::Error;
                        });
        const std::vector inputs{StatementMergeInput{0, parsed}};
        const auto merged = DeterministicStatementMerger{}.merge(inputs);
        for (const auto& [row, factor] : factors)
        {
            const auto matchesRow = [row](const TradeInstrument& aInstrument) {
                return std::any_of(aInstrument.mCorporateActions.begin(),
                                   aInstrument.mCorporateActions.end(),
                                   [row](const CorporateAction& aAction) {
                                       return primarySource(aAction.mMetadata).mSourceRow == row;
                                   });
            };

            if (!std::any_of(merged.mStatement.mPresentation.mTradeInstruments.begin(),
                             merged.mStatement.mPresentation.mTradeInstruments.end(),
                             matchesRow))
            {
                throw std::runtime_error{"A supplied split row did not match a parsed corporate "
                                         "action; run without factors to inspect current rows"};
            }
        }

        const std::filesystem::path outputPath{aArguments[2]};

        if (outputPath.has_parent_path())
        {
            std::filesystem::create_directories(outputPath.parent_path());
        }
        std::ofstream output{outputPath};

        if (!output)
        {
            throw std::runtime_error{"Unable to open local output file"};
        }

        output
            << "Local Trade Republic arithmetic inspection\n"
            << "Source: " << SourceFilename::fromPath(aArguments[1]).value() << '\n'
            << "Only split rows and their before/after purchase lots are shown. Trades are used "
               "internally.\n"
            << "Split factors supplied on the command line are local preview choices.\n"
            << "This is an arithmetic preview, not a completed tax report or action validation.\n"
            << "Parser diagnostics: " << parsed.mDiagnostics.size() << '\n';

        for (const auto& diagnostic : parsed.mDiagnostics)
        {
            output << "  "
                   << (diagnostic.mSeverity == DiagnosticSeverity::Error ? "Error" : "Warning")
                   << " row " << diagnostic.mRowIndex.value_or(0) << ": " << diagnostic.mMessage
                   << '\n';
        }

        // Merge conflicts can remove events, so a conflicted run cannot support a reliable ledger
        // preview.
        const bool incomplete =
            parseErrors ||
            std::any_of(
                merged.mDiagnostics.begin(),
                merged.mDiagnostics.end(),
                [](const StatementMergeDiagnostic& aDiagnostic) {
                    const auto isError = [](const auto& aValue) {
                        if constexpr (std::is_same_v<std::decay_t<decltype(aValue)>,
                                                     SourcedParseDiagnostic>)
                        {
                            return aValue.mDiagnostic.mSeverity == DiagnosticSeverity::Error;
                        }
                        else if constexpr (std::is_same_v<std::decay_t<decltype(aValue)>,
                                                          MergeDiagnostic>)
                        {
                            return aValue.mSeverity == DiagnosticSeverity::Error;
                        }
                        else
                        {
                            static_assert(std::is_same_v<std::decay_t<decltype(aValue)>, void>);
                        }
                    };

                    return std::visit(isError, aDiagnostic);
                });

        if (incomplete)
        {
            output << "Parsing or merge errors: position calculations are disabled.\n";
        }

        for (const auto& instrument : merged.mStatement.mPresentation.mTradeInstruments)
        {
            if (!instrument.mCorporateActions.empty())
            {
                writeInstrument(output, instrument, factors, incomplete);
            }
        }

        if (!output)
        {
            throw std::runtime_error{"Unable to write local output file"};
        }

        std::cout << "Wrote local arithmetic inspection to " << outputPath << '\n';
        return 0;
    } catch (const std::exception& error)
    {
        std::cerr << "Unable to inspect local arithmetic: " << error.what() << '\n';
        return 1;
    }
}
