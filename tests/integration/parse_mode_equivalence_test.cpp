// Sequential and concurrent parsing give the same result. Every scenario runs the whole
// application (`ApplicationRun`) once in each parse mode below and compares everything the user
// sees: the result JSON of the HTTP API (sources and source IDs, diagnostics and their IDs in
// order, requests, decisions, exclusions, the summaries) and the XML of all four forms, byte for
// byte. The parse mode never changes the result, so the first mode, one file at a time in request
// order, is the reference. Each test also checks literals from the fixture README or a golden
// file, so the reference is not only the output of the current code.
//
// All files are synthetic: `tests/test_data/pipeline_mixed` (one Trade Republic and two
// Interactive Brokers files, their README and the golden Doh-KDVP of 2024) and
// `tests/test_data/manual` (four Trade Republic and four Interactive Brokers files, 2022 to 2025;
// its README lists the questions and answers of each year).

#include "cli/decision_prompts.hpp"
#include "taxbroker/api/answer_json.hpp"
#include "taxbroker/api/application_result_json.hpp"
#include "taxbroker/application_request.hpp"
#include "taxbroker/application_result.hpp"
#include "taxbroker/application_run.hpp"
#include "taxbroker/diagnostic_messages.hpp"
#include "taxbroker/input_file.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace taxbroker;

/// How the files of a run are parsed.
struct ParseMode {
    std::string_view mName;     ///< For the trace of a failed check.
    std::size_t mWorkerLimit{}; ///< 1 parses one file at a time on the calling thread.
    bool mReversed{};           ///< Take the files last to first.
    bool mInterleaved{};        ///< Take the files 1, 3, 5, ..., then 0, 2, 4, ...
};

// The reference first. One worker with the files taken last to first makes the parsers finish in
// exactly the reverse order, on any machine. With 4 or 8 workers (fewer when the machine has
// fewer hardware threads) the files are parsed at the same time and finish in a varied order.
constexpr std::array<ParseMode, 4> kModes{{
    {.mName = "one worker, request order", .mWorkerLimit = 1},
    {.mName = "one worker, reversed", .mWorkerLimit = 1, .mReversed = true},
    {.mName = "four workers, reversed", .mWorkerLimit = 4, .mReversed = true},
    {.mName = "eight workers, interleaved", .mWorkerLimit = 8, .mInterleaved = true},
}};

/// The run options of a mode for a request of `aFileCount` files.
ApplicationRunOptions optionsOf(const ParseMode& aMode, const std::size_t aFileCount) {
    ApplicationRunOptions options{.mParseWorkerLimit = aMode.mWorkerLimit};

    if (aMode.mReversed)
    {
        for (std::size_t index = aFileCount; index > 0; --index)
        {
            options.mParseStartOrder.push_back(index - 1);
        }
    }

    if (aMode.mInterleaved)
    {
        for (std::size_t index = 1; index < aFileCount; index += 2)
        {
            options.mParseStartOrder.push_back(index);
        }

        for (std::size_t index = 0; index < aFileCount; index += 2)
        {
            options.mParseStartOrder.push_back(index);
        }
    }

    return options;
}

