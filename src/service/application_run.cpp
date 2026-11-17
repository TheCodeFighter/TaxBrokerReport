#include "taxbroker/application_run.hpp"

#include "parsers/traderepublic_parser.hpp"
#include "processors/application_summaries.hpp"
#include "processors/capital_gains_report_builder.hpp"
#include "processors/capital_report_gates.hpp"
#include "processors/corporate_actions.hpp"
#include "processors/failure_resolution_stage.hpp"
#include "processors/fee_summary.hpp"
#include "processors/fifo_matcher.hpp"
#include "processors/income_reports.hpp"
#include "processors/loss_eligibility.hpp"
#include "processors/tax_classifier.hpp"
#include "processors/tax_processor.hpp"
#include "processors/trade_conversion.hpp"
#include "taxbroker/logging.hpp"
#include "utils/tax_number.hpp"
#include "parsers/ecb_rate_file.hpp"
#include "xml/report_xml_stage.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace taxbroker {
namespace {

/// The milliseconds since a start time, for a stage line.
long long millisecondsSince(std::chrono::steady_clock::time_point aStart) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 aStart)
        .count();
}

/// The counts of a run request as one text, for example `brokers [Trade Republic x2]`. Names a
/// broker, never a file; the other kinds are counts.
std::string requestLogSummary(const ApplicationRequest& aRequest) {
    std::map<std::string_view, std::size_t> brokers;

    for (const BrokerInputFile& file : aRequest.mBrokerFiles)
    {
        ++brokers[brokerName(file.mBroker)];
    }

    std::string text = "brokers [";

    for (const auto& [name, count] : brokers)
    {
        text += text.back() == '[' ? "" : ", ";
        text += name;
        text += " x" + std::to_string(count);
    }

    return text + "], ecb rate files " + std::to_string(aRequest.mEcbRateFiles.size()) +
           ", reference files " + std::to_string(aRequest.mReferenceFiles.size());
}

/// One info line for a started or restarted run. The taxpayer is never in it.
void logRunStart(std::string_view aEvent, const ApplicationRequest& aRequest) {
    LOG_INFO("Run {}: year {}, filing {}, developer mode {}, {}",
             aEvent,
             aRequest.mReportingYear,
             aRequest.mFilingType == FilingType::Original ? "original" : "self_report",
             aRequest.mDeveloperMode ? "on" : "off",
             requestLogSummary(aRequest));
}

/// The info lines of the load stages that parse and merge the files and prepare the year: counts
/// only, never a value, a name or a file name.
void logLoadStages(const ApplicationRequest& aRequest,
                   const TaxProcessingResult& aPrepared,
                   long long aParseAndMergeMs) {
    std::size_t parseErrors = 0;
    std::size_t parseWarnings = 0;
    std::size_t mergeErrors = 0;
    std::size_t mergeWarnings = 0;
    std::size_t conflicts = 0;

    for (const StatementMergeDiagnostic& diagnostic : aPrepared.mMerged.mDiagnostics)
    {
        if (const auto* parse = std::get_if<SourcedParseDiagnostic>(&diagnostic))
        {
            const bool isError = parse->mDiagnostic.mSeverity == DiagnosticSeverity::Error;

            ++(isError ? parseErrors : parseWarnings);
            continue;
        }

        const auto& merge = std::get<MergeDiagnostic>(diagnostic);
        const bool isError = merge.mSeverity == DiagnosticSeverity::Error;

        ++(isError ? mergeErrors : mergeWarnings);
        conflicts += merge.mCode == MergeDiagnosticCode::ConflictingDuplicate ? 1U : 0U;
    }

    LOG_INFO("Stage parse and merge: sources {}, events {}, parse errors {}, parse warnings {}, "
             "merge errors {}, merge warnings {}, conflicts {} ({} ms)",
             aRequest.mBrokerFiles.size(),
             aPrepared.mMerged.mStatement.mChronologicalOrder.size(),
             parseErrors,
             parseWarnings,
             mergeErrors,
             mergeWarnings,
             conflicts,
             aParseAndMergeMs);

    if (aPrepared.mLedger.has_value())
    {
        LOG_INFO("Stage year: instruments {}, capital events {}, events of the year {}",
                 aPrepared.mMerged.mStatement.mPresentation.mTradeInstruments.size(),
                 aPrepared.mLedger->mCapitalEvents.size(),
                 aPrepared.mLedger->mSelectedYearEvents.size());
    }
    else
    {
        LOG_INFO("Stage year: no ledger, the year {} is not supported", aRequest.mReportingYear);
    }
}

