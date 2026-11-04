#include "taxbroker/official_rates.hpp"
#include "utils/date_utils.hpp"
#include "utils/numeric_util.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace taxbroker;

/// Writes one block per loaded file (name, year, counts, first and last date) and then
/// every diagnostic. Only file names are written, never paths.
void writeSummary(std::ostream& aOutput, const OfficialRateLoadResult& aResult) {
    for (const auto& file : aResult.mTable.mFiles)
    {
        std::set<std::string> currencies;
        std::optional<Date> firstDate;
        std::optional<Date> lastDate;

        for (const auto& [key, rate] : aResult.mTable.mRates)
        {
            if (rate.mSourceFile != file.mSourceFile)
            {
                continue;
            }

            currencies.insert(rate.mCurrencyCode);
            firstDate =
                firstDate ? std::min(*firstDate, rate.mObservationDate) : rate.mObservationDate;
            lastDate =
                lastDate ? std::max(*lastDate, rate.mObservationDate) : rate.mObservationDate;
        }

        aOutput << "file " << file.mSourceFile.value() << '\n'
                << "  year " << file.mYear << '\n'
                << "  rates " << file.mRateCount << '\n'
                << "  currencies " << currencies.size() << '\n'
                << "  first date " << formatCalendarDate(*firstDate) << '\n'
                << "  last date " << formatCalendarDate(*lastDate) << '\n';
    }

    for (const auto& diagnostic : aResult.mDiagnostics)
    {
        aOutput << "error " << diagnostic.mSourceFile;

        if (diagnostic.mRowIndex)
        {
            aOutput << " row " << *diagnostic.mRowIndex;
        }

        if (diagnostic.mField)
        {
            aOutput << " field " << *diagnostic.mField;
        }

        aOutput << ": " << diagnostic.mMessage << '\n';
    }
}

/// Writes `rates_<year>.csv` for each loaded file into the folder: the header `currency,date,rate`
/// and one row per rate, sorted by currency and then date, with the rate as the file gave it
/// (trailing zeros removed). This is the form of the files built into the program.
void writeCompactFiles(const std::filesystem::path& aFolder, const OfficialRateTable& aTable) {
    for (const auto& file : aTable.mFiles)
    {
        std::ofstream output{aFolder / ("rates_" + std::to_string(file.mYear) + ".csv"),
                             std::ios::binary | std::ios::trunc};

        output << "currency,date,rate\n";

        for (const auto& [key, rate] : aTable.mRates)
        {
            if (rate.mSourceFile != file.mSourceFile)
            {
                continue;
            }

            output << rate.mCurrencyCode << ',' << formatCalendarDate(rate.mObservationDate) << ','
                   << formatFixedPoint(rate.mValue, EXCHANGE_RATE_SCALE, 0) << '\n';
        }
    }
}

} // namespace

int main(int aArgumentCount, char** aArguments) {
    if (aArgumentCount < 3)
    {
        std::cerr << "Usage: taxbroker_ecb_dump <output-file> <ecb-csv>...\n"
                  << "       taxbroker_ecb_dump --compact <output-folder> <ecb-csv>...\n";
        return 2;
    }

    const bool compact = std::string_view{aArguments[1]} == "--compact";

    if (compact && aArgumentCount < 4)
    {
        std::cerr << "Usage: taxbroker_ecb_dump --compact <output-folder> <ecb-csv>...\n";
        return 2;
    }

    try
    {
        std::vector<std::filesystem::path> inputs;

        for (int index = compact ? 3 : 2; index < aArgumentCount; ++index)
        {
            inputs.emplace_back(aArguments[index]);
        }

        const auto result = loadEcbRateFiles(inputs);

        if (compact)
        {
            // A rejected file writes nothing, so a half-converted set is never left behind.
            if (result.mDiagnostics.empty())
            {
                writeCompactFiles(aArguments[2], result.mTable);
                return 0;
            }

            writeSummary(std::cerr, result);
            return 1;
        }

        std::ofstream output{aArguments[1], std::ios::trunc};

        if (!output)
        {
            std::cerr << "Failed to open the summary file.\n";
            return 1;
        }

        writeSummary(output, result);

        return result.mDiagnostics.empty() ? 0 : 1;
    } catch (const std::exception& exception)
    {
        std::cerr << "Failed to read the rate files: " << exception.what() << '\n';
        return 1;
    }
}