/// A committed file under `tests/test_data`, read byte for byte.
std::string readTestData(std::string_view aFolder, std::string_view aName) {
    const std::filesystem::path path =
        std::filesystem::path{__FILE__}.parent_path().parent_path() / "test_data" / aFolder / aName;
    std::ifstream input{path, std::ios::binary};

    if (!input.is_open())
    {
        throw std::runtime_error{"Unable to open the test file " + std::string{aName}};
    }

    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

BrokerInputFile brokerFile(const Broker aBroker, std::string aName, std::string aContent) {
    return {.mBroker = aBroker,
            .mFile = {.mName = std::move(aName), .mContent = std::move(aContent)}};
}

/// The taxpayer of the golden files.
TaxpayerMetadata syntheticTaxpayer() {
    return {.mTaxNumber = "12345679",
            .mName = "Synthetic Taxpayer",
            .mAddress = "Synthetic Street 1",
            .mCity = "Synthetic City",
            .mPostCode = "1000",
            .mPostName = "Synthetic City",
            .mBirthDate = *parseCalendarDate("1990-01-31"),
            .mIsResident = true,
            .mEmail = "taxpayer@example.com",
            .mPhoneNumber = "+38600000000"};
}

/// A request of 2024 with the synthetic taxpayer and the given files, in this order.
ApplicationRequest requestOf2024(std::vector<BrokerInputFile> aFiles) {
    return {.mReportingYear = 2024,
            .mFilingType = FilingType::Original,
            .mTaxpayer = syntheticTaxpayer(),
            .mBrokerFiles = std::move(aFiles)};
}

/// The Trade Republic file and the Interactive Brokers file of 2023 and 2024 of the mixed
/// pipeline, in this order.
std::vector<BrokerInputFile> mixedFiles() {
    return {brokerFile(Broker::TradeRepublic,
                       "trade_republic_2021_2024.csv",
                       readTestData("pipeline_mixed", "trade_republic_2021_2024.csv")),
            brokerFile(Broker::InteractiveBrokers,
                       "ibkr_2023_2024.xml",
                       readTestData("pipeline_mixed", "ibkr_2023_2024.xml"))};
}

/// The eight manual files in the order of their README: Trade Republic 2022 to 2025 (sources 0
/// to 3), then Interactive Brokers 2022 to 2025 (sources 4 to 7).
std::vector<BrokerInputFile> manualFiles() {
    std::vector<BrokerInputFile> files;

    for (const std::string_view year : {"2022", "2023", "2024", "2025"})
    {
        const std::string name = "trade_republic_" + std::string{year} + ".csv";

        files.push_back(brokerFile(Broker::TradeRepublic, name, readTestData("manual", name)));
    }

    for (const std::string_view year : {"2022", "2023", "2024", "2025"})
    {
        const std::string name = "ibkr_" + std::string{year} + ".xml";

        files.push_back(brokerFile(Broker::InteractiveBrokers, name, readTestData("manual", name)));
    }

    return files;
}

/// Everything of a result that the user sees.
struct RunSnapshot {
    std::string mResultJson;
    std::array<std::optional<std::string>, 4> mXml; ///< In report order; empty without a form.
};

RunSnapshot snapshotOf(const ApplicationResult& aResult) {
    return {.mResultJson = api::serializeApplicationResultJson(aResult),
            .mXml = {aResult.report(ReportType::CapitalGains).mXml,
                     aResult.report(ReportType::Dividends).mXml,
                     aResult.report(ReportType::BankDepositInterest).mXml,
                     aResult.report(ReportType::OtherInterest).mXml}};
}

void expectSameSnapshot(const RunSnapshot& aActual, const RunSnapshot& aReference) {
    EXPECT_EQ(aActual.mResultJson, aReference.mResultJson);
    EXPECT_EQ(aActual.mXml[0], aReference.mXml[0]);
    EXPECT_EQ(aActual.mXml[1], aReference.mXml[1]);
    EXPECT_EQ(aActual.mXml[2], aReference.mXml[2]);
    EXPECT_EQ(aActual.mXml[3], aReference.mXml[3]);
}

/// The run, or an empty optional (and a failed test) when the files were not accepted.
std::optional<ApplicationRun> startRun(ApplicationRequest aRequest,
                                       const ApplicationRunOptions& aOptions) {
    std::variant<ApplicationInputFault, ApplicationRun> started =
        ApplicationRun::start(std::move(aRequest), aOptions);
    ApplicationRun* run = std::get_if<ApplicationRun>(&started);

    EXPECT_NE(run, nullptr);

    if (run == nullptr)
    {
        return std::nullopt;
    }

    return std::optional<ApplicationRun>{std::move(*run)};
}

/// Gives the answers of one HTTP answer body to the run; a failed check if any is rejected.
void answer(ApplicationRun& aRun, std::string_view aBody) {
    std::variant<api::AnswerCall, api::CallBodyError> parsed =
        api::parseAnswerCallJson(aBody, aRun.result());
    api::AnswerCall* call = std::get_if<api::AnswerCall>(&parsed);

    ASSERT_NE(call, nullptr);
    ASSERT_TRUE(call->mShapeRejections.empty());

    const std::vector<RejectedAnswer> rejections = aRun.session().answer(std::move(call->mAnswers));

    EXPECT_TRUE(rejections.empty());
}

/// How often `aNeedle` is in `aText`.
std::size_t countOf(std::string_view aText, std::string_view aNeedle) {
    std::size_t count = 0;

    for (std::size_t at = aText.find(aNeedle); at != std::string_view::npos;
         at = aText.find(aNeedle, at + aNeedle.size()))
    {
        ++count;
    }

    return count;
}

/// The types of the open requests, in the order of the result.
std::vector<RequestType> requestTypesOf(const ApplicationResult& aResult) {
    std::vector<RequestType> types;

    std::transform(aResult.mRequests.begin(),
                   aResult.mRequests.end(),
                   std::back_inserter(types),
                   [](const DecisionRequest& aRequest) { return aRequest.mType; });

    return types;
}

/// The diagnostics of one code (of any of the three stage enums), in the order of the result.
template <typename Code>
std::vector<const ApplicationDiagnostic*> diagnosticsOf(const ApplicationResult& aResult,
                                                        const Code aCode) {
    std::vector<const ApplicationDiagnostic*> found;

    for (const ApplicationDiagnostic& diagnostic : aResult.mDiagnostics)
    {
        const Code* code = std::get_if<Code>(&diagnostic.mCode);

        if (code != nullptr && *code == aCode)
        {
            found.push_back(&diagnostic);
        }
    }

    return found;
}

// The answers of the mixed scenario, one call each, as the browser sends them (the same bodies as
// the mixed-broker HTTP tests). The identity answer says that rows 1 and 2 are one action and
// row 3 is another; the loss answer says there was no other activity; then the two ratios.
constexpr std::string_view kGroupingAndLossAnswers =
    R"({"runRevision":1,"answers":[)"
    R"({"requestId":"request-0001","answer":{"type":"grouping","actionOfRow":[1,1,2]}},)"
    R"({"requestId":"request-0002","answer":"no_other_activity"}]})";
constexpr std::string_view kFirstRatioAnswer =
    R"({"runRevision":2,"answers":[{"requestId":"request-0001","answer":)"
    R"({"newShares":2,"oldShares":1,"effectiveDate":"2024-11-10","confirmedUnits":"20"}}]})";
constexpr std::string_view kSecondRatioAnswer =
    R"({"runRevision":3,"answers":[{"requestId":"request-0001","answer":)"
    R"({"newShares":2,"oldShares":1,"effectiveDate":"2024-11-20","confirmedUnits":"40"}}]})";

/// Gives the three answer calls of the mixed scenario, one after the other.
void answerTheMixedQuestions(ApplicationRun& aRun) {
    answer(aRun, kGroupingAndLossAnswers);
    answer(aRun, kFirstRatioAnswer);
    answer(aRun, kSecondRatioAnswer);
}

// The answer lines of 2024 of the manual README, in the order the command line asks: class
// (`capital`), bond type of Bond B (`discount`), deposit total (`no`), reverse split (`1 : 10`,
// the row date, `yes`), payer of NVIDIA (`1`), one split of the two-broker share (`yes`, then
// `2 : 1`, the row date, `yes`), 30-day loss (`yes`), quantity (the units left), ISIN change
// (`yes`, `1 : 1`, the date of the rows), merger (`taxable`, the value and the date of the rows),
// exclusion of Bond B from Doh-Obr (`exclude`).
constexpr std::string_view kManualAnswers2024 =
    "capital\ndiscount\nno\n1 : 10\n\nyes\n1\nyes\n2 : 1\n\nyes\nyes\n\nyes\n1 : 1\n\ntaxable\n"
    "\n\nexclude\n";

TEST(ParseModeEquivalenceTest, MixedFilesAndTheirAnswersGiveTheGoldenDohKdvpInEveryMode) {
    const std::string golden = readTestData("pipeline_mixed", "Doh-KDVP_2024.golden.xml");
    std::vector<std::vector<RunSnapshot>> snapshots;

    for (const ParseMode& mode : kModes)
    {
        SCOPED_TRACE(mode.mName);

        std::optional<ApplicationRun> run =
            startRun(requestOf2024(mixedFiles()), optionsOf(mode, 2));

        ASSERT_TRUE(run.has_value());

        // The split rows of two brokers ask which rows are one action; the loss sale of
        // XX0000000203 asks the loss question.
        const ApplicationResult& started = run->result();

        ASSERT_EQ(started.mRequests.size(), 2U);
        EXPECT_EQ(started.mRequests[0].mId, "request-0001");
        EXPECT_EQ(started.mRequests[0].mType, RequestType::CorporateActionIdentityRequired);
        EXPECT_EQ(started.mRequests[0].mIsin, "XX0000000202");
        EXPECT_EQ(started.mRequests[1].mId, "request-0002");
        EXPECT_EQ(started.mRequests[1].mType, RequestType::LossEligibilityConfirmationRequired);

        std::vector<RunSnapshot> steps{snapshotOf(started)};

        answer(*run, kGroupingAndLossAnswers);
        steps.push_back(snapshotOf(run->result()));
        answer(*run, kFirstRatioAnswer);
        steps.push_back(snapshotOf(run->result()));
        answer(*run, kSecondRatioAnswer);
        steps.push_back(snapshotOf(run->result()));

        const ApplicationResult& done = run->result();

        EXPECT_EQ(done.mRunRevision, 4U);
        EXPECT_TRUE(done.mRequests.empty());
        ASSERT_TRUE(done.report(ReportType::CapitalGains).mXml.has_value());
        EXPECT_EQ(*done.report(ReportType::CapitalGains).mXml, golden);

        snapshots.push_back(std::move(steps));
    }

    ASSERT_EQ(snapshots.size(), kModes.size());

    for (std::size_t mode = 1; mode < kModes.size(); ++mode)
    {
        SCOPED_TRACE(kModes[mode].mName);

        ASSERT_EQ(snapshots[mode].size(), 4U);

        for (std::size_t step = 0; step < 4; ++step)
        {
            expectSameSnapshot(snapshots[mode][step], snapshots[0][step]);
        }
    }
}

TEST(ParseModeEquivalenceTest, TheManualFilesOf2024AskTheSameQuestionsInEveryMode) {
    std::vector<RunSnapshot> snapshots;
    std::vector<std::string> prompts;

    for (const ParseMode& mode : kModes)
    {
        SCOPED_TRACE(mode.mName);

        std::optional<ApplicationRun> run =
            startRun(requestOf2024(manualFiles()), optionsOf(mode, 8));

        ASSERT_TRUE(run.has_value());

        std::istringstream in{std::string{kManualAnswers2024}};
        std::ostringstream out;

        cli::askOpenQuestions(run->session(), in, out);

        const ApplicationResult& done = run->result();

        // The 2024 result of the manual README: every question answered, Doh-KDVP with 10 lists,
        // Doh-Div 5 rows, Doh-DHO no data (deposit answer `no`), Doh-Obr 3 rows without Bond B.
        EXPECT_TRUE(done.mRequests.empty());
        EXPECT_EQ(done.mDecisions.size(), 11U);
        ASSERT_TRUE(done.report(ReportType::CapitalGains).mXml.has_value());
        EXPECT_EQ(countOf(*done.report(ReportType::CapitalGains).mXml, "<KDVPItem>"), 10U);
        ASSERT_TRUE(done.report(ReportType::Dividends).mXml.has_value());
        EXPECT_EQ(countOf(*done.report(ReportType::Dividends).mXml, "<Dividend>"), 5U);
        EXPECT_EQ(done.report(ReportType::BankDepositInterest).mStatus, ReportStatus::NoData);
        ASSERT_TRUE(done.report(ReportType::OtherInterest).mXml.has_value());
        EXPECT_EQ(countOf(*done.report(ReportType::OtherInterest).mXml, "<Interest>"), 3U);

        // The two-broker split share warns that its rows of 2024 come from two files; each broker
        // has margin or leverage rows that are left out.
        EXPECT_EQ(
            diagnosticsOf(done, ProcessingDiagnosticCode::CorporateActionSourcesMismatchPossible)
                .size(),
            1U);
        EXPECT_EQ(diagnosticsOf(done, ProcessingDiagnosticCode::MarginOrLeverageRowsLeftOut).size(),
                  2U);

        snapshots.push_back(snapshotOf(done));
        prompts.push_back(out.str());
    }

    ASSERT_EQ(snapshots.size(), kModes.size());

    for (std::size_t mode = 1; mode < kModes.size(); ++mode)
    {
        SCOPED_TRACE(kModes[mode].mName);

        EXPECT_EQ(prompts[mode], prompts[0]);
        expectSameSnapshot(snapshots[mode], snapshots[0]);
    }
}

TEST(ParseModeEquivalenceTest, SameFileNamesAndRepeatedRowsGiveTheSameResultInEveryMode) {
    // The eight manual files, every Trade Republic file named export.csv and every Interactive
    // Brokers file export.xml, and the Trade Republic file of 2024 a second time as source 8:
    // every Trade Republic row of 2024, the split row of the two-broker share among them, is in
    // two files.
    std::vector<BrokerInputFile> files = manualFiles();

    for (BrokerInputFile& file : files)
    {
        file.mFile.mName = file.mBroker == Broker::TradeRepublic ? "export.csv" : "export.xml";
    }

    files.push_back(brokerFile(Broker::TradeRepublic,
                               "export.csv",
                               readTestData("manual", "trade_republic_2024.csv")));

    // The same files with their own names, once: the names and the repeated file ask nothing new.
    std::optional<ApplicationRun> named =
        startRun(requestOf2024(manualFiles()), optionsOf(kModes[0], 8));

    ASSERT_TRUE(named.has_value());

    const std::vector<RequestType> namedTypes = requestTypesOf(named->result());

    std::vector<RunSnapshot> snapshots;

    for (const ParseMode& mode : kModes)
    {
        SCOPED_TRACE(mode.mName);

        std::optional<ApplicationRun> run = startRun(requestOf2024(files), optionsOf(mode, 9));

        ASSERT_TRUE(run.has_value());

        const ApplicationResult& result = run->result();

        // The repeated rows are exact duplicates: they merge and keep both sources.
        EXPECT_TRUE(diagnosticsOf(result, MergeDiagnosticCode::ConflictingDuplicate).empty());

        EXPECT_EQ(requestTypesOf(result), namedTypes);

        // The question of the two-broker split share (XX0000000116) names its Trade Republic row
        // of 2024-06-10 (`manual-2024-043`) in both files, source 2 and source 8, by the
        // request position of the file and not by its name.
        std::vector<std::size_t> splitRowSources;

        for (const DecisionRequest& request : result.mRequests)
        {
            if (request.mIsin != "XX0000000116")
            {
                continue;
            }

            for (const ApplicationSource& source : request.mSources)
            {
                if (source.mTransactionId == "manual-2024-043")
                {
                    ASSERT_TRUE(source.mFilename.has_value());
                    EXPECT_EQ(source.mFilename->value(), "export.csv");
                    splitRowSources.push_back(source.mSourceIndex);
                }
            }
        }

        EXPECT_EQ(splitRowSources, (std::vector<std::size_t>{2, 8}));

        snapshots.push_back(snapshotOf(result));
    }

    ASSERT_EQ(snapshots.size(), kModes.size());

    for (std::size_t mode = 1; mode < kModes.size(); ++mode)
    {
        SCOPED_TRACE(kModes[mode].mName);

        expectSameSnapshot(snapshots[mode], snapshots[0]);
    }
}

TEST(ParseModeEquivalenceTest, FileErrorsKeepTheOtherFilesInEveryMode) {
    // Source 2, the Trade Republic file of 2024, has the column amount twice in its header (only
    // the header line changes), and source 5, the Interactive Brokers file of 2023, is cut off
    // in the middle of the XML. Both are file-level errors: no row of these two files is read.
    std::vector<BrokerInputFile> files = manualFiles();
    std::string& tradeRepublic2024 = files[2].mFile.mContent;

    tradeRepublic2024.insert(tradeRepublic2024.find('\n'), ",amount");
    files[5] = brokerFile(Broker::InteractiveBrokers,
                          "truncated.xml",
                          readTestData("ibkr/file_level", "truncated.xml"));

    // The parser diagnostics of the six other files, from a run of the eight good files.
    std::optional<ApplicationRun> good =
        startRun(requestOf2024(manualFiles()), optionsOf(kModes[0], 8));

    ASSERT_TRUE(good.has_value());

    std::vector<std::string> otherFileMessages;

    for (const ApplicationDiagnostic& diagnostic : good->result().mDiagnostics)
    {
        const bool otherFile = !diagnostic.mSources.empty() &&
                               diagnostic.mSources[0].mSourceIndex != 2 &&
                               diagnostic.mSources[0].mSourceIndex != 5;

        if (diagnostic.mStage == PipelineStage::Parsing && otherFile)
        {
            otherFileMessages.push_back(diagnostic.mMessage);
        }
    }

    ASSERT_FALSE(otherFileMessages.empty());

    std::vector<RunSnapshot> snapshots;

    for (const ParseMode& mode : kModes)
    {
        SCOPED_TRACE(mode.mName);

        std::optional<ApplicationRun> run = startRun(requestOf2024(files), optionsOf(mode, 8));

        ASSERT_TRUE(run.has_value());

        const ApplicationResult& result = run->result();
        const std::vector<const ApplicationDiagnostic*> headers =
            diagnosticsOf(result, DiagnosticCode::InvalidHeader);

        ASSERT_EQ(headers.size(), 2U);

        ASSERT_EQ(headers[0]->mSources.size(), 1U);
        EXPECT_EQ(headers[0]->mSources[0].mSourceIndex, 2U);
        EXPECT_EQ(headers[0]->mMessageKey, MessageKey::InvalidHeaderDuplicateColumn);

        ASSERT_EQ(headers[1]->mSources.size(), 1U);
        EXPECT_EQ(headers[1]->mSources[0].mSourceIndex, 5U);
        EXPECT_EQ(headers[1]->mMessageKey, MessageKey::InvalidHeaderFlexQueryXmlRequired);

        // A file-level error fails every form.
        EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Failed);
        EXPECT_EQ(result.report(ReportType::Dividends).mStatus, ReportStatus::Failed);
        EXPECT_EQ(result.report(ReportType::BankDepositInterest).mStatus, ReportStatus::Failed);
        EXPECT_EQ(result.report(ReportType::OtherInterest).mStatus, ReportStatus::Failed);

        // The six other files are read as before: their parser diagnostics are all there, in
        // the same order.
        std::vector<std::string> messages;

        for (const ApplicationDiagnostic& diagnostic : result.mDiagnostics)
        {
            const bool otherFile = !diagnostic.mSources.empty() &&
                                   diagnostic.mSources[0].mSourceIndex != 2 &&
                                   diagnostic.mSources[0].mSourceIndex != 5;

            if (diagnostic.mStage == PipelineStage::Parsing && otherFile)
            {
                messages.push_back(diagnostic.mMessage);
            }
        }

        EXPECT_EQ(messages, otherFileMessages);

        snapshots.push_back(snapshotOf(result));
    }

    ASSERT_EQ(snapshots.size(), kModes.size());

    for (std::size_t mode = 1; mode < kModes.size(); ++mode)
    {
        SCOPED_TRACE(kModes[mode].mName);

        expectSameSnapshot(snapshots[mode], snapshots[0]);
    }
}