/// The ISINs that the builder blocked, for a preview of the rows of every other ISIN.
std::vector<Isin> blockedIsins(const CapitalGainsBuildResult& aBuilt) {
    std::vector<Isin> isins;

    std::transform(aBuilt.mBlocked.begin(),
                   aBuilt.mBlocked.end(),
                   std::back_inserter(isins),
                   [](const KdvpBlockedIsin& aBlocked) { return aBlocked.mIsin; });

    return isins;
}

/// The warning for a taxpayer tax number whose check digit is wrong. It is about every form, so it
/// names no report; the number itself is never in the text.
ProcessingDiagnostic taxNumberCheckDigitWarning() {
    auto message = makeDiagnosticMessage(MessageKey::TaxpayerTaxNumberCheckDigit);

    return ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Warning,
        .mCode = ProcessingDiagnosticCode::TaxpayerTaxNumberCheckDigit,
        .mContext = {.mStage = PipelineStage::XmlGeneration},
        .mMessage = std::move(message.mEnglish),
        .mMessageKey = message.mKey,
        .mMessageParams = std::move(message.mParams),
    };
}

/// The investments the developer export is offered for, and one warning for each investment with
/// missing history that it cannot be written for.
struct DeveloperEligibility {
    std::vector<Isin> mIsins;
    std::vector<ProcessingDiagnostic> mWarnings;
};

/// The warning that the developer export was left out for a pool, with the sale without history
/// as its source.
ProcessingDiagnostic developerUnavailableWarning(const std::vector<TradeInstrument>& aInstruments,
                                                 std::size_t aInstrumentIndex,
                                                 const FifoIsinResult& aPool,
                                                 DeveloperExportCheck aCheck) {
    const bool tooLarge = aCheck == DeveloperExportCheck::UnitValueTooLarge;
    auto message = makeDiagnosticMessage(
        tooLarge ? MessageKey::DeveloperExportUnavailableUnitValueTooLarge
                 : MessageKey::DeveloperExportUnavailableCorporateActionAfterGap);
    const auto& sale =
        aInstruments.at(aInstrumentIndex).mTransactions.at(aPool.mStoppedAt->mEventIndex);

    return ProcessingDiagnostic{
        .mSeverity = DiagnosticSeverity::Warning,
        .mCode = ProcessingDiagnosticCode::DeveloperExportUnavailable,
        .mContext =
            {
                .mStage = PipelineStage::HistoricalValidation,
                .mAffectedReports = {ReportType::CapitalGains},
                .mSources = sale.mMetadata.mSources,
                .mTaxDate = sale.mMetadata.mTaxDate,
                .mInstrumentName = aPool.mName,
                .mIsin = aPool.mIsin,
            },
        .mMessage = std::move(message.mEnglish),
        .mMessageKey = message.mKey,
        .mMessageParams = std::move(message.mParams),
    };
}

/// The ISINs whose FIFO pool stopped at a sale with too few units and whose developer list can be
/// built. Empty, and no walk over the pools, when developer mode is off.
DeveloperEligibility developerEligibility(bool aDeveloperMode,
                                          const std::vector<TradeInstrument>& aInstruments,
                                          const ReportingYearLedger& aLedger,
                                          const FifoMatchingResult& aFifo) {
    DeveloperEligibility eligibility;

    if (!aDeveloperMode)
    {
        return eligibility;
    }

    for (std::size_t index = 0; index < aFifo.mIsins.size(); ++index)
    {
        const auto& pool = aFifo.mIsins.at(index);

        // A left-out instrument has no pool run, so it is never stopped for missing history.
        const auto check = CapitalGainsReportBuilder::checkDeveloperExport(aLedger, pool, index);

        if (check == DeveloperExportCheck::Possible)
        {
            eligibility.mIsins.push_back(pool.mIsin);
        }
        else if (check != DeveloperExportCheck::NotApplicable)
        {
            eligibility.mWarnings.push_back(
                developerUnavailableWarning(aInstruments, index, pool, check));
        }
    }

    return eligibility;
}

