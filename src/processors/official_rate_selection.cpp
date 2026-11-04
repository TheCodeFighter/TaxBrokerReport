#include "processors/official_rate_selection.hpp"

#include "taxbroker/currency.hpp"
#include "utils/date_utils.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>

namespace taxbroker {
namespace {

/// Everything the needs say about one currency and tax date.
struct PairNeed {
    Currency mCurrency{Currency::Unknown};
    std::set<ReportType> mReports;
    std::set<SourceReference, StableSourceOrder> mSources;
};

int yearOf(Date aDate) {
    const std::chrono::year_month_day day{std::chrono::sys_days{aDate.time_since_epoch()}};

    return static_cast<int>(day.year());
}

const OfficialRateFile* fileOfYear(const OfficialRateTable& aTable, int aYear) {
    const auto found = std::ranges::find_if(aTable.mFiles, [aYear](const OfficialRateFile& aFile) {
        return aFile.mYear == aYear;
    });

    return found == aTable.mFiles.end() ? nullptr : &*found;
}

Date startOfYear(int aYear) {
    using namespace std::chrono;

    return Date{sys_days{year{aYear} / January / 1}.time_since_epoch()};
}

Date endOfYear(int aYear) {
    using namespace std::chrono;

    return Date{sys_days{year{aYear} / December / 31}.time_since_epoch()};
}

/// The last day a file vouches for. A file that ends within kMaxRateDaysBack days of 31 December
/// is a whole year: the ECB publishes nothing on the last days of some years.
Date coveredUntil(const OfficialRateFile& aFile) {
    const Date yearEnd = endOfYear(aFile.mYear);

    return aFile.mLastDate + DayDuration{kMaxRateDaysBack} >= yearEnd ? yearEnd : aFile.mLastDate;
}

ProcessingDiagnostic pairDiagnostic(DiagnosticSeverity aSeverity,
                                    ProcessingDiagnosticCode aCode,
                                    DiagnosticMessage aMessage,
                                    const PairNeed& aPair,
                                    Date aTaxDate) {
    return ProcessingDiagnostic{
        .mSeverity = aSeverity,
        .mCode = aCode,
        .mContext =
            {
                .mStage = PipelineStage::FifoTax,
                .mAffectedReports = {aPair.mReports.begin(), aPair.mReports.end()},
                .mSources = {aPair.mSources.begin(), aPair.mSources.end()},
                .mTaxDate = aTaxDate,
            },
        .mMessage = std::move(aMessage.mEnglish),
        .mMessageKey = aMessage.mKey,
        .mMessageParams = std::move(aMessage.mParams),
    };
}

ProcessingDiagnostic
missingRateError(DiagnosticMessage aMessage, const PairNeed& aPair, Date aTaxDate) {
    return pairDiagnostic(DiagnosticSeverity::Error,
                          ProcessingDiagnosticCode::MissingExchangeRate,
                          std::move(aMessage),
                          aPair,
                          aTaxDate);
}

DiagnosticMessage noRateFileMessage(std::string_view aCode, Date aTaxDate, int aYear) {
    return makeDiagnosticMessage(MessageKey::MissingExchangeRateNoRateFile,
                                 {textParam("currency", std::string{aCode}),
                                  textParam("date", formatCalendarDate(aTaxDate)),
                                  numberParam("year", static_cast<std::uint64_t>(aYear))});
}

/// Why the loaded files cannot tell whether a day had a rate, and which file would fix it.
struct UncoveredDay {
    DiagnosticMessage mMessage;
    RateFileNeed mFileNeed;
};

/// The reason the loaded files cannot tell whether this day had a rate, or nothing when they can.
/// The file needed is that of the year of aDay, which can be the year before the tax date.
std::optional<UncoveredDay>
uncoveredDay(const OfficialRateTable& aTable, std::string_view aCode, Date aTaxDate, Date aDay) {
    const auto* file = fileOfYear(aTable, yearOf(aDay));

    if (file == nullptr)
    {
        return UncoveredDay{noRateFileMessage(aCode, aTaxDate, yearOf(aDay)),
                            {.mYear = yearOf(aDay), .mProblem = RateFileProblem::NotLoaded}};
    }

    if (aDay > coveredUntil(*file))
    {
        return UncoveredDay{
            makeDiagnosticMessage(MessageKey::MissingExchangeRateRateFileEndsEarly,
                                  {textParam("currency", std::string{aCode}),
                                   textParam("date", formatCalendarDate(aTaxDate)),
                                   textParam("lastDate", formatCalendarDate(file->mLastDate))}),
            {.mYear = yearOf(aDay), .mProblem = RateFileProblem::EndsEarly}};
    }

    return std::nullopt;
}

/// The needs of one rate file's year: all of them, and those after the end of the file.
struct FileUse {
    Date mFirstTaxDate{};
    PairNeed mNeeds;
    Date mFirstTaxDateAfterEnd{};
    PairNeed mNeedsAfterEnd;
};

void addPairNeed(PairNeed& aTarget, const PairNeed& aPair) {
    aTarget.mReports.insert(aPair.mReports.begin(), aPair.mReports.end());
    aTarget.mSources.insert(aPair.mSources.begin(), aPair.mSources.end());
}

/// The warnings for the rate file of one year that the needs use: it starts after the first days
/// of the year, or ends before the year but still vouches for a need after its end.
void addFileWarnings(const OfficialRateFile& aFile,
                     const FileUse& aUse,
                     OfficialRateSelection& aSelection) {
    const auto year = static_cast<std::uint64_t>(aFile.mYear);

    if (aFile.mFirstDate > startOfYear(aFile.mYear) + DayDuration{kMaxRateDaysBack})
    {
        aSelection.mDiagnostics.push_back(pairDiagnostic(
            DiagnosticSeverity::Warning,
            ProcessingDiagnosticCode::OfficialRateFileIncomplete,
            makeDiagnosticMessage(MessageKey::OfficialRateFileIncompleteStartsLate,
                                  {numberParam("year", year),
                                   textParam("firstDate", formatCalendarDate(aFile.mFirstDate))}),
            aUse.mNeeds,
            aUse.mFirstTaxDate));
    }

    if (!aUse.mNeedsAfterEnd.mReports.empty() && coveredUntil(aFile) == endOfYear(aFile.mYear))
    {
        aSelection.mDiagnostics.push_back(pairDiagnostic(
            DiagnosticSeverity::Warning,
            ProcessingDiagnosticCode::OfficialRateFileIncomplete,
            makeDiagnosticMessage(MessageKey::OfficialRateFileIncompleteEndsEarly,
                                  {numberParam("year", year),
                                   textParam("lastDate", formatCalendarDate(aFile.mLastDate))}),
            aUse.mNeedsAfterEnd,
            aUse.mFirstTaxDateAfterEnd));
    }
}

/// Looks for the pair; returns the rate, or adds the error that says why there is none. When a
/// rate file would fix it, the file is recorded in mMissingFiles.
const OfficialRate* resolvePair(const OfficialRateTable& aTable,
                                const std::set<Date>& aPublicationDays,
                                const PairNeed& aPair,
                                Date aTaxDate,
                                OfficialRateSelection& aSelection) {
    const std::string code{currencyCode(aPair.mCurrency)};
    const auto currencyParam = textParam("currency", code);
    const auto dateParam = textParam("date", formatCalendarDate(aTaxDate));
    const auto uncoveredError = [&](UncoveredDay aUncovered) {
        aSelection.mDiagnostics.push_back(
            missingRateError(std::move(aUncovered.mMessage), aPair, aTaxDate));
        aSelection.mMissingFiles.emplace(std::make_pair(code, aTaxDate), aUncovered.mFileNeed);
    };
    const auto noRateOnDate = [&]() {
        aSelection.mDiagnostics.push_back(
            missingRateError(makeDiagnosticMessage(MessageKey::MissingExchangeRateNoRateOnDate,
                                                   {currencyParam, dateParam}),
                             aPair,
                             aTaxDate));
    };

    if (const auto* exact = aTable.find(code, aTaxDate))
    {
        return exact;
    }

    // After the euro date the currency has no rate and never gets a nearby one.
    const auto euroDate = euroReplacementDate(aPair.mCurrency);

    if (euroDate && aTaxDate >= *euroDate)
    {
        aSelection.mDiagnostics.push_back(missingRateError(
            makeDiagnosticMessage(
                MessageKey::MissingExchangeRateReplacedByEur,
                {currencyParam, dateParam, textParam("euroDate", formatCalendarDate(*euroDate))}),
            aPair,
            aTaxDate));

        return nullptr;
    }

    // The files must vouch for the tax date itself before any earlier day is tried.
    if (auto uncovered = uncoveredDay(aTable, code, aTaxDate, aTaxDate))
    {
        uncoveredError(std::move(*uncovered));

        return nullptr;
    }

    // The ECB published rates on this day, but not for this currency: it was stopped.
    if (aPublicationDays.contains(aTaxDate))
    {
        noRateOnDate();

        return nullptr;
    }

    for (int daysBack = 1; daysBack <= kMaxRateDaysBack; ++daysBack)
    {
        const Date candidate = aTaxDate - DayDuration{daysBack};

        if (auto uncovered = uncoveredDay(aTable, code, aTaxDate, candidate))
        {
            uncoveredError(std::move(*uncovered));

            return nullptr;
        }

        if (const auto* earlier = aTable.find(code, candidate))
        {
            aSelection.mDiagnostics.push_back(pairDiagnostic(
                DiagnosticSeverity::Warning,
                ProcessingDiagnosticCode::OfficialRateFromNearbyDate,
                makeDiagnosticMessage(MessageKey::OfficialRateFromNearbyDate,
                                      {currencyParam,
                                       dateParam,
                                       textParam("rateDate", formatCalendarDate(candidate))}),
                aPair,
                aTaxDate));

            return earlier;
        }

        // A publication day without this currency: the currency stopped, so do not look further.
        if (aPublicationDays.contains(candidate))
        {
            break;
        }
    }

    noRateOnDate();

    return nullptr;
}

} // namespace

const OfficialRate* OfficialRateSelection::find(Currency aCurrency, Date aTaxDate) const {
    const auto found = mSelected.find({std::string{currencyCode(aCurrency)}, aTaxDate});

    return found == mSelected.end() ? nullptr : &found->second;
}

const RateFileNeed* OfficialRateSelection::findMissingFile(Currency aCurrency,
                                                           Date aTaxDate) const {
    const auto found = mMissingFiles.find({std::string{currencyCode(aCurrency)}, aTaxDate});

    return found == mMissingFiles.end() ? nullptr : &found->second;
}

const ProcessingDiagnostic* OfficialRateSelection::findMissing(Currency aCurrency,
                                                               Date aTaxDate) const {
    const auto found = mMissing.find({std::string{currencyCode(aCurrency)}, aTaxDate});

    return found == mMissing.end() ? nullptr : &found->second;
}

OfficialRateSelection selectOfficialRates(const std::vector<RateNeed>& aNeeds,
                                          const OfficialRateTable& aTable) {
    // Ordered by tax date, then currency code, so the diagnostics come in that order.
    std::map<std::pair<Date, std::string>, PairNeed> pairs;

    for (const auto& need : aNeeds)
    {
        if (need.mCurrency == Currency::EUR)
        {
            continue;
        }

        if (need.mCurrency == Currency::Unknown)
        {
            throw std::logic_error("A rate need has an unknown currency");
        }

        auto& pair = pairs[{need.mTaxDate, std::string{currencyCode(need.mCurrency)}}];

        pair.mCurrency = need.mCurrency;
        pair.mReports.insert(need.mReport);
        pair.mSources.insert(need.mSources.begin(), need.mSources.end());
    }

    // The days on which the ECB published any rate.
    const std::set<Date> publicationDays = aTable.publicationDays();

    OfficialRateSelection selection;
    std::map<int, FileUse> fileUses;

    for (const auto& [key, pair] : pairs)
    {
        const auto* rate = resolvePair(aTable, publicationDays, pair, key.first, selection);

        if (rate != nullptr)
        {
            selection.mSelected.emplace(std::make_pair(key.second, key.first), *rate);
        }
        else
        {
            // A pair without a rate has exactly one error, which resolvePair added last.
            selection.mMissing.emplace(std::make_pair(key.second, key.first),
                                       selection.mDiagnostics.back());
        }

        const auto* file = fileOfYear(aTable, yearOf(key.first));

        if (file == nullptr)
        {
            continue;
        }

        auto& use = fileUses[file->mYear];

        if (use.mNeeds.mReports.empty())
        {
            use.mFirstTaxDate = key.first;
        }

        addPairNeed(use.mNeeds, pair);

        if (key.first > file->mLastDate)
        {
            if (use.mNeedsAfterEnd.mReports.empty())
            {
                use.mFirstTaxDateAfterEnd = key.first;
            }

            addPairNeed(use.mNeedsAfterEnd, pair);
        }
    }

    // After the pair diagnostics, in year order.
    for (const auto& [year, use] : fileUses)
    {
        addFileWarnings(*fileOfYear(aTable, year), use, selection);
    }

    return selection;
}

NumericResult<Money> convertOfficialMoney(Money aForeignMoney,
                                          const OfficialRate& aRate,
                                          const DestinationRange& aRange) {
    return convertMoney(aForeignMoney, aRate.mValue, ExchangeRateDirection::ForeignPerEur, aRange);
}

NumericResult<UnitPrice> convertOfficialUnitPrice(UnitPrice aForeignUnitPrice,
                                                  const OfficialRate& aRate,
                                                  const DestinationRange& aRange) {
    return convertUnitPrice(aForeignUnitPrice,
                            aRate.mValue,
                            ExchangeRateDirection::ForeignPerEur,
                            aRange);
}

} // namespace taxbroker