TEST(ParseModeEquivalenceTest, AnswersGivenInOneModeAreUsedInTheOther) {
    const std::string golden = readTestData("pipeline_mixed", "Doh-KDVP_2024.golden.xml");

    // Answered with one worker, then the same files again with four workers; and the other way
    // round.
    for (const auto& [first, second] :
         {std::pair{kModes[0], kModes[2]}, std::pair{kModes[2], kModes[0]}})
    {
        SCOPED_TRACE(std::string{first.mName} + ", then " + std::string{second.mName});

        std::optional<ApplicationRun> run =
            startRun(requestOf2024(mixedFiles()), optionsOf(first, 2));

        ASSERT_TRUE(run.has_value());

        answerTheMixedQuestions(*run);

        ASSERT_FALSE(run->restart(requestOf2024(mixedFiles()), optionsOf(second, 2)).has_value());

        const ApplicationResult& restarted = run->result();

        // The identity answer, the loss answer and both ratio answers are kept from the earlier
        // run and used, so nothing is asked again and the form is the golden one.
        EXPECT_EQ(restarted.mRunRevision, 5U);
        EXPECT_TRUE(restarted.mRequests.empty());

        ASSERT_EQ(restarted.mDecisions.size(), 4U);

        for (const RecordedDecision& decision : restarted.mDecisions)
        {
            EXPECT_TRUE(decision.mFromEarlierRun);
            EXPECT_TRUE(decision.mUsed);
        }

        ASSERT_TRUE(restarted.report(ReportType::CapitalGains).mXml.has_value());
        EXPECT_EQ(*restarted.report(ReportType::CapitalGains).mXml, golden);
    }
}