/// True when an instrument that a tax rule or the user left out has a sale in the year, or an
/// excluded bond has matured units. Only then did the year have a disposal that is not reported.
bool hasLeftOutDisposal(const MergedStatement& aStatement,
                        const ReportingYearLedger& aLedger,
                        const std::vector<ExcludedItemInput>& aExcludedItems) {
    const auto& instruments = aStatement.mPresentation.mTradeInstruments;

    for (const auto& instrument : instruments)
    {
        if (!aLedger.isLeftOut(instrument.mIsin))
        {
            continue;
        }

        const auto& trades = instrument.mTransactions;
        const bool hasSale =
            std::any_of(trades.begin(), trades.end(), [&aLedger](const auto& aTrade) {
                const auto date = aTrade.mMetadata.mTaxDate;

                return aTrade.mTradeSide == TradeSide::Sell && date >= aLedger.mYearStart &&
                       date <= aLedger.mYearEnd;
            });

        if (hasSale)
        {
            return true;
        }
    }

    const bool hasMaturedUnits =
        std::any_of(aExcludedItems.begin(),
                    aExcludedItems.end(),
                    [](const ExcludedItemInput& aInput) {
                        return aInput.mItem.mBond && aInput.mItem.mBond->mMaturedUnits != 0;
                    });

    if (hasMaturedUnits)
    {
        return true;
    }

    return false;
}

/// One calculation from the merged files and the answers: FIFO, the corporate-action stage, the
/// loss rule, the builder, the gates, the choice to leave failed investments out and the XML. It
/// keeps no state between calls: everything the summary needs goes into the input of the result.
ApplicationResultInput calculateReport(const ApplicationRequest& aRequest,
                                       const TaxProcessingResult& aPrepared,
                                       const ReferenceCatalogue& aCatalogue,
                                       const OfficialRateTable& aRates,
                                       const ApplicationDataSources& aDataSources,
                                       RunDecisions& aDecisions) {
    // A year that is not supported has no ledger: only the diagnostics of the merge and of the
    // year check are reported, and every form fails with them.
    if (!aPrepared.mLedger)
    {
        ApplicationResultInput failed{
            .mReportingYear = aRequest.mReportingYear,
            .mMergeDiagnostics = aPrepared.mMerged.mDiagnostics,
            .mProcessingDiagnostics = aPrepared.mDiagnostics,
        };

        failed.mDataSources = aDataSources;

        return failed;
    }

    // The classification comes first: the instruments it leaves out are not part of the capital
    // stages.
    auto ledger = *aPrepared.mLedger;
    const auto& statement = aPrepared.mMerged.mStatement;
    auto classification =
        TaxClassifier::classify(statement, ledger, aPrepared.mMerged.mDiagnostics, aDecisions);

    ledger.mLeftOutIsins = classification.mLeftOutIsins;

    LOG_DEBUG("Stage classify: instruments {}, left out {}",
              statement.mPresentation.mTradeInstruments.size(),
              ledger.mLeftOutIsins.size());

    // Foreign trades are converted to EUR before FIFO, so every later stage reads EUR prices.
    auto conversion = convertCapitalTrades(statement, ledger, aRates);
    const auto& convertedStatement = conversion.statement(statement);

    auto pools = matchCapitalPools(convertedStatement, ledger, aDecisions);
    auto& fifo = pools.mFifo;
    auto& actions = pools.mActions;
    auto& quantities = pools.mQuantities;

    LOG_DEBUG("Stage fifo: pools {}, diagnostics {}", fifo.mIsins.size(), fifo.mDiagnostics.size());

    // The stages after the pools read the sale units after the quantity corrections.
    const auto& capitalStatement = pools.statement(convertedStatement);
    auto loss = LossEligibilityChecker::check(capitalStatement, ledger, fifo, aDecisions);
    auto built = CapitalGainsReportBuilder::build(capitalStatement, ledger, fifo, loss, {});
    auto gateDiagnostics =
        CapitalReportGates::check(capitalStatement, ledger, fifo, loss, built, conversion.mRates);

    LOG_DEBUG("Stage loss and builder: loss requests {}, blocked ISINs {}, excluded ISINs {}, "
              "lists {}",
              loss.mRequests.size(),
              built.mBlocked.size(),
              built.mExcluded.size(),
              built.mReport ? built.mReport->mLists.size() : 0U);

    // Copied, not moved: every calculation of the run starts from the same diagnostics.
    ApplicationResultInput input{
        .mReportingYear = aRequest.mReportingYear,
        .mMergeDiagnostics = aPrepared.mMerged.mDiagnostics,
        .mProcessingDiagnostics = aPrepared.mDiagnostics,
    };
    auto& processing = input.mProcessingDiagnostics;

    appendClassification(input, std::move(classification));
    std::move(conversion.mDiagnostics.begin(),
              conversion.mDiagnostics.end(),
              std::back_inserter(processing));
    std::move(fifo.mDiagnostics.begin(), fifo.mDiagnostics.end(), std::back_inserter(processing));
    appendStageOutput(input, std::move(actions.mDiagnostics), std::move(actions.mRequests));
    appendStageOutput(input, std::move(quantities.mDiagnostics), std::move(quantities.mRequests));
    appendStageOutput(input, std::move(loss.mDiagnostics), std::move(loss.mRequests));

    const auto firstGateDiagnostic = processing.size();

    std::move(gateDiagnostics.begin(), gateDiagnostics.end(), std::back_inserter(processing));

    // The user may leave out every investment with an error of its own (or ask for no report).
    auto developerChoices = developerEligibility(aRequest.mDeveloperMode,
                                                 capitalStatement.mPresentation.mTradeInstruments,
                                                 ledger,
                                                 fifo);

    std::move(developerChoices.mWarnings.begin(),
              developerChoices.mWarnings.end(),
              std::back_inserter(processing));

    if (!isSlovenianTaxNumber(aRequest.mTaxpayer.mTaxNumber))
    {
        processing.push_back(taxNumberCheckDigitWarning());
    }

    const auto capitalChoice = FailureResolutionStage::resolveUnits(
        input,
        aDecisions,
        kCapitalFailure,
        DeveloperOffer{.mEnabled = aRequest.mDeveloperMode,
                       .mEligibleIsins = std::move(developerChoices.mIsins)});
    const auto& excluded = capitalChoice.mIsins;
    const auto& developer = capitalChoice.mDeveloperIsins;

    if (!excluded.empty() || !developer.empty())
    {
        built = CapitalGainsReportBuilder::build(capitalStatement,
                                                 ledger,
                                                 fifo,
                                                 loss,
                                                 excluded,
                                                 developer);
    }

    std::optional<CapitalGainsReportKDVP> previewWithoutBlocked;

    if (!built.mReport)
    {
        auto leftOut = excluded;
        const auto blocked = blockedIsins(built);

        leftOut.insert(leftOut.end(), blocked.begin(), blocked.end());
        previewWithoutBlocked =
            CapitalGainsReportBuilder::build(capitalStatement, ledger, fifo, loss, leftOut).mReport;
    }

    // A gate error of an excluded investment was resolved and does not stop the XML.
    bool hasGateError = false;

    for (auto position = firstGateDiagnostic; position < processing.size(); ++position)
    {
        const bool resolved = std::find(input.mResolvedDiagnostics.begin(),
                                        input.mResolvedDiagnostics.end(),
                                        position) != input.mResolvedDiagnostics.end();

        hasGateError = hasGateError ||
                       (processing[position].mSeverity == DiagnosticSeverity::Error && !resolved);
    }

    const ReportMetadata metadata{
        .mTaxpayer = aRequest.mTaxpayer,
        .mReportingYear = aRequest.mReportingYear,
        .mFilingType = aRequest.mFilingType,
    };

    // A blocked ISIN always has an error (FIFO, corporate action or gate), so without a report
    // the capital form is failed or needs input, never empty.
    if (built.mReport && !hasGateError)
    {
        // The warnings of the rows that the XML lists; without XML there are none.
        auto basisDifferences = basisDifferenceDiagnostics(capitalStatement, built);

        std::move(basisDifferences.begin(), basisDifferences.end(), std::back_inserter(processing));

        if (built.mReport->mLists.empty())
        {
            // When investments were left out, nothing is left to report, which is not the same
            // as having no sale.
            if (excluded.empty() && !hasLeftOutDisposal(statement, ledger, input.mExcludedItems))
            {
                auto& capital = input.mReports[static_cast<std::size_t>(ReportType::CapitalGains)];

                capital.mNoDataReason = NoDataReason::NoSelectedYearDisposals;
            }
        }
        else
        {
            writeReportXml(input, metadata, {.mCapitalGains = &*built.mReport});
        }
    }

    // The income stages come last, so nothing in the capital output or its question order moves.
    // Each form keeps its own status: a form whose stage has an error or a question is not
    // generated, whatever the writer made, and the other forms are not touched.
    auto income = prepareIncomeReports(input, statement, ledger, aCatalogue, aRates, aDecisions);

    LOG_DEBUG("Stage income: dividend rows {}, deposit interest rows {}, other interest rows {}",
              income.mDividends.mRows.size(),
              income.mDeposit.mRows.size(),
              income.mOther.mRows.size());

    writeReportXml(
        input,
        metadata,
        {.mDividends = &income.mDividends, .mDeposit = &income.mDeposit, .mOther = &income.mOther});

    LOG_DEBUG("Stage xml: forms written {}",
              std::count_if(input.mReports.begin(), input.mReports.end(), [](const auto& aReport) {
                  return aReport.mXml.has_value();
              }));

    // The summary reads the preview: the report, or the rows of the ISINs without a problem.
    const auto& preview = built.mReport ? *built.mReport : previewWithoutBlocked.value();

    appendCapitalSummary(input, capitalStatement, built, preview);
    appendCorrections(input, capitalStatement, fifo);
    input.mUsedRates = mergedRates(std::move(conversion.mUsedRates), std::move(income.mUsedRates));
    input.mDataSources = aDataSources;

    // Information only: the summaries read the merged events and never change a stage's output.
    auto summaries = summarizeFeesAndBenefits(statement, ledger);

    input.mFees = std::move(summaries.mFees);
    input.mBenefits = std::move(summaries.mBenefits);

    return input;
}