TEST(ParseModeEquivalenceTest, AFileAddedInARestartRemovesTheLossQuestionInEveryMode) {
    std::vector<RunSnapshot> snapshots;

    for (const ParseMode& mode : kModes)
    {
        SCOPED_TRACE(mode.mName);

        // Started and answered with one worker; the restart adds the Interactive Brokers file
        // of 2025 and parses the three files in the mode of the loop.
        std::optional<ApplicationRun> run =
            startRun(requestOf2024(mixedFiles()), optionsOf(kModes[0], 2));

        ASSERT_TRUE(run.has_value());

        answerTheMixedQuestions(*run);

        std::vector<BrokerInputFile> files = mixedFiles();

        files.push_back(brokerFile(Broker::InteractiveBrokers,
                                   "ibkr_2025.xml",
                                   readTestData("pipeline_mixed", "ibkr_2025.xml")));

        ASSERT_FALSE(run->restart(requestOf2024(std::move(files)), optionsOf(mode, 3)).has_value());

        const ApplicationResult& restarted = run->result();

        // 4 units of XX0000000203 bought on 2025-01-06, 17 days after the loss sale of
        // 2024-12-20, replace all 4 units sold: the loss answer is kept but not used, and the
        // sale is `F10` false. The other two sales stay `F10` true.
        EXPECT_EQ(restarted.mRunRevision, 5U);
        EXPECT_TRUE(restarted.mRequests.empty());

        ASSERT_EQ(restarted.mDecisions.size(), 4U);
        EXPECT_EQ(restarted.mDecisions[1].mType, RequestType::LossEligibilityConfirmationRequired);
        EXPECT_FALSE(restarted.mDecisions[1].mUsed);

        ASSERT_TRUE(restarted.report(ReportType::CapitalGains).mXml.has_value());

        const std::string& xml = *restarted.report(ReportType::CapitalGains).mXml;

        EXPECT_EQ(countOf(xml, "<F10>true</F10>"), 2U);
        EXPECT_EQ(countOf(xml, "<F10>false</F10>"), 1U);

        snapshots.push_back(snapshotOf(restarted));
    }

    ASSERT_EQ(snapshots.size(), kModes.size());

    for (std::size_t mode = 1; mode < kModes.size(); ++mode)
    {
        SCOPED_TRACE(kModes[mode].mName);

        expectSameSnapshot(snapshots[mode], snapshots[0]);
    }
}

TEST(ParseModeEquivalenceTest, AStartOrderThatIsNotAPermutationFailsTheStart) {
    ApplicationRunOptions options{.mParseWorkerLimit = 4, .mParseStartOrder = {0, 0}};

    EXPECT_THROW(static_cast<void>(ApplicationRun::start(requestOf2024(mixedFiles()), options)),
                 std::invalid_argument);
}

} // namespace