/// Parses every broker file with the parser of its broker and merges them; the position in the
/// request is the source index.
StatementMergeResult parseAndMerge(const std::vector<BrokerInputFile>& aFiles) {
    tr::TradeRepublicParser tradeRepublic;
    std::vector<StatementMergeInput> inputs;

    for (std::size_t index = 0; index < aFiles.size(); ++index)
    {
        const auto& file = aFiles[index];

        switch (file.mBroker)
        {
        case Broker::TradeRepublic:
            inputs.push_back(
                {index, tradeRepublic.parse(file.mFile.mContent, file.mFile.mName, index)});
            break;
        case Broker::InteractiveBrokers:
        case Broker::Unknown:
            // No other parser exists yet.
            throw std::logic_error("A broker file has a broker without a parser.");
        }
    }

    return DeterministicStatementMerger{}.merge(std::move(inputs));
}

/// What a request loads before any calculation: the files, the data sources and the merged
/// statement of the year.
struct RunData {
    ApplicationRequest mRequest;
    OfficialRateTable mRates;
    ReferenceCatalogue mCatalogue;
    ApplicationDataSources mDataSources;
    /// Parsed, merged and prepared once; every calculation of the session reads it.
    TaxProcessingResult mPrepared;
};

/// Loads the rate and reference files of the request and prepares the year. A supplied file that
/// is not valid gives a fault. Parsing, merging and the year do not depend on an answer.
std::variant<ApplicationInputFault, RunData> loadRunData(ApplicationRequest aRequest) {
    // The files are named by the user, so a file that is not valid fails the run.
    auto rates = loadOfficialRates(aRequest.mEcbRateFiles);

    if (!rates.mDiagnostics.empty())
    {
        return ApplicationInputFault{RateFileFault{std::move(rates.mDiagnostics.front())}};
    }

    auto reference = loadReferenceData(aRequest.mReferenceFiles);

    if (reference.mFault)
    {
        return ApplicationInputFault{std::move(*reference.mFault)};
    }

    LOG_INFO("Stage rates: supplied files {}", aRequest.mEcbRateFiles.size());
    LOG_INFO("Stage reference: supplied files {}", aRequest.mReferenceFiles.size());

    auto dataSources = dataSourcesOf(reference.mOrigins, rates.mTable);
    const auto parseStart = std::chrono::steady_clock::now();
    auto prepared = TaxProcessor::prepareReportingYear(TaxProcessingRequest{
        .mReportingYear = aRequest.mReportingYear,
        .mMerged = parseAndMerge(aRequest.mBrokerFiles),
    });

    logLoadStages(aRequest, prepared, millisecondsSince(parseStart));

    return RunData{.mRequest = std::move(aRequest),
                   .mRates = std::move(rates.mTable),
                   .mCatalogue = std::move(reference.mCatalogue.value()),
                   .mDataSources = std::move(dataSources),
                   .mPrepared = std::move(prepared)};
}

} // namespace

/// The data a run owns, and its decision session.
struct ApplicationRun::Impl {
    /// The data of the current run. The calculation of the session holds its own share of them.
    std::shared_ptr<const RunData> mData;
    DecisionSession mSession;

    explicit Impl(RunData aData) : mData(std::make_shared<const RunData>(std::move(aData))) {
        mSession.start(calculationOn(mData));
    }

    /// Starts a new run on the new data and keeps the recorded answers. If the calculation
    /// throws, the run keeps its previous data and the session stays as it was.
    void restart(RunData aData) {
        auto data = std::make_shared<const RunData>(std::move(aData));

        mSession.start(calculationOn(data));
        mData = std::move(data);
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

  private:
    /// One calculation from the given data; it keeps them alive for as long as the session does.
    static RunCalculation calculationOn(std::shared_ptr<const RunData> aData) {
        return [data = std::move(aData)](RunDecisions& aDecisions) {
            const auto start = std::chrono::steady_clock::now();
            auto input = calculateReport(data->mRequest,
                                         data->mPrepared,
                                         data->mCatalogue,
                                         data->mRates,
                                         data->mDataSources,
                                         aDecisions);

            LOG_INFO("Calculation done ({} ms)", millisecondsSince(start));

            return input;
        };
    }
};

ApplicationRun::ApplicationRun(std::unique_ptr<Impl> aImpl) : mImpl(std::move(aImpl)) {}

ApplicationRun::ApplicationRun(ApplicationRun&&) noexcept = default;

ApplicationRun& ApplicationRun::operator=(ApplicationRun&&) noexcept = default;

ApplicationRun::~ApplicationRun() = default;

const ApplicationRequest& ApplicationRun::request() const {
    return mImpl->mData->mRequest;
}

const ApplicationResult& ApplicationRun::result() const {
    return mImpl->mSession.result();
}

DecisionSession& ApplicationRun::session() {
    return mImpl->mSession;
}

std::variant<ApplicationInputFault, ApplicationRun>
ApplicationRun::start(ApplicationRequest aRequest) {
    auto loaded = loadRunData(std::move(aRequest));

    if (auto* fault = std::get_if<ApplicationInputFault>(&loaded))
    {
        return std::move(*fault);
    }

    logRunStart("started", std::get<RunData>(loaded).mRequest);

    return ApplicationRun{std::make_unique<Impl>(std::move(std::get<RunData>(loaded)))};
}

std::optional<ApplicationInputFault> ApplicationRun::restart(ApplicationRequest aRequest) {
    auto loaded = loadRunData(std::move(aRequest));

    if (auto* fault = std::get_if<ApplicationInputFault>(&loaded))
    {
        return std::move(*fault);
    }

    logRunStart("restarted", std::get<RunData>(loaded).mRequest);
    mImpl->restart(std::move(std::get<RunData>(loaded)));

    return std::nullopt;
}

} // namespace taxbroker
