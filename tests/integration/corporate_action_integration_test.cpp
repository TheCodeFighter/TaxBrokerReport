#include "parsers/ibkr_parser.hpp"
#include "parsers/traderepublic_parser.hpp"
#include "processors/capital_gains_report_builder.hpp"
#include "processors/capital_report_gates.hpp"
#include "processors/corporate_actions.hpp"
#include "processors/fifo_matcher.hpp"
#include "processors/isin_change.hpp"
#include "processors/loss_eligibility.hpp"
#include "processors/tax_processor.hpp"
#include "taxbroker/api/application_result_json.hpp"
#include "taxbroker/decision_session.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace taxbroker;

Date day(std::string_view aDate) {
    return *parseCalendarDate(aDate);
}

class TemporaryActionInputs {
  public:
    TemporaryActionInputs() {
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
        mDirectory = std::filesystem::temp_directory_path() /
                     ("taxbroker_action_synthetic_" + std::to_string(token));

        if (!std::filesystem::create_directory(mDirectory))
        {
            throw std::runtime_error{"Unable to create synthetic action directory"};
        }
    }

    TemporaryActionInputs(const TemporaryActionInputs&) = delete;
    TemporaryActionInputs& operator=(const TemporaryActionInputs&) = delete;

    ~TemporaryActionInputs() {
        std::error_code error;
        std::filesystem::remove_all(mDirectory, error);
    }

    /// Writes exactly the synthetic rows supplied by each scenario.
    std::filesystem::path write(std::string_view aName,
                                std::initializer_list<std::string_view> aRows) const {
        const auto path = mDirectory / aName;
        std::ofstream output{path};

        output << "datetime,date,account_type,category,type,asset_class,name,symbol,shares,price,"
                  "amount,fee,tax,currency,original_amount,original_currency,fx_rate,description,"
                  "transaction_id,counterparty_name,counterparty_iban,payment_reference,mcc_code\n";

        for (const auto row : aRows)
        {
            output << row << '\n';
        }

        output.close();

        if (!output)
        {
            throw std::runtime_error{"Unable to write synthetic action CSV"};
        }

        return path;
    }

  private:
    std::filesystem::path mDirectory;
};

/// What one calculation built before the result, for a test that checks the lots or the rows.
struct CapitalStages {
    FifoMatchingResult mFifo;
    CapitalGainsBuildResult mBuilt;
};

/// The capital-gains part of one run, as the CLI runs it, from the parsed files: merge, year,
/// FIFO with the confirmed corporate actions applied, the corporate actions and the loss rule with
/// the session's answers, the report builder and the gates. aStages, when given, gets the FIFO
/// and builder results of this calculation.
ApplicationResultInput calculateCapitalGains(const std::vector<ParseResult>& aParsed,
                                             int aYear,
                                             RunDecisions& aDecisions,
                                             CapitalStages* aStages = nullptr) {
    std::vector<StatementMergeInput> inputs;

    for (std::size_t index = 0; index < aParsed.size(); ++index)
    {
        inputs.push_back({index, aParsed[index]});
    }

    auto prepared = TaxProcessor::prepareReportingYear(
        {aYear, DeterministicStatementMerger{}.merge(std::move(inputs))});
    IsinChangeResult isinChanges =
        applyIsinChanges(prepared.mMerged.mStatement, *prepared.mLedger, aDecisions);
    std::optional<TaxProcessingResult> carriedOut;

    // An answered ISIN change moves the lots to the new ISIN: the ledger is made again from it.
    if (isinChanges.mStatement)
    {
        carriedOut = TaxProcessor::prepareReportingYear(
            {aYear, StatementMergeResult{std::move(*isinChanges.mStatement), {}}});
    }

    const ReportingYearLedger& ledger =
        carriedOut ? carriedOut->mLedger.value() : prepared.mLedger.value();
    const MergedStatement& statement =
        carriedOut ? carriedOut->mMerged.mStatement : prepared.mMerged.mStatement;
    auto pools = matchCapitalPools(statement, ledger, aDecisions);
    auto& fifo = pools.mFifo;
    auto& actions = pools.mActions;
    auto loss = LossEligibilityChecker::check(statement, ledger, fifo, aDecisions);
    const auto built = CapitalGainsReportBuilder::build(statement, ledger, fifo, loss, {});
    auto gates = CapitalReportGates::check(statement, ledger, fifo, loss, built, {});

    if (aStages != nullptr)
    {
        *aStages = CapitalStages{.mFifo = fifo, .mBuilt = built};
    }

    ApplicationResultInput input{
        .mReportingYear = aYear,
        .mMergeDiagnostics = std::move(prepared.mMerged.mDiagnostics),
        .mProcessingDiagnostics = std::move(prepared.mDiagnostics),
    };

    std::move(fifo.mDiagnostics.begin(),
              fifo.mDiagnostics.end(),
              std::back_inserter(input.mProcessingDiagnostics));
    appendStageOutput(input, std::move(isinChanges.mDiagnostics), std::move(isinChanges.mRequests));
    appendStageOutput(input, std::move(actions.mDiagnostics), std::move(actions.mRequests));
    appendStageOutput(input, std::move(loss.mDiagnostics), std::move(loss.mRequests));
    std::move(gates.begin(), gates.end(), std::back_inserter(input.mProcessingDiagnostics));

    return input;
}

/// The question data of the request at aIndex.
const CorporateActionRatioQuestion& ratioCheckAt(const ApplicationResult& aResult,
                                                 std::size_t aIndex) {
    return std::get<CorporateActionRatioQuestion>(aResult.mRequests.at(aIndex).mData);
}

/// The codes of the diagnostics, in result order.
std::vector<ApplicationDiagnosticCode> codesOf(const ApplicationResult& aResult) {
    std::vector<ApplicationDiagnosticCode> codes;

    std::transform(aResult.mDiagnostics.begin(),
                   aResult.mDiagnostics.end(),
                   std::back_inserter(codes),
                   [](const ApplicationDiagnostic& aDiagnostic) { return aDiagnostic.mCode; });

    return codes;
}

TEST(CorporateActionIntegrationTest, ASplitAsksForTheRatio) {
    // 3 units bought, then a split with `shares` 3, then 1 unit sold in the year.
    const TemporaryActionInputs files;
    const auto history = files.write(
        "history.csv",
        {
            R"(2023-02-01T09:00:00Z,2023-02-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,3,10,-30,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-06-01T09:00:00Z,2024-06-01,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,3,,,,,,,,,,synthetic-split-1,,,,)",
            R"(2024-09-01T09:00:00Z,2024-09-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-1,6,6,,,EUR,,,,,synthetic-sell-1,,,,)",
        });
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parseFile(history, 0)};
    DecisionSession session;

    const auto& result = session.start([&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions);
    });

    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::NeedsInput);
    EXPECT_EQ(result.mStatus, OverallStatus::NeedsInput);

    ASSERT_EQ(result.mRequests.size(), 1U);

    const auto& request = result.mRequests.at(0);
    const auto& data = ratioCheckAt(result, 0);

    EXPECT_EQ(request.mType, RequestType::CorporateActionRatioRequired);
    EXPECT_EQ(request.mIsin, "XX0000000001");
    ASSERT_EQ(request.mSources.size(), 1U);
    EXPECT_EQ(request.mSources.at(0).mRow, 3U); // the split row; the header is row 1
    EXPECT_EQ(data.mName, "Synthetic Share");
    EXPECT_EQ(data.mPositionBefore, 300'000'000); // 3 units
    // `shares` is shown as it is; the stage does not read it as a ratio.
    EXPECT_EQ(data.mSources,
              (std::vector<CorporateActionSourceFacts>{{.mBroker = Broker::TradeRepublic,
                                                        .mTransactionId = "synthetic-split-1",
                                                        .mDate = day("2024-06-01"),
                                                        .mSourceValue = "3"}}));

    // Only capital gains waits for the answer.
    EXPECT_EQ(request.mAffectedReports, (std::vector<ReportType>{ReportType::CapitalGains}));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode,
              ApplicationDiagnosticCode{ProcessingDiagnosticCode::CorporateActionAmbiguous});
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey,
              MessageKey::CorporateActionAmbiguousRatioRequired);
    EXPECT_EQ(result.mDiagnostics.at(0).mRequestId, "request-0001");
    EXPECT_EQ(result.mDiagnostics.at(0).mAffectedReports,
              (std::vector<ReportType>{ReportType::CapitalGains}));
}

TEST(CorporateActionIntegrationTest, APurchaseOnTheSplitDayIsNotInThePositionBefore) {
    // The purchase of 1 unit at 09:00 is earlier in the file than the split at 10:00 on the same
    // day, but a split comes before the trades of its day: the position before it is 3 units,
    // not 4, so 2 / 1 previews 6 units, not 8.
    const TemporaryActionInputs files;
    const auto history = files.write(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,3,10,-30,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-06-01T09:00:00Z,2024-06-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1,10,-10,,,EUR,,,,,synthetic-buy-2,,,,)",
            R"(2024-06-01T10:00:00Z,2024-06-01,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,3,,,,,,,,,,synthetic-split-1,,,,)",
            R"(2024-09-01T09:00:00Z,2024-09-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-1,6,6,,,EUR,,,,,synthetic-sell-1,,,,)",
        });
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parseFile(history, 0)};
    DecisionSession session;

    const auto& result = session.start([&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions);
    });

    ASSERT_EQ(result.mRequests.size(), 1U);
    EXPECT_EQ(ratioCheckAt(result, 0).mPositionBefore, 300'000'000); // 3 units
    // From 2024-06-02 the same-day purchase comes before the split: 3 + 1 = 4 units, and from the
    // day after the sale of 2024-09-01, 4 - 1 = 3 units. After a date of the first range come +1
    // and -1 (never below 0), after one of the second range the sale of 1 unit.
    EXPECT_EQ(ratioCheckAt(result, 0).mDateRanges,
              (std::vector<EffectiveDateRange>{{.mEarliestDate = day("2024-01-11"),
                                                .mLatestDate = day("2024-06-01"),
                                                .mPositionBefore = 300'000'000},
                                               {.mEarliestDate = day("2024-06-02"),
                                                .mLatestDate = day("2024-09-01"),
                                                .mPositionBefore = 400'000'000,
                                                .mLowestChangeAfter = -100'000'000},
                                               {.mEarliestDate = day("2024-09-02"),
                                                .mLatestDate = day("2024-12-31"),
                                                .mPositionBefore = 300'000'000}}));

    // The preview of 8 units would include the same-day purchase.
    const auto wrong = session.answer({{"request-0001",
                                        1,
                                        SplitRatioAnswer{.mNewShares = 2,
                                                         .mOldShares = 1,
                                                         .mEffectiveDate = day("2024-06-01"),
                                                         .mConfirmedUnits = 800'000'000}}});

    ASSERT_EQ(wrong.size(), 1U);
    EXPECT_EQ(wrong.at(0).mReason, AnswerRejection::PreviewMismatch);

    // The error names the split row, not the purchase before it.
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    ASSERT_EQ(result.mDiagnostics.at(0).mSources.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mSources.at(0).mRow, 4U); // the header is row 1
}

TEST(CorporateActionIntegrationTest, AConfirmedRatioIsAppliedAndASaleOfChangedUnitsIsListed) {
    // The split is applied: 3 units at EUR 10 become 6 units at EUR 5. The sale of 1 unit uses
    // the changed lot: the purchase row lists 1 unit at EUR 5.
    const TemporaryActionInputs files;
    const auto history = files.write(
        "history.csv",
        {
            R"(2023-02-01T09:00:00Z,2023-02-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,3,10,-30,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-06-01T09:00:00Z,2024-06-01,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,3,,,,,,,,,,synthetic-split-1,,,,)",
            R"(2024-09-01T09:00:00Z,2024-09-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-1,6,6,,,EUR,,,,,synthetic-sell-1,,,,)",
        });
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parseFile(history, 0)};
    DecisionSession session;
    session.start([&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions);
    });

    // 3 units with 2 / 1 are 6 units.
    const auto rejections = session.answer({{"request-0001",
                                             1,
                                             SplitRatioAnswer{.mNewShares = 2,
                                                              .mOldShares = 1,
                                                              .mEffectiveDate = day("2024-06-01"),
                                                              .mConfirmedUnits = 600'000'000}}});

    EXPECT_TRUE(rejections.empty());

    const auto& result = session.result();

    EXPECT_TRUE(result.mRequests.empty());

    // No error: the sale is listed. This helper does not write the XML, so the form has no data;
    // the CLI tests check the rows.
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::NoData);

    // The applied warning is the only diagnostic.
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& applied = result.mDiagnostics.at(0);

    EXPECT_EQ(applied.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(applied.mCode,
              ApplicationDiagnosticCode{ProcessingDiagnosticCode::CorporateActionApplied});
    EXPECT_EQ(applied.mMessageKey, MessageKey::CorporateActionAppliedUserRatio);
    EXPECT_EQ(applied.mMessageParams,
              (std::vector<MessageParam>{numberParam("newShares", 2),
                                         numberParam("oldShares", 1),
                                         textParam("effectiveDate", "2024-06-01")}));
    EXPECT_FALSE(applied.mRequestId.has_value());

    ASSERT_EQ(result.mDecisions.size(), 1U);
    EXPECT_EQ(result.mDecisions.at(0).mAnswer,
              (AnswerValue{SplitRatioAnswer{.mNewShares = 2,
                                            .mOldShares = 1,
                                            .mEffectiveDate = day("2024-06-01"),
                                            .mConfirmedUnits = 600'000'000}}));
    EXPECT_TRUE(result.mDecisions.at(0).mUsed);
}

TEST(CorporateActionIntegrationTest, AReverseSplitRatioIsRecorded) {
    // 6 units, then a one-for-three reverse split: 2 units.
    const TemporaryActionInputs files;
    const auto history = files.write(
        "history.csv",
        {
            R"(2023-02-01T09:00:00Z,2023-02-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,6,10,-60,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-06-01T09:00:00Z,2024-06-01,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,-4,,,,,,,,,,synthetic-split-1,,,,)",
            R"(2024-09-01T09:00:00Z,2024-09-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-1,30,30,,,EUR,,,,,synthetic-sell-1,,,,)",
        });
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parseFile(history, 0)};
    DecisionSession session;
    session.start([&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions);
    });

    ASSERT_EQ(session.result().mRequests.size(), 1U);
    EXPECT_EQ(ratioCheckAt(session.result(), 0).mPositionBefore, 600'000'000); // 6 units
    EXPECT_EQ(ratioCheckAt(session.result(), 0).mSources.at(0).mSourceValue, "-4");

    const auto rejections = session.answer({{"request-0001",
                                             1,
                                             SplitRatioAnswer{.mNewShares = 1,
                                                              .mOldShares = 3,
                                                              .mEffectiveDate = day("2024-06-01"),
                                                              .mConfirmedUnits = 200'000'000}}});

    EXPECT_TRUE(rejections.empty());
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions.at(0).mAnswer,
              (AnswerValue{SplitRatioAnswer{.mNewShares = 1,
                                            .mOldShares = 3,
                                            .mEffectiveDate = day("2024-06-01"),
                                            .mConfirmedUnits = 200'000'000}}));
}

TEST(CorporateActionIntegrationTest, TheSourceValueIsNeverUsedAsTheRatio) {
    // A ten-for-one split: 0.52 units before it, and a `shares` value of 4.68, 9 times the
    // position (the added units, if that is what it means). 10 / 1 gives 5.20 units, 9 / 1 gives
    // 4.68 units. The backend cannot know which the announcement says: the preview is the
    // user's check, so it accepts either, and keeps what the user confirmed.
    const TemporaryActionInputs files;
    const auto history = files.write(
        "history.csv",
        {
            R"(2025-03-01T09:00:00Z,2025-03-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,0.52,100,-52,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2025-11-14T09:00:00Z,2025-11-14,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,4.68,,,,,,,,,,synthetic-split-1,,,,)",
            R"(2025-12-01T09:00:00Z,2025-12-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-0.1,12,1.2,,,EUR,,,,,synthetic-sell-1,,,,)",
        });
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parseFile(history, 0)};
    const auto calculation = [&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2025, aDecisions);
    };

    DecisionSession tenForOne;
    tenForOne.start(calculation);

    ASSERT_EQ(tenForOne.result().mRequests.size(), 1U);
    EXPECT_EQ(ratioCheckAt(tenForOne.result(), 0).mPositionBefore, 52'000'000); // 0.52 units
    EXPECT_EQ(ratioCheckAt(tenForOne.result(), 0).mSources.at(0).mSourceValue, "4.68");

    const auto ten = tenForOne.answer({{"request-0001",
                                        1,
                                        SplitRatioAnswer{.mNewShares = 10,
                                                         .mOldShares = 1,
                                                         .mEffectiveDate = day("2025-11-14"),
                                                         .mConfirmedUnits = 520'000'000}}});

    EXPECT_TRUE(ten.empty());
    EXPECT_EQ(tenForOne.result().mDecisions.at(0).mAnswer,
              (AnswerValue{SplitRatioAnswer{.mNewShares = 10,
                                            .mOldShares = 1,
                                            .mEffectiveDate = day("2025-11-14"),
                                            .mConfirmedUnits = 520'000'000}}));

    DecisionSession nineForOne;
    nineForOne.start(calculation);

    const auto nine = nineForOne.answer({{"request-0001",
                                          1,
                                          SplitRatioAnswer{.mNewShares = 9,
                                                           .mOldShares = 1,
                                                           .mEffectiveDate = day("2025-11-14"),
                                                           .mConfirmedUnits = 468'000'000}}});

    EXPECT_TRUE(nine.empty());
    EXPECT_EQ(nineForOne.result().mDecisions.at(0).mAnswer,
              (AnswerValue{SplitRatioAnswer{.mNewShares = 9,
                                            .mOldShares = 1,
                                            .mEffectiveDate = day("2025-11-14"),
                                            .mConfirmedUnits = 468'000'000}}));
}

TEST(CorporateActionIntegrationTest, CancellingTheRatioQuestionFailsTheReport) {
    const TemporaryActionInputs files;
    const auto history = files.write(
        "history.csv",
        {
            R"(2023-02-01T09:00:00Z,2023-02-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,3,10,-30,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-06-01T09:00:00Z,2024-06-01,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,3,,,,,,,,,,synthetic-split-1,,,,)",
            R"(2024-09-01T09:00:00Z,2024-09-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-1,6,6,,,EUR,,,,,synthetic-sell-1,,,,)",
        });
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parseFile(history, 0)};
    DecisionSession session;
    session.start([&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions);
    });

    const auto rejections = session.answer({{"request-0001", 1, CancelAnswer{}}});

    EXPECT_TRUE(rejections.empty());

    const auto& result = session.result();

    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Failed);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode,
              ApplicationDiagnosticCode{ProcessingDiagnosticCode::CorporateActionAmbiguous});
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey, MessageKey::CorporateActionAmbiguousCancelled);

    ASSERT_EQ(result.mDecisions.size(), 1U);
    EXPECT_EQ(result.mDecisions.at(0).mAnswer, (AnswerValue{CancelAnswer{}}));
}

TEST(CorporateActionIntegrationTest, TheSameRowInTwoFilesIsOneQuestion) {
    // Two overlapping exports both have the split row. The merger keeps one event with both
    // sources, so one question with one source record, and one confirmed action.
    const TemporaryActionInputs files;
    const auto first = files.write(
        "first.csv",
        {
            R"(2023-02-01T09:00:00Z,2023-02-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,3,10,-30,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-06-01T09:00:00Z,2024-06-01,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,3,,,,,,,,,,synthetic-split-1,,,,)",
        });
    const auto second = files.write(
        "second.csv",
        {
            R"(2024-06-01T09:00:00Z,2024-06-01,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,3,,,,,,,,,,synthetic-split-1,,,,)",
            R"(2024-09-01T09:00:00Z,2024-09-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-1,6,6,,,EUR,,,,,synthetic-sell-1,,,,)",
        });
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parseFile(first, 0),
                                          tr::TradeRepublicParser{}.parseFile(second, 1)};
    DecisionSession session;
    session.start([&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions);
    });

    ASSERT_EQ(session.result().mRequests.size(), 1U);

    const auto& request = session.result().mRequests.at(0);

    // Both rows are shown; the question has one source record.
    ASSERT_EQ(request.mSources.size(), 2U);
    EXPECT_EQ(request.mSources.at(0).mSourceIndex, 0U);
    EXPECT_EQ(request.mSources.at(0).mRow, 3U);
    EXPECT_EQ(request.mSources.at(1).mSourceIndex, 1U);
    EXPECT_EQ(request.mSources.at(1).mRow, 2U);
    EXPECT_EQ(ratioCheckAt(session.result(), 0).mSources.size(), 1U);

    const auto rejections = session.answer({{"request-0001",
                                             1,
                                             SplitRatioAnswer{.mNewShares = 2,
                                                              .mOldShares = 1,
                                                              .mEffectiveDate = day("2024-06-01"),
                                                              .mConfirmedUnits = 600'000'000}}});

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(session.result().mRequests.empty());
    // One action applied once (one warning), the warning that the rows of 2024 come from two
    // files, and the report is written.
    EXPECT_EQ(codesOf(session.result()),
              (std::vector<ApplicationDiagnosticCode>{
                  ProcessingDiagnosticCode::CorporateActionApplied,
                  ProcessingDiagnosticCode::CorporateActionSourcesMismatchPossible}));
    EXPECT_EQ(session.result().report(ReportType::CapitalGains).mStatus, ReportStatus::NoData);
}

TEST(CorporateActionIntegrationTest, TwoSplitsAreAskedOneAtATime) {
    // A 2 / 1 split in March and another split in September. Only March is asked; September
    // waits, linked to the March question, so capital gains needs input rather than failing.
    const TemporaryActionInputs files;
    const auto history = files.write(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,3,10,-30,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,3,,,,,,,,,,synthetic-split-1,,,,)",
            R"(2024-06-01T09:00:00Z,2024-06-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1,5,-5,,,EUR,,,,,synthetic-buy-2,,,,)",
            R"(2024-09-01T09:00:00Z,2024-09-01,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,14,,,,,,,,,,synthetic-split-2,,,,)",
            R"(2024-10-01T09:00:00Z,2024-10-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-1,2,2,,,EUR,,,,,synthetic-sell-1,,,,)",
        });
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parseFile(history, 0)};
    DecisionSession session;
    session.start([&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions);
    });

    const auto& asked = session.result();

    EXPECT_EQ(asked.report(ReportType::CapitalGains).mStatus, ReportStatus::NeedsInput);
    ASSERT_EQ(asked.mRequests.size(), 1U);
    EXPECT_EQ(ratioCheckAt(asked, 0).mEffectiveDate,
              Date{std::chrono::sys_days{std::chrono::year{2024} / 3 / 1}.time_since_epoch()});
    EXPECT_EQ(ratioCheckAt(asked, 0).mPositionBefore, 300'000'000); // 3 units

    ASSERT_EQ(asked.mDiagnostics.size(), 2U);
    EXPECT_EQ(asked.mDiagnostics.at(0).mMessageKey,
              MessageKey::CorporateActionAmbiguousRatioRequired);
    EXPECT_EQ(asked.mDiagnostics.at(0).mRequestId, "request-0001");
    EXPECT_EQ(asked.mDiagnostics.at(1).mMessageKey,
              MessageKey::CorporateActionNotAppliedWaitingForEarlierAction);
    EXPECT_EQ(asked.mDiagnostics.at(1).mMessageParams,
              (std::vector<MessageParam>{textParam("earlierDate", "2024-03-01")}));
    EXPECT_EQ(asked.mDiagnostics.at(1).mRequestId, "request-0001");

    // The JSON of the result names the code of the waiting action.
    EXPECT_NE(api::serializeApplicationResultJson(asked).find("\"corporate_action_not_applied\""),
              std::string::npos);

    const auto rejections = session.answer({{"request-0001",
                                             1,
                                             SplitRatioAnswer{.mNewShares = 2,
                                                              .mOldShares = 1,
                                                              .mEffectiveDate = day("2024-03-01"),
                                                              .mConfirmedUnits = 600'000'000}}});

    EXPECT_TRUE(rejections.empty());

    // March is applied: 3 units x 2 / 1 = 6, and the purchase of 1 unit on 2024-06-01 comes
    // after it, so September is asked with 7 units in the same run.
    const auto& answered = session.result();

    EXPECT_EQ(answered.report(ReportType::CapitalGains).mStatus, ReportStatus::NeedsInput);
    ASSERT_EQ(answered.mRequests.size(), 1U);
    EXPECT_EQ(ratioCheckAt(answered, 0).mEffectiveDate, day("2024-09-01"));
    EXPECT_EQ(ratioCheckAt(answered, 0).mPositionBefore, 700'000'000); // 6 + 1 units

    // The March row comes first in the file, so its warning comes first.
    ASSERT_EQ(answered.mDiagnostics.size(), 2U);
    EXPECT_EQ(answered.mDiagnostics.at(0).mMessageKey, MessageKey::CorporateActionAppliedUserRatio);
    EXPECT_EQ(answered.mDiagnostics.at(1).mMessageKey,
              MessageKey::CorporateActionAmbiguousRatioRequired);
    EXPECT_EQ(answered.mDiagnostics.at(1).mRequestId, "request-0001");

    // 7 units with 3 / 1 are 21 units.
    const auto second = session.answer({{"request-0001",
                                         2,
                                         SplitRatioAnswer{.mNewShares = 3,
                                                          .mOldShares = 1,
                                                          .mEffectiveDate = day("2024-09-01"),
                                                          .mConfirmedUnits = 2'100'000'000}}});

    EXPECT_TRUE(second.empty());

    // Both are applied; the sale of 2024-10-01 uses a changed lot and gives no error (this helper
    // writes no XML, so NoData).
    const auto& applied = session.result();

    EXPECT_TRUE(applied.mRequests.empty());
    EXPECT_EQ(applied.report(ReportType::CapitalGains).mStatus, ReportStatus::NoData);
    EXPECT_EQ(applied.mDecisions.size(), 2U);
    EXPECT_EQ(
        codesOf(applied),
        (std::vector<ApplicationDiagnosticCode>{ProcessingDiagnosticCode::CorporateActionApplied,
                                                ProcessingDiagnosticCode::CorporateActionApplied}));
}

TEST(CorporateActionIntegrationTest, TwoSplitRowsOnOneDayAskWhichRowsAreOneAction) {
    const TemporaryActionInputs files;
    const auto history = files.write(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,3,10,-30,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-06-01T09:00:00Z,2024-06-01,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,3,,,,,,,,,,synthetic-split-1,,,,)",
            R"(2024-06-01T09:00:00Z,2024-06-01,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,3,,,,,,,,,,synthetic-split-2,,,,)",
            R"(2024-09-01T09:00:00Z,2024-09-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-1,6,6,,,EUR,,,,,synthetic-sell-1,,,,)",
        });
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parseFile(history, 0)};
    DecisionSession session;

    const auto& result = session.start([&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions);
    });

    // Two rows of one broker on one day with no broker action ID: one identity question.
    ASSERT_EQ(result.mRequests.size(), 1U);
    EXPECT_EQ(result.mRequests.at(0).mType, RequestType::CorporateActionIdentityRequired);
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::NeedsInput);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey,
              MessageKey::CorporateActionAmbiguousIdentityRequired);
    EXPECT_EQ(result.mDiagnostics.at(0).mSources.size(), 2U);
    EXPECT_EQ(result.mDiagnostics.at(0).mRequestId, "request-0001");
}

TEST(CorporateActionIntegrationTest, RowsOfTwoBrokersAreGroupedByTheUserAndAskedOneActionAtATime) {
    // Trade Republic books a split row on 2024-11-10; Interactive Brokers books two rows with
    // their own action IDs on 2024-11-14 and 2024-11-20. The three rows are within 10 days, so the
    // files cannot say which are one action: the identity question asks.
    const TemporaryActionInputs files;
    const auto trHistory = files.write(
        "history.csv",
        {
            R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-11-10T09:00:00Z,2024-11-10,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,10,,,,,,,,,,synthetic-split-1,,,,)",
            R"(2024-12-02T09:00:00Z,2024-12-02,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-1,6,6,,,EUR,,,,,synthetic-sell-1,,,,)",
        });
    constexpr std::string_view ibkrXml = R"xml(<FlexQueryResponse queryName="synthetic" type="AF">
  <FlexStatements count="1">
    <FlexStatement accountId="U0000000">
      <AccountInformation accountId="U0000000" ibEntity="IB-IE"/>
      <CorporateActions>
        <CorporateAction accountId="U0000000" transactionID="synthetic-ibkr-split-1" actionID="S1" type="FS" assetCategory="STK" description="Synthetic Share" isin="XX0000000001" currency="EUR" dateTime="20241114;202500" reportDate="20241114" quantity="10" proceeds="0" value="0"/>
        <CorporateAction accountId="U0000000" transactionID="synthetic-ibkr-split-2" actionID="S2" type="FS" assetCategory="STK" description="Synthetic Share" isin="XX0000000001" currency="EUR" dateTime="20241120;202500" reportDate="20241120" quantity="20" proceeds="0" value="0"/>
      </CorporateActions>
    </FlexStatement>
  </FlexStatements>
</FlexQueryResponse>)xml";
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parseFile(trHistory, 0),
                                          ibkr::IbkrParser{}.parse(ibkrXml, "ibkr.xml", 1)};
    DecisionSession session;
    const auto& asked = session.start([&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions);
    });

    // One identity question with the three rows in date order, and no ratio question yet.
    ASSERT_EQ(asked.mRequests.size(), 1U);
    EXPECT_EQ(asked.mRequests.at(0).mType, RequestType::CorporateActionIdentityRequired);

    const auto& rows = std::get<CorporateActionIdentityQuestion>(asked.mRequests.at(0).mData).mRows;

    ASSERT_EQ(rows.size(), 3U);
    EXPECT_EQ(rows.at(0).mBroker, Broker::TradeRepublic);
    EXPECT_EQ(rows.at(0).mDate, day("2024-11-10"));
    EXPECT_EQ(rows.at(1).mBroker, Broker::InteractiveBrokers);
    EXPECT_EQ(rows.at(1).mDate, day("2024-11-14"));
    EXPECT_EQ(rows.at(1).mBrokerActionId, "S1");
    EXPECT_EQ(rows.at(2).mBroker, Broker::InteractiveBrokers);
    EXPECT_EQ(rows.at(2).mDate, day("2024-11-20"));
    EXPECT_EQ(rows.at(2).mBrokerActionId, "S2");

    // Rows 1 and 2 are one split booked by two brokers; row 3 is another split.
    const auto grouped = session.answer(
        {{"request-0001", 1, CorporateActionGroupingAnswer{.mActionOfRow = {1, 1, 2}}}});

    EXPECT_TRUE(grouped.empty());

    // The first action has the rows of both brokers and is asked as one split, not as a question
    // whether the rows are one split. It holds 10 units just before 2024-11-10.
    ASSERT_EQ(session.result().mRequests.size(), 1U);
    EXPECT_EQ(session.result().mRequests.at(0).mType, RequestType::CorporateActionRatioRequired);
    EXPECT_EQ(ratioCheckAt(session.result(), 0).mEffectiveDate, day("2024-11-10"));
    EXPECT_FALSE(ratioCheckAt(session.result(), 0).mAskSameEvent);
    EXPECT_EQ(ratioCheckAt(session.result(), 0).mPositionBefore, 1'000'000'000); // 10 units
    ASSERT_EQ(ratioCheckAt(session.result(), 0).mSources.size(), 2U);
    EXPECT_EQ(ratioCheckAt(session.result(), 0).mSources.at(0).mBroker, Broker::TradeRepublic);
    EXPECT_EQ(ratioCheckAt(session.result(), 0).mSources.at(1).mBroker, Broker::InteractiveBrokers);

    // 10 units with 2 / 1 are 20 units (the confirmed units are those after the action); the
    // second action is asked with them.
    const auto first = session.answer({{"request-0001",
                                        2,
                                        SplitRatioAnswer{.mNewShares = 2,
                                                         .mOldShares = 1,
                                                         .mEffectiveDate = day("2024-11-10"),
                                                         .mConfirmedUnits = 2'000'000'000}}});

    EXPECT_TRUE(first.empty());
    ASSERT_EQ(session.result().mRequests.size(), 1U);
    EXPECT_EQ(ratioCheckAt(session.result(), 0).mEffectiveDate, day("2024-11-20"));
    EXPECT_EQ(ratioCheckAt(session.result(), 0).mPositionBefore, 2'000'000'000); // 20 units
    ASSERT_EQ(ratioCheckAt(session.result(), 0).mSources.size(), 1U);
    EXPECT_EQ(ratioCheckAt(session.result(), 0).mSources.at(0).mBroker, Broker::InteractiveBrokers);

    const auto second = session.answer({{"request-0001",
                                         3,
                                         SplitRatioAnswer{.mNewShares = 2,
                                                          .mOldShares = 1,
                                                          .mEffectiveDate = day("2024-11-20"),
                                                          .mConfirmedUnits = 4'000'000'000}}});

    // Both actions are applied once (10 x 2 x 2 = 40 units), and the rows of 2024 from two files
    // are warned about. Diagnostics follow their first source: the first action and the warning
    // start at the Trade Republic row, the second action at the Interactive Brokers file.
    EXPECT_TRUE(second.empty());
    EXPECT_TRUE(session.result().mRequests.empty());
    EXPECT_EQ(session.result().mDecisions.size(), 3U);
    EXPECT_EQ(codesOf(session.result()),
              (std::vector<ApplicationDiagnosticCode>{
                  ProcessingDiagnosticCode::CorporateActionApplied,
                  ProcessingDiagnosticCode::CorporateActionSourcesMismatchPossible,
                  ProcessingDiagnosticCode::CorporateActionApplied}));
}

TEST(CorporateActionIntegrationTest, AnAddedPurchaseAsksANewQuestion) {
    // The answer was for 3 units before the split. A new run with one more purchase before it
    // has 4 units: another question, and the old answer stays listed, not used.
    const TemporaryActionInputs files;
    const auto history = files.write(
        "history.csv",
        {
            R"(2023-02-01T09:00:00Z,2023-02-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,3,10,-30,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-06-01T09:00:00Z,2024-06-01,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,3,,,,,,,,,,synthetic-split-1,,,,)",
            R"(2024-09-01T09:00:00Z,2024-09-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-1,6,6,,,EUR,,,,,synthetic-sell-1,,,,)",
        });
    const auto added = files.write(
        "added.csv",
        {
            R"(2023-03-01T09:00:00Z,2023-03-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1,10,-10,,,EUR,,,,,synthetic-buy-2,,,,)",
        });
    const std::vector<ParseResult> before{tr::TradeRepublicParser{}.parseFile(history, 0)};
    const std::vector<ParseResult> after{tr::TradeRepublicParser{}.parseFile(history, 0),
                                         tr::TradeRepublicParser{}.parseFile(added, 1)};
    DecisionSession session;
    session.start([&before](RunDecisions& aDecisions) {
        return calculateCapitalGains(before, 2024, aDecisions);
    });

    const auto rejections = session.answer({{"request-0001",
                                             1,
                                             SplitRatioAnswer{.mNewShares = 2,
                                                              .mOldShares = 1,
                                                              .mEffectiveDate = day("2024-06-01"),
                                                              .mConfirmedUnits = 600'000'000}}});

    EXPECT_TRUE(rejections.empty());

    const auto& result = session.start([&after](RunDecisions& aDecisions) {
        return calculateCapitalGains(after, 2024, aDecisions);
    });

    ASSERT_EQ(result.mRequests.size(), 1U);
    EXPECT_EQ(ratioCheckAt(result, 0).mPositionBefore, 400'000'000); // 3 + 1 units
    ASSERT_EQ(result.mDecisions.size(), 1U);
    EXPECT_TRUE(result.mDecisions.at(0).mFromEarlierRun);
    EXPECT_FALSE(result.mDecisions.at(0).mUsed);
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::NeedsInput);
}

TEST(CorporateActionIntegrationTest, NoUnitsBeforeTheSplitIsAMissingPosition) {
    const TemporaryActionInputs files;
    const auto history = files.write(
        "history.csv",
        {
            R"(2023-01-10T09:00:00Z,2023-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2023-03-01T09:00:00Z,2023-03-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-2,20,40,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2023-06-01T09:00:00Z,2023-06-01,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,1,,,,,,,,,,synthetic-split-1,,,,)",
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1,10,-10,,,EUR,,,,,synthetic-buy-2,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-1,20,20,,,EUR,,,,,synthetic-sell-2,,,,)",
        });
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parseFile(history, 0)};
    DecisionSession session;

    const auto& result = session.start([&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions);
    });

    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Failed);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode,
              ApplicationDiagnosticCode{ProcessingDiagnosticCode::CorporateActionMissingPosition});
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey, MessageKey::CorporateActionMissingPosition);
}

TEST(CorporateActionIntegrationTest, AnAnnouncementDateBeforeAPurchaseTakesTheUnitsOfThatDate) {
    // The announcement says the split took effect on 2025-11-10; the broker books the row on
    // 2025-11-14. The 0.1 units bought on 2025-11-12 lie between: with the row date they count
    // before the split (0.62 units), with 2025-11-10 after it (0.52 units). The user's date is
    // the one used, so its preview is 0.52 x 10 = 5.2 units.
    const TemporaryActionInputs files;
    const auto history = files.write(
        "history.csv",
        {
            R"(2025-03-01T09:00:00Z,2025-03-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,0.52,100,-52,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2025-11-12T09:00:00Z,2025-11-12,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,0.1,100,-10,,,EUR,,,,,synthetic-buy-2,,,,)",
            R"(2025-11-14T09:00:00Z,2025-11-14,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,5.58,,,,,,,,,,synthetic-split-1,,,,)",
            R"(2025-12-01T09:00:00Z,2025-12-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-0.1,12,1.2,,,EUR,,,,,synthetic-sell-1,,,,)",
        });
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parseFile(history, 0)};
    DecisionSession session;
    session.start([&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2025, aDecisions);
    });

    ASSERT_EQ(session.result().mRequests.size(), 1U);
    EXPECT_EQ(ratioCheckAt(session.result(), 0).mPositionBefore, 62'000'000); // 0.52 + 0.1
    // 0.52 units up to the purchase, 0.62 up to the sale of 0.1 units, then 0.52 again. Only the
    // second range has a later trade that goes below 0: the sale of 0.1 units.
    EXPECT_EQ(ratioCheckAt(session.result(), 0).mDateRanges,
              (std::vector<EffectiveDateRange>{{.mEarliestDate = day("2025-03-02"),
                                                .mLatestDate = day("2025-11-12"),
                                                .mPositionBefore = 52'000'000},
                                               {.mEarliestDate = day("2025-11-13"),
                                                .mLatestDate = day("2025-12-01"),
                                                .mPositionBefore = 62'000'000,
                                                .mLowestChangeAfter = -10'000'000},
                                               {.mEarliestDate = day("2025-12-02"),
                                                .mLatestDate = day("2025-12-31"),
                                                .mPositionBefore = 52'000'000}}));

    // The row date's preview, 0.62 x 10 = 6.2 units, does not fit 2025-11-10.
    const auto wrong = session.answer({{"request-0001",
                                        1,
                                        SplitRatioAnswer{.mNewShares = 10,
                                                         .mOldShares = 1,
                                                         .mEffectiveDate = day("2025-11-10"),
                                                         .mConfirmedUnits = 620'000'000}}});

    ASSERT_EQ(wrong.size(), 1U);
    EXPECT_EQ(wrong.at(0).mReason, AnswerRejection::PreviewMismatch);
    EXPECT_TRUE(session.result().mDecisions.empty());

    const auto rejections = session.answer({{"request-0001",
                                             1,
                                             SplitRatioAnswer{.mNewShares = 10,
                                                              .mOldShares = 1,
                                                              .mEffectiveDate = day("2025-11-10"),
                                                              .mConfirmedUnits = 520'000'000}}});

    EXPECT_TRUE(rejections.empty());
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    // The sale of 0.1 units on 2025-12-01 takes them from the changed lot of 2025-03-01 (5.2
    // units) and gives no error (NoData: the helper writes no XML).
    EXPECT_EQ(
        codesOf(session.result()),
        (std::vector<ApplicationDiagnosticCode>{ProcessingDiagnosticCode::CorporateActionApplied}));
    EXPECT_EQ(session.result().report(ReportType::CapitalGains).mStatus, ReportStatus::NoData);
    EXPECT_EQ(session.result().mDiagnostics.at(0).mMessageParams,
              (std::vector<MessageParam>{numberParam("newShares", 10),
                                         numberParam("oldShares", 1),
                                         textParam("effectiveDate", "2025-11-10")}));
}

TEST(CorporateActionIntegrationTest, AnAnnouncementDateWithoutTradesBetweenIsAccepted) {
    // The same split without the purchase of 2025-11-12: no trade lies between 2025-11-10 and
    // the row date, so the position of 0.52 units is the same and the date is kept.
    const TemporaryActionInputs files;
    const auto history = files.write(
        "history.csv",
        {
            R"(2025-03-01T09:00:00Z,2025-03-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,0.52,100,-52,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2025-11-14T09:00:00Z,2025-11-14,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,4.68,,,,,,,,,,synthetic-split-1,,,,)",
            R"(2025-12-01T09:00:00Z,2025-12-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-0.1,12,1.2,,,EUR,,,,,synthetic-sell-1,,,,)",
        });
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parseFile(history, 0)};
    DecisionSession session;
    session.start([&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2025, aDecisions);
    });

    ASSERT_EQ(session.result().mRequests.size(), 1U);
    EXPECT_EQ(ratioCheckAt(session.result(), 0).mDateRanges.front().mEarliestDate,
              day("2025-03-02"));

    // 0.52 units with 10 / 1 are 5.2 units.
    const auto rejections = session.answer({{"request-0001",
                                             1,
                                             SplitRatioAnswer{.mNewShares = 10,
                                                              .mOldShares = 1,
                                                              .mEffectiveDate = day("2025-11-10"),
                                                              .mConfirmedUnits = 520'000'000}}});

    EXPECT_TRUE(rejections.empty());
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions.at(0).mAnswer,
              (AnswerValue{SplitRatioAnswer{.mNewShares = 10,
                                            .mOldShares = 1,
                                            .mEffectiveDate = day("2025-11-10"),
                                            .mConfirmedUnits = 520'000'000}}));
}

TEST(CorporateActionIntegrationTest, ASplitAfterTheSaleKeepsTheSaleRowsAndDoublesTheClosingLots) {
    // 10 units bought at EUR 10, 4 sold at EUR 12 in June, a split row in November. With 2 / 1
    // the June sale is listed as it is, and the 6 units left become 12 at 31 December.
    const TemporaryActionInputs files;
    const auto history = files.write(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-06-03T09:00:00Z,2024-06-03,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-4,12,48,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2024-11-15T09:00:00Z,2024-11-15,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,6,,,,,,,,,,synthetic-split-1,,,,)",
        });
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parseFile(history, 0)};
    CapitalStages stages;
    DecisionSession session;

    session.start([&parsed, &stages](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions, &stages);
    });

    // 6 units before the split, 6 x 2 / 1 = 12 after it.
    const auto rejections = session.answer({{"request-0001",
                                             1,
                                             SplitRatioAnswer{.mNewShares = 2,
                                                              .mOldShares = 1,
                                                              .mEffectiveDate = day("2024-11-15"),
                                                              .mConfirmedUnits = 1'200'000'000}}});

    EXPECT_TRUE(rejections.empty());

    // Only the warning that the user gave the ratio; no error blocks the report.
    EXPECT_TRUE(session.result().mRequests.empty());
    EXPECT_EQ(
        codesOf(session.result()),
        (std::vector<ApplicationDiagnosticCode>{ProcessingDiagnosticCode::CorporateActionApplied}));

    ASSERT_TRUE(stages.mBuilt.mReport.has_value());
    ASSERT_EQ(stages.mBuilt.mReport->mLists.size(), 1U);

    const auto& rows = stages.mBuilt.mReport->mLists.at(0).mRows;

    // One purchase row with the 4 units the sale uses, and the sale row, both in units before
    // the split.
    ASSERT_EQ(rows.size(), 2U);

    const auto& purchase = std::get<KdvpPurchaseRow>(rows.at(0).mEntry);
    const auto& sale = std::get<KdvpSaleRow>(rows.at(1).mEntry);

    EXPECT_EQ(purchase.mDate, day("2024-01-10"));
    EXPECT_EQ(purchase.mUnits, 400'000'000);       // 4 units
    EXPECT_EQ(purchase.mUnitPrice, 1'000'000'000); // EUR 10
    EXPECT_EQ(rows.at(0).mStock, 400'000'000);
    EXPECT_EQ(sale.mDate, day("2024-06-03"));
    EXPECT_EQ(sale.mUnits, 400'000'000);       // 4 units
    EXPECT_EQ(sale.mUnitPrice, 1'200'000'000); // EUR 12
    EXPECT_EQ(rows.at(1).mStock, 0);

    // The lot at 31 December: 10 - 4 = 6 units, doubled to 12, with the EUR 60 purchase value
    // of the 6 units unchanged.
    ASSERT_EQ(stages.mFifo.mIsins.size(), 1U);

    const auto& isin = stages.mFifo.mIsins.at(0);

    EXPECT_EQ(isin.mStatus, FifoIsinStatus::Matched);
    ASSERT_EQ(isin.mOpenLots.size(), 1U);
    EXPECT_EQ(isin.mOpenLots.at(0).mPurchaseDate, day("2024-01-10"));
    EXPECT_EQ(isin.mOpenLots.at(0).mState.mRemainingUnits, 1'200'000'000); // 12 units
    EXPECT_EQ(basisInEur(isin.mOpenLots.at(0).mState.mRemainingBasis),
              ExactRational::create(60, 1)); // EUR 60
}

TEST(CorporateActionIntegrationTest, AnEarlyDateWithTooFewUnitsForALaterSaleIsRejected) {
    // 10 units bought, 5 sold on 2024-05-01, a split row on 2024-06-01. 1 / 10 from 2024-03-01
    // would leave 1 unit for the sale of 5, so the answer is rejected and the question stays open.
    const TemporaryActionInputs files;
    const auto history = files.write(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-05-01T09:00:00Z,2024-05-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-5,12,60,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2024-06-01T09:00:00Z,2024-06-01,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,1,,,,,,,,,,synthetic-split-1,,,,)",
        });
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parseFile(history, 0)};
    DecisionSession session;

    session.start([&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions);
    });

    const auto rejections = session.answer({{"request-0001",
                                             1,
                                             SplitRatioAnswer{.mNewShares = 1,
                                                              .mOldShares = 10,
                                                              .mEffectiveDate = day("2024-03-01"),
                                                              .mConfirmedUnits = 100'000'000}}});

    ASSERT_EQ(rejections.size(), 1U);
    EXPECT_EQ(rejections.at(0).mReason, AnswerRejection::LaterSaleTooLarge);
    EXPECT_TRUE(session.result().mDecisions.empty());
    EXPECT_EQ(session.result().report(ReportType::CapitalGains).mStatus, ReportStatus::NeedsInput);
}

TEST(CorporateActionIntegrationTest, ALossSaleBeforeASplitIsComparedWithARebuyAfterIt) {
    // 12 bought at EUR 10, 10 sold at EUR 8 on 2024-05-02 (a loss), a split row on 2024-05-10,
    // 6 bought at EUR 4 on 2024-05-20. With 2 / 1 the 6 new units are 3 units of the sale, so 3
    // of its 10 loss units are blocked by the files and 7 are asked. yes (bought elsewhere) blocks
    // them too: the sale is written with F10 false.
    const TemporaryActionInputs files;
    const auto history = files.write(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,12,10,-120,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-05-02T09:00:00Z,2024-05-02,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,8,80,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2024-05-10T09:00:00Z,2024-05-10,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,2,,,,,,,,,,synthetic-split-1,,,,)",
            R"(2024-05-20T09:00:00Z,2024-05-20,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,6,4,-24,,,EUR,,,,,synthetic-buy-2,,,,)",
        });
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parseFile(history, 0)};
    CapitalStages stages;
    DecisionSession session;

    session.start([&parsed, &stages](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions, &stages);
    });

    // 2 units before the split, 2 x 2 / 1 = 4 after it.
    const auto ratio = session.answer({{"request-0001",
                                        1,
                                        SplitRatioAnswer{.mNewShares = 2,
                                                         .mOldShares = 1,
                                                         .mEffectiveDate = day("2024-05-10"),
                                                         .mConfirmedUnits = 400'000'000}}});

    EXPECT_TRUE(ratio.empty());
    ASSERT_EQ(session.result().mRequests.size(), 1U);

    const auto& lossCheck =
        std::get<LossEligibilityQuestion>(session.result().mRequests.at(0).mData);

    ASSERT_EQ(lossCheck.mSales.size(), 1U);
    EXPECT_EQ(lossCheck.mSales.at(0).mLossUnits, 1'000'000'000);          // 10 units
    EXPECT_EQ(lossCheck.mSales.at(0).mKnownIneligibleUnits, 300'000'000); // 3 units
    EXPECT_EQ(lossCheck.mSales.at(0).mOpenUnits, 700'000'000);            // 7 units

    const auto loss = session.answer({{"request-0001", 2, OtherActivityAnswer{}}});

    EXPECT_TRUE(loss.empty());

    // The warning that the user gave the ratio, then the warning of the answer `other_activity`;
    // the sale used no changed lot.
    EXPECT_TRUE(session.result().mRequests.empty());
    EXPECT_EQ(codesOf(session.result()),
              (std::vector<ApplicationDiagnosticCode>{
                  ProcessingDiagnosticCode::CorporateActionApplied,
                  ProcessingDiagnosticCode::LossEligibilityUnresolved}));

    ASSERT_TRUE(stages.mBuilt.mReport.has_value());
    ASSERT_EQ(stages.mBuilt.mReport->mLists.size(), 1U);

    const auto& rows = stages.mBuilt.mReport->mLists.at(0).mRows;

    ASSERT_EQ(rows.size(), 2U);

    const auto& purchase = std::get<KdvpPurchaseRow>(rows.at(0).mEntry);
    const auto& sale = std::get<KdvpSaleRow>(rows.at(1).mEntry);

    EXPECT_EQ(purchase.mDate, day("2024-01-10"));
    EXPECT_EQ(purchase.mUnits, 1'000'000'000); // 10 units
    EXPECT_EQ(sale.mDate, day("2024-05-02"));
    EXPECT_EQ(sale.mUnits, 1'000'000'000); // 10 units
    EXPECT_EQ(sale.mLossRule, KdvpLossRule::Blocked);
}

/// One Interactive Brokers statement with the given trade and corporate action rows.
std::string ibkrStatement(std::string_view aTrades, std::string_view aActions) {
    std::string xml = "<FlexQueryResponse queryName=\"synthetic\" type=\"AF\">\n"
                      "  <FlexStatements count=\"1\">\n"
                      "    <FlexStatement accountId=\"U0000000\">\n"
                      "      <AccountInformation accountId=\"U0000000\" ibEntity=\"IB-IE\"/>\n"
                      "      <Trades>\n";

    xml += aTrades;
    xml += "\n      </Trades>\n      <CorporateActions>\n";
    xml += aActions;
    xml += "\n      </CorporateActions>\n    </FlexStatement>\n  </FlexStatements>\n"
           "</FlexQueryResponse>\n";

    return xml;
}

TEST(CorporateActionIntegrationTest, ACashMergerIsASaleAtItsProceeds) {
    // 10 units bought at EUR 40 (basis 400), then taken over for EUR 500 on 2024-09-03: a sale of
    // 10 units at EUR 50, gain 100. The warning names the date of the broker's row.
    const std::string xml = ibkrStatement(
        R"(<Trade accountId="U0000000" transactionID="1001" tradeID="1001" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNE" description="SYNTHETIC CASH TARGET INC" isin="XX0000000051" currency="EUR" tradeDate="20230301" dateTime="20230301;100000" buySell="BUY" quantity="10" tradePrice="40" proceeds="-400" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="O" notes="" multiplier="1"/>)",
        R"(<CorporateAction accountId="U0000000" transactionID="3002" actionID="M9001" type="TC" assetCategory="STK" symbol="SYNE" description="SYNTHETIC CASH TARGET INC" isin="XX0000000051" currency="EUR" dateTime="20240903;202500" reportDate="20240903" quantity="-10" proceeds="500" value="500"/>)");
    const std::vector<ParseResult> parsed{ibkr::IbkrParser{}.parse(xml, "ibkr.xml", 0)};
    CapitalStages stages;
    DecisionSession session;

    const ApplicationResult& result = session.start([&parsed, &stages](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions, &stages);
    });

    EXPECT_TRUE(result.mRequests.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const ApplicationDiagnostic& warning = result.mDiagnostics.at(0);

    EXPECT_EQ(warning.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(warning.mCode,
              ApplicationDiagnosticCode{ProcessingDiagnosticCode::CorporateActionApplied});
    EXPECT_EQ(warning.mMessageKey, MessageKey::CorporateActionAppliedCashMerger);
    EXPECT_EQ(warning.mMessageParams,
              (std::vector<MessageParam>{textParam("saleDate", "2024-09-03")}));

    ASSERT_EQ(stages.mFifo.mIsins.size(), 1U);

    const FifoIsinResult& pool = stages.mFifo.mIsins.at(0);

    EXPECT_EQ(pool.mStatus, FifoIsinStatus::Matched);
    EXPECT_TRUE(pool.mOpenLots.empty());
    ASSERT_EQ(pool.mMatches.size(), 1U);
    EXPECT_EQ(pool.mMatches.at(0).mSaleDate, day("2024-09-03"));
    EXPECT_EQ(pool.mMatches.at(0).mUnits, 1'000'000'000); // 10 units
    // Gain EUR 100 = 500 - 400, in ticks of 10^-16 EUR.
    EXPECT_EQ(pool.mMatches.at(0).mGainTicks,
              ExactRational{WideInteger{1'000'000'000'000'000'000}});

    ASSERT_TRUE(stages.mBuilt.mReport.has_value());
    ASSERT_EQ(stages.mBuilt.mReport->mLists.size(), 1U);

    const std::vector<KdvpRow>& rows = stages.mBuilt.mReport->mLists.at(0).mRows;

    ASSERT_EQ(rows.size(), 2U);

    const KdvpPurchaseRow& purchase = std::get<KdvpPurchaseRow>(rows.at(0).mEntry);
    const KdvpSaleRow& sale = std::get<KdvpSaleRow>(rows.at(1).mEntry);

    EXPECT_EQ(purchase.mDate, day("2023-03-01"));
    EXPECT_EQ(purchase.mUnits, 1'000'000'000);     // 10 units
    EXPECT_EQ(purchase.mUnitPrice, 4'000'000'000); // EUR 40
    EXPECT_EQ(sale.mDate, day("2024-09-03"));
    EXPECT_EQ(sale.mUnits, 1'000'000'000);     // 10 units
    EXPECT_EQ(sale.mUnitPrice, 5'000'000'000); // EUR 500 / 10 units = EUR 50
    EXPECT_EQ(sale.mLossRule, KdvpLossRule::NotBlocked);
}

TEST(CorporateActionIntegrationTest, ACashMergerSellsUnitsOfAnotherBrokerToo) {
    // Broker A (Interactive Brokers) bought 6 units at EUR 10 on 2023-03-01 and sends the row of
    // the takeover for 10 units and EUR 150. Broker B (Trade Republic) bought 4 units at EUR 12 on
    // 2023-03-02 and sends no row. One pool: FIFO takes 6 units at 10 and 4 at 12, so the basis is
    // 60 + 48 = 108 and the gain is 150 - 108 = 42.
    const std::string xml = ibkrStatement(
        R"(<Trade accountId="U0000000" transactionID="1001" tradeID="1001" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNE" description="SYNTHETIC CASH TARGET INC" isin="XX0000000051" currency="EUR" tradeDate="20230301" dateTime="20230301;100000" buySell="BUY" quantity="6" tradePrice="10" proceeds="-60" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="O" notes="" multiplier="1"/>)",
        R"(<CorporateAction accountId="U0000000" transactionID="3002" actionID="M9001" type="TC" assetCategory="STK" symbol="SYNE" description="SYNTHETIC CASH TARGET INC" isin="XX0000000051" currency="EUR" dateTime="20240903;202500" reportDate="20240903" quantity="-10" proceeds="150" value="150"/>)");
    const TemporaryActionInputs files;
    const std::filesystem::path otherBroker = files.write(
        "other.csv",
        {
            R"(2023-03-02T09:00:00Z,2023-03-02,DEFAULT,TRADING,BUY,STOCK,SYNTHETIC CASH TARGET INC,XX0000000051,4,12,-48,,,EUR,,,,,synthetic-buy-1,,,,)",
        });
    const std::vector<ParseResult> parsed{ibkr::IbkrParser{}.parse(xml, "ibkr.xml", 0),
                                          tr::TradeRepublicParser{}.parseFile(otherBroker, 1)};
    CapitalStages stages;
    DecisionSession session;

    const ApplicationResult& result = session.start([&parsed, &stages](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions, &stages);
    });

    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(
        codesOf(result),
        (std::vector<ApplicationDiagnosticCode>{ProcessingDiagnosticCode::CorporateActionApplied}));

    const FifoIsinResult& pool = stages.mFifo.mIsins.at(0);

    EXPECT_EQ(pool.mStatus, FifoIsinStatus::Matched);
    ASSERT_EQ(pool.mMatches.size(), 2U);
    EXPECT_EQ(pool.mMatches.at(0).mUnits, 600'000'000); // 6 units of broker A
    EXPECT_EQ(pool.mMatches.at(1).mUnits, 400'000'000); // 4 units of broker B
    // The sale price is EUR 15 a unit: gains 6 x (15 - 10) = 30 and 4 x (15 - 12) = 12, in total
    // 42, in ticks of 10^-16 EUR.
    EXPECT_EQ(pool.mMatches.at(0).mGainTicks, ExactRational{WideInteger{300'000'000'000'000'000}});
    EXPECT_EQ(pool.mMatches.at(1).mGainTicks, ExactRational{WideInteger{120'000'000'000'000'000}});
}

TEST(CorporateActionIntegrationTest, TheRowsOfOneCashMergerOfTwoAccountsAreOneSaleEach) {
    // 10 units bought; two accounts of the broker send a row each, for 6 and 4 units on the same
    // day. The pool is empty after the second row, so there is no error.
    const std::string xml = ibkrStatement(
        R"(<Trade accountId="U0000000" transactionID="1001" tradeID="1001" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNE" description="SYNTHETIC CASH TARGET INC" isin="XX0000000051" currency="EUR" tradeDate="20230301" dateTime="20230301;100000" buySell="BUY" quantity="10" tradePrice="40" proceeds="-400" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="O" notes="" multiplier="1"/>)",
        R"(<CorporateAction accountId="U0000000" transactionID="3002" actionID="M9001" type="TC" assetCategory="STK" symbol="SYNE" description="SYNTHETIC CASH TARGET INC" isin="XX0000000051" currency="EUR" dateTime="20240903;202500" reportDate="20240903" quantity="-6" proceeds="300" value="300"/>
        <CorporateAction accountId="U0000001" transactionID="3003" actionID="M9001" type="TC" assetCategory="STK" symbol="SYNE" description="SYNTHETIC CASH TARGET INC" isin="XX0000000051" currency="EUR" dateTime="20240903;202500" reportDate="20240903" quantity="-4" proceeds="200" value="200"/>)");
    const std::vector<ParseResult> parsed{ibkr::IbkrParser{}.parse(xml, "ibkr.xml", 0)};
    CapitalStages stages;
    DecisionSession session;

    const ApplicationResult& result = session.start([&parsed, &stages](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions, &stages);
    });

    // One warning for each row, no error.
    EXPECT_EQ(
        codesOf(result),
        (std::vector<ApplicationDiagnosticCode>{ProcessingDiagnosticCode::CorporateActionApplied,
                                                ProcessingDiagnosticCode::CorporateActionApplied}));
    EXPECT_EQ(stages.mFifo.mIsins.at(0).mStatus, FifoIsinStatus::Matched);
    EXPECT_TRUE(stages.mFifo.mIsins.at(0).mOpenLots.empty());
    ASSERT_TRUE(stages.mBuilt.mReport.has_value());
}

TEST(CorporateActionIntegrationTest, UnitsLeftAfterACashMergerBlockTheInvestment) {
    // 10 units bought, a takeover row for 6: 4 units are still held, so a row is probably
    // missing. Only this ISIN fails.
    const std::string xml = ibkrStatement(
        R"(<Trade accountId="U0000000" transactionID="1001" tradeID="1001" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNE" description="SYNTHETIC CASH TARGET INC" isin="XX0000000051" currency="EUR" tradeDate="20230301" dateTime="20230301;100000" buySell="BUY" quantity="10" tradePrice="40" proceeds="-400" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="O" notes="" multiplier="1"/>)",
        R"(<CorporateAction accountId="U0000000" transactionID="3002" actionID="M9001" type="TC" assetCategory="STK" symbol="SYNE" description="SYNTHETIC CASH TARGET INC" isin="XX0000000051" currency="EUR" dateTime="20240903;202500" reportDate="20240903" quantity="-6" proceeds="300" value="300"/>)");
    const std::vector<ParseResult> parsed{ibkr::IbkrParser{}.parse(xml, "ibkr.xml", 0)};
    CapitalStages stages;
    DecisionSession session;

    const ApplicationResult& result = session.start([&parsed, &stages](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions, &stages);
    });

    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Failed);
    EXPECT_EQ(stages.mFifo.mIsins.at(0).mStatus, FifoIsinStatus::NumericFailure);
    EXPECT_FALSE(stages.mBuilt.mReport.has_value());

    const auto error = std::find_if(result.mDiagnostics.begin(),
                                    result.mDiagnostics.end(),
                                    [](const ApplicationDiagnostic& aDiagnostic) {
                                        return aDiagnostic.mSeverity == DiagnosticSeverity::Error;
                                    });

    ASSERT_NE(error, result.mDiagnostics.end());

    EXPECT_EQ(error->mCode,
              ApplicationDiagnosticCode{ProcessingDiagnosticCode::CorporateActionInconsistent});
    EXPECT_EQ(error->mMessageKey, MessageKey::CorporateActionInconsistentUnitsLeftAfterCashMerger);
}

TEST(CorporateActionIntegrationTest, AnIsinChangeOfASoldInvestmentAsksOneQuestion) {
    // 20 units of the old ISIN bought in 2023, the ISIN changes on 2024-03-04 (two rows), 20 units
    // of the new ISIN sold in 2025: one question, and both ISINs are blocked until it is answered.
    const std::string xml = ibkrStatement(
        R"(<Trade accountId="U0000000" transactionID="1001" tradeID="1001" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNO" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" tradeDate="20230301" dateTime="20230301;100000" buySell="BUY" quantity="20" tradePrice="15" proceeds="-300" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="O" notes="" multiplier="1"/>
        <Trade accountId="U0000000" transactionID="1002" tradeID="1002" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" tradeDate="20250303" dateTime="20250303;100000" buySell="SELL" quantity="-20" tradePrice="18" proceeds="360" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="C" notes="" multiplier="1"/>)",
        R"(<CorporateAction accountId="U0000000" transactionID="4001" actionID="C9001" type="IC" assetCategory="STK" symbol="SYNO" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" dateTime="20240304;202500" reportDate="20240304" quantity="-20" proceeds="0" value="0"/>
        <CorporateAction accountId="U0000000" transactionID="4002" actionID="C9001" type="IC" assetCategory="STK" symbol="SYNN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" dateTime="20240304;202500" reportDate="20240304" quantity="20" proceeds="0" value="0"/>)");
    const std::vector<ParseResult> parsed{ibkr::IbkrParser{}.parse(xml, "ibkr.xml", 0)};
    DecisionSession session;

    const ApplicationResult& result = session.start([&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2025, aDecisions);
    });

    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::NeedsInput);

    ASSERT_EQ(result.mRequests.size(), 1U);

    const DecisionRequest& request = result.mRequests.at(0);
    const IsinChangeQuestion& data = std::get<IsinChangeQuestion>(request.mData);

    EXPECT_EQ(request.mType, RequestType::CorporateActionIsinChangeRequired);
    EXPECT_EQ(request.mIsin, "XX0000000134");
    EXPECT_EQ(data.mOldIsin, "XX0000000134");
    EXPECT_EQ(data.mOldName, "OLD SYNTHETIC INC");
    EXPECT_EQ(data.mNewIsin, "XX0000000135");
    EXPECT_EQ(data.mNewName, "NEW SYNTHETIC INC");
    EXPECT_EQ(data.mEffectiveDate, day("2024-03-04"));
    EXPECT_EQ(data.mEarliestDate, day("2024-03-04"));
    EXPECT_EQ(data.mLatestDate, day("2024-03-04"));
    // 20 units came in for 20 that left: 1 new for 1 old.
    EXPECT_EQ(data.mSuggestedRatio, (SplitRatio{.mNewShares = 1, .mOldShares = 1}));
    ASSERT_EQ(data.mSources.size(), 2U);
    EXPECT_EQ(data.mSources.at(0).mTransactionId, "4001");
    EXPECT_EQ(data.mSources.at(0).mSourceValue, "-20"); // the units that left
    EXPECT_EQ(data.mSources.at(1).mTransactionId, "4002");
    EXPECT_EQ(data.mSources.at(1).mSourceValue, "20"); // the units that came in

    // One error for each ISIN, both linked to the question.
    ASSERT_EQ(result.mDiagnostics.size(), 2U);
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey,
              MessageKey::CorporateActionAmbiguousIsinChangeRequired);
    EXPECT_EQ(result.mDiagnostics.at(0).mIsin, "XX0000000134");
    EXPECT_EQ(result.mDiagnostics.at(1).mIsin, "XX0000000135");
    EXPECT_EQ(result.mDiagnostics.at(0).mRequestId, "request-0001");
    EXPECT_EQ(result.mDiagnostics.at(1).mRequestId, "request-0001");
}

TEST(CorporateActionIntegrationTest, AnIsinChangeWithoutASaleOfEitherIsinAsksNothing) {
    // The same rows, but the sale of the new ISIN is in 2025 and the year is 2024.
    const std::string xml = ibkrStatement(
        R"(<Trade accountId="U0000000" transactionID="1001" tradeID="1001" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNO" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" tradeDate="20230301" dateTime="20230301;100000" buySell="BUY" quantity="20" tradePrice="15" proceeds="-300" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="O" notes="" multiplier="1"/>
        <Trade accountId="U0000000" transactionID="1002" tradeID="1002" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" tradeDate="20250303" dateTime="20250303;100000" buySell="SELL" quantity="-20" tradePrice="18" proceeds="360" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="C" notes="" multiplier="1"/>)",
        R"(<CorporateAction accountId="U0000000" transactionID="4001" actionID="C9001" type="IC" assetCategory="STK" symbol="SYNO" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" dateTime="20240304;202500" reportDate="20240304" quantity="-20" proceeds="0" value="0"/>
        <CorporateAction accountId="U0000000" transactionID="4002" actionID="C9001" type="IC" assetCategory="STK" symbol="SYNN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" dateTime="20240304;202500" reportDate="20240304" quantity="20" proceeds="0" value="0"/>)");
    const std::vector<ParseResult> parsed{ibkr::IbkrParser{}.parse(xml, "ibkr.xml", 0)};
    DecisionSession session;

    const ApplicationResult& result = session.start([&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions);
    });

    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::NoData);
}

TEST(CorporateActionIntegrationTest, AnIsinChangeCarriesTheLotsToTheNewIsin) {
    // Answer: the same company, 1 : 1, on the row's date. The purchase of 2023-03-01 (20 units at
    // EUR 15) is listed under the new ISIN, and the sale of 20 units at EUR 18 makes a gain of 60.
    const std::string xml = ibkrStatement(
        R"(<Trade accountId="U0000000" transactionID="1001" tradeID="1001" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNO" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" tradeDate="20230301" dateTime="20230301;100000" buySell="BUY" quantity="20" tradePrice="15" proceeds="-300" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="O" notes="" multiplier="1"/>
        <Trade accountId="U0000000" transactionID="1002" tradeID="1002" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" tradeDate="20250303" dateTime="20250303;100000" buySell="SELL" quantity="-20" tradePrice="18" proceeds="360" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="C" notes="" multiplier="1"/>)",
        R"(<CorporateAction accountId="U0000000" transactionID="4001" actionID="C9001" type="IC" assetCategory="STK" symbol="SYNO" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" dateTime="20240304;202500" reportDate="20240304" quantity="-20" proceeds="0" value="0"/>
        <CorporateAction accountId="U0000000" transactionID="4002" actionID="C9001" type="IC" assetCategory="STK" symbol="SYNN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" dateTime="20240304;202500" reportDate="20240304" quantity="20" proceeds="0" value="0"/>)");
    const std::vector<ParseResult> parsed{ibkr::IbkrParser{}.parse(xml, "ibkr.xml", 0)};
    CapitalStages stages;
    DecisionSession session;
    session.start([&parsed, &stages](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2025, aDecisions, &stages);
    });

    const std::vector<RejectedAnswer> rejections =
        session.answer({{"request-0001",
                         1,
                         IsinChangeAnswer{.mSameIssuer = true,
                                          .mNewShares = 1,
                                          .mOldShares = 1,
                                          .mEffectiveDate = day("2024-03-04")}}});

    EXPECT_TRUE(rejections.empty());

    const ApplicationResult& result = session.result();

    EXPECT_TRUE(result.mRequests.empty());

    // The only diagnostic is the warning that the change was applied.
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey, MessageKey::CorporateActionAppliedIsinChange);
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageParams,
              (std::vector<MessageParam>{textParam("oldIsin", "XX0000000134"),
                                         textParam("newIsin", "XX0000000135"),
                                         numberParam("newShares", 1),
                                         numberParam("oldShares", 1),
                                         textParam("effectiveDate", "2024-03-04")}));

    // One pool: the old ISIN has no instrument any more.
    ASSERT_EQ(stages.mFifo.mIsins.size(), 1U);
    EXPECT_EQ(stages.mFifo.mIsins.at(0).mIsin, "XX0000000135");
    ASSERT_EQ(stages.mFifo.mIsins.at(0).mMatches.size(), 1U);
    EXPECT_EQ(stages.mFifo.mIsins.at(0).mMatches.at(0).mPurchaseDate, day("2023-03-01"));
    EXPECT_EQ(stages.mFifo.mIsins.at(0).mMatches.at(0).mUnits, 2'000'000'000); // 20 units
    // Gain EUR 60 = 20 x (18 - 15), in ticks of 10^-16 EUR.
    EXPECT_EQ(stages.mFifo.mIsins.at(0).mMatches.at(0).mGainTicks,
              ExactRational{WideInteger{600'000'000'000'000'000}});

    ASSERT_TRUE(stages.mBuilt.mReport.has_value());
    ASSERT_EQ(stages.mBuilt.mReport->mLists.size(), 1U);

    const KdvpInventoryList& list = stages.mBuilt.mReport->mLists.at(0);

    EXPECT_EQ(list.mIsin, "XX0000000135");
    EXPECT_EQ(list.mName, "NEW SYNTHETIC INC");
    ASSERT_EQ(list.mRows.size(), 2U);

    const KdvpPurchaseRow& purchase = std::get<KdvpPurchaseRow>(list.mRows.at(0).mEntry);
    const KdvpSaleRow& sale = std::get<KdvpSaleRow>(list.mRows.at(1).mEntry);

    EXPECT_EQ(purchase.mDate, day("2023-03-01"));
    EXPECT_EQ(purchase.mMethod, KdvpPurchaseMethod::B);
    EXPECT_EQ(purchase.mUnits, 2'000'000'000);     // 20 units
    EXPECT_EQ(purchase.mUnitPrice, 1'500'000'000); // EUR 15
    EXPECT_EQ(sale.mDate, day("2025-03-03"));
    EXPECT_EQ(sale.mUnits, 2'000'000'000);     // 20 units
    EXPECT_EQ(sale.mUnitPrice, 1'800'000'000); // EUR 18
    EXPECT_EQ(sale.mLossRule, KdvpLossRule::NotBlocked);
}

TEST(CorporateActionIntegrationTest, AnIsinChangeWithARatioListsTheUnitsAfterIt) {
    // The rows show 20 units out and 40 in: 2 new units for 1 old one. 20 units at EUR 15 (basis
    // 300) become 40 units at EUR 7.50; the sale of 40 units at EUR 9 (360) makes a gain of 60.
    const std::string xml = ibkrStatement(
        R"(<Trade accountId="U0000000" transactionID="1001" tradeID="1001" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNO" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" tradeDate="20230301" dateTime="20230301;100000" buySell="BUY" quantity="20" tradePrice="15" proceeds="-300" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="O" notes="" multiplier="1"/>
        <Trade accountId="U0000000" transactionID="1002" tradeID="1002" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" tradeDate="20250303" dateTime="20250303;100000" buySell="SELL" quantity="-40" tradePrice="9" proceeds="360" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="C" notes="" multiplier="1"/>)",
        R"(<CorporateAction accountId="U0000000" transactionID="4001" actionID="C9001" type="IC" assetCategory="STK" symbol="SYNO" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" dateTime="20240304;202500" reportDate="20240304" quantity="-20" proceeds="0" value="0"/>
        <CorporateAction accountId="U0000000" transactionID="4002" actionID="C9001" type="IC" assetCategory="STK" symbol="SYNN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" dateTime="20240304;202500" reportDate="20240304" quantity="40" proceeds="0" value="0"/>)");
    const std::vector<ParseResult> parsed{ibkr::IbkrParser{}.parse(xml, "ibkr.xml", 0)};
    CapitalStages stages;
    DecisionSession session;
    const ApplicationResult& question = session.start([&parsed, &stages](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2025, aDecisions, &stages);
    });

    ASSERT_EQ(question.mRequests.size(), 1U);
    EXPECT_EQ(std::get<IsinChangeQuestion>(question.mRequests.at(0).mData).mSuggestedRatio,
              (SplitRatio{.mNewShares = 2, .mOldShares = 1}));

    const std::vector<RejectedAnswer> rejections =
        session.answer({{"request-0001",
                         1,
                         IsinChangeAnswer{.mSameIssuer = true,
                                          .mNewShares = 4,
                                          .mOldShares = 2,
                                          .mEffectiveDate = day("2024-03-04")}}});

    EXPECT_TRUE(rejections.empty());

    // The answer is kept reduced: 4 / 2 as 2 / 1.
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions.at(0).mAnswer,
              (AnswerValue{IsinChangeAnswer{.mSameIssuer = true,
                                            .mNewShares = 2,
                                            .mOldShares = 1,
                                            .mEffectiveDate = day("2024-03-04")}}));

    // No ratio question and one warning: the change was applied (the split row has no second one).
    EXPECT_TRUE(session.result().mRequests.empty());
    EXPECT_EQ(
        codesOf(session.result()),
        (std::vector<ApplicationDiagnosticCode>{ProcessingDiagnosticCode::CorporateActionApplied}));
    EXPECT_EQ(session.result().mDiagnostics.at(0).mMessageKey,
              MessageKey::CorporateActionAppliedIsinChange);

    ASSERT_TRUE(stages.mBuilt.mReport.has_value());
    ASSERT_EQ(stages.mBuilt.mReport->mLists.size(), 1U);

    const std::vector<KdvpRow>& rows = stages.mBuilt.mReport->mLists.at(0).mRows;

    ASSERT_EQ(rows.size(), 2U);

    const KdvpPurchaseRow& purchase = std::get<KdvpPurchaseRow>(rows.at(0).mEntry);
    const KdvpSaleRow& sale = std::get<KdvpSaleRow>(rows.at(1).mEntry);

    EXPECT_EQ(purchase.mDate, day("2023-03-01"));
    EXPECT_EQ(purchase.mUnits, 4'000'000'000);   // 40 units
    EXPECT_EQ(purchase.mUnitPrice, 750'000'000); // EUR 300 / 40 units = EUR 7.50
    EXPECT_EQ(sale.mUnits, 4'000'000'000);       // 40 units
    EXPECT_EQ(sale.mUnitPrice, 900'000'000);     // EUR 9
}

TEST(CorporateActionIntegrationTest, AnIsinChangeRatioThatDiffersFromItsRowsIsRejected) {
    // The rows show 20 units out and 20 in, so only 1 : 1 fits. 2 : 1 would make 40 units.
    const std::string xml = ibkrStatement(
        R"(<Trade accountId="U0000000" transactionID="1001" tradeID="1001" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNO" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" tradeDate="20230301" dateTime="20230301;100000" buySell="BUY" quantity="20" tradePrice="15" proceeds="-300" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="O" notes="" multiplier="1"/>
        <Trade accountId="U0000000" transactionID="1002" tradeID="1002" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" tradeDate="20250303" dateTime="20250303;100000" buySell="SELL" quantity="-20" tradePrice="18" proceeds="360" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="C" notes="" multiplier="1"/>)",
        R"(<CorporateAction accountId="U0000000" transactionID="4001" actionID="C9001" type="IC" assetCategory="STK" symbol="SYNO" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" dateTime="20240304;202500" reportDate="20240304" quantity="-20" proceeds="0" value="0"/>
        <CorporateAction accountId="U0000000" transactionID="4002" actionID="C9001" type="IC" assetCategory="STK" symbol="SYNN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" dateTime="20240304;202500" reportDate="20240304" quantity="20" proceeds="0" value="0"/>)");
    const std::vector<ParseResult> parsed{ibkr::IbkrParser{}.parse(xml, "ibkr.xml", 0)};
    DecisionSession session;
    session.start([&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2025, aDecisions);
    });

    const std::vector<RejectedAnswer> rejections =
        session.answer({{"request-0001",
                         1,
                         IsinChangeAnswer{.mSameIssuer = true,
                                          .mNewShares = 2,
                                          .mOldShares = 1,
                                          .mEffectiveDate = day("2024-03-04")}}});

    ASSERT_EQ(rejections.size(), 1U);
    EXPECT_EQ(rejections.at(0).mReason, AnswerRejection::IsinChangeRatioMismatch);
    EXPECT_EQ(rejections.at(0).mMessageKey, MessageKey::AnswerRejectedIsinChangeRatioMismatch);
    EXPECT_EQ(rejections.at(0).mMessageParams,
              (std::vector<MessageParam>{textParam("requestId", "request-0001"),
                                         numberParam("newShares", 1),
                                         numberParam("oldShares", 1)}));

    // The question is still open: the answer was not taken.
    EXPECT_EQ(session.result().mRequests.size(), 1U);

    // The same ratio written as 3 : 3 is reduced to 1 : 1 and fits.
    const std::vector<RejectedAnswer> accepted =
        session.answer({{"request-0001",
                         1,
                         IsinChangeAnswer{.mSameIssuer = true,
                                          .mNewShares = 3,
                                          .mOldShares = 3,
                                          .mEffectiveDate = day("2024-03-04")}}});

    EXPECT_TRUE(accepted.empty());
    EXPECT_TRUE(session.result().mRequests.empty());
}

TEST(CorporateActionIntegrationTest, AnIsinChangeAnsweredNoIsAStockMerger) {
    const std::string xml = ibkrStatement(
        R"(<Trade accountId="U0000000" transactionID="1001" tradeID="1001" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNO" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" tradeDate="20230301" dateTime="20230301;100000" buySell="BUY" quantity="20" tradePrice="15" proceeds="-300" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="O" notes="" multiplier="1"/>
        <Trade accountId="U0000000" transactionID="1002" tradeID="1002" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" tradeDate="20250303" dateTime="20250303;100000" buySell="SELL" quantity="-20" tradePrice="18" proceeds="360" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="C" notes="" multiplier="1"/>)",
        R"(<CorporateAction accountId="U0000000" transactionID="4001" actionID="C9001" type="IC" assetCategory="STK" symbol="SYNO" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" dateTime="20240304;202500" reportDate="20240304" quantity="-20" proceeds="0" value="0"/>
        <CorporateAction accountId="U0000000" transactionID="4002" actionID="C9001" type="IC" assetCategory="STK" symbol="SYNN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" dateTime="20240304;202500" reportDate="20240304" quantity="20" proceeds="0" value="0"/>)");
    const std::vector<ParseResult> parsed{ibkr::IbkrParser{}.parse(xml, "ibkr.xml", 0)};
    DecisionSession session;
    session.start([&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2025, aDecisions);
    });

    const std::vector<RejectedAnswer> rejections =
        session.answer({{"request-0001", 1, IsinChangeAnswer{}}});

    EXPECT_TRUE(rejections.empty());

    const ApplicationResult& result = session.result();

    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Failed);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode,
              ApplicationDiagnosticCode{ProcessingDiagnosticCode::UnsupportedCorporateAction});
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey,
              MessageKey::UnsupportedCorporateActionStockMerger);
    EXPECT_EQ(result.mDiagnostics.at(0).mIsin, "XX0000000135");
}

TEST(CorporateActionIntegrationTest, ACancelledIsinChangeBlocksBothIsins) {
    const std::string xml = ibkrStatement(
        R"(<Trade accountId="U0000000" transactionID="1001" tradeID="1001" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNO" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" tradeDate="20230301" dateTime="20230301;100000" buySell="BUY" quantity="20" tradePrice="15" proceeds="-300" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="O" notes="" multiplier="1"/>
        <Trade accountId="U0000000" transactionID="1002" tradeID="1002" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" tradeDate="20250303" dateTime="20250303;100000" buySell="SELL" quantity="-20" tradePrice="18" proceeds="360" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="C" notes="" multiplier="1"/>)",
        R"(<CorporateAction accountId="U0000000" transactionID="4001" actionID="C9001" type="IC" assetCategory="STK" symbol="SYNO" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" dateTime="20240304;202500" reportDate="20240304" quantity="-20" proceeds="0" value="0"/>
        <CorporateAction accountId="U0000000" transactionID="4002" actionID="C9001" type="IC" assetCategory="STK" symbol="SYNN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" dateTime="20240304;202500" reportDate="20240304" quantity="20" proceeds="0" value="0"/>)");
    const std::vector<ParseResult> parsed{ibkr::IbkrParser{}.parse(xml, "ibkr.xml", 0)};
    DecisionSession session;
    session.start([&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2025, aDecisions);
    });

    const std::vector<RejectedAnswer> rejections =
        session.answer({{"request-0001", 1, CancelAnswer{}}});

    EXPECT_TRUE(rejections.empty());

    const ApplicationResult& result = session.result();

    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Failed);

    ASSERT_EQ(result.mDiagnostics.size(), 2U);
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey, MessageKey::CorporateActionAmbiguousCancelled);
    EXPECT_EQ(result.mDiagnostics.at(0).mIsin, "XX0000000134");
    EXPECT_EQ(result.mDiagnostics.at(1).mMessageKey, MessageKey::CorporateActionAmbiguousCancelled);
    EXPECT_EQ(result.mDiagnostics.at(1).mIsin, "XX0000000135");
}

TEST(CorporateActionIntegrationTest, ATradeOfTheNewIsinBeforeTheChangeBlocksBothIsins) {
    // 5 units of the new ISIN were bought on 2024-03-01, before the change of 2024-03-04: the
    // trades do not fit one change.
    const std::string xml = ibkrStatement(
        R"(<Trade accountId="U0000000" transactionID="1001" tradeID="1001" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNO" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" tradeDate="20230301" dateTime="20230301;100000" buySell="BUY" quantity="20" tradePrice="15" proceeds="-300" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="O" notes="" multiplier="1"/>
        <Trade accountId="U0000000" transactionID="1003" tradeID="1003" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" tradeDate="20240301" dateTime="20240301;100000" buySell="BUY" quantity="5" tradePrice="16" proceeds="-80" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="O" notes="" multiplier="1"/>
        <Trade accountId="U0000000" transactionID="1002" tradeID="1002" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" tradeDate="20250303" dateTime="20250303;100000" buySell="SELL" quantity="-20" tradePrice="18" proceeds="360" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="C" notes="" multiplier="1"/>)",
        R"(<CorporateAction accountId="U0000000" transactionID="4001" actionID="C9001" type="IC" assetCategory="STK" symbol="SYNO" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" dateTime="20240304;202500" reportDate="20240304" quantity="-20" proceeds="0" value="0"/>
        <CorporateAction accountId="U0000000" transactionID="4002" actionID="C9001" type="IC" assetCategory="STK" symbol="SYNN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" dateTime="20240304;202500" reportDate="20240304" quantity="20" proceeds="0" value="0"/>)");
    const std::vector<ParseResult> parsed{ibkr::IbkrParser{}.parse(xml, "ibkr.xml", 0)};
    DecisionSession session;
    session.start([&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2025, aDecisions);
    });

    const std::vector<RejectedAnswer> rejections =
        session.answer({{"request-0001",
                         1,
                         IsinChangeAnswer{.mSameIssuer = true,
                                          .mNewShares = 1,
                                          .mOldShares = 1,
                                          .mEffectiveDate = day("2024-03-04")}}});

    EXPECT_TRUE(rejections.empty());

    const ApplicationResult& result = session.result();

    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Failed);

    ASSERT_EQ(result.mDiagnostics.size(), 2U);
    EXPECT_EQ(result.mDiagnostics.at(0).mCode,
              ApplicationDiagnosticCode{ProcessingDiagnosticCode::CorporateActionInconsistent});
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey,
              MessageKey::CorporateActionInconsistentTradeAcrossIsinChange);
    EXPECT_EQ(result.mDiagnostics.at(1).mMessageKey,
              MessageKey::CorporateActionInconsistentTradeAcrossIsinChange);
}

TEST(CorporateActionIntegrationTest, AnEffectiveDateOutsideTheRowsOfAnIsinChangeIsRejected) {
    const std::string xml = ibkrStatement(
        R"(<Trade accountId="U0000000" transactionID="1001" tradeID="1001" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNO" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" tradeDate="20230301" dateTime="20230301;100000" buySell="BUY" quantity="20" tradePrice="15" proceeds="-300" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="O" notes="" multiplier="1"/>
        <Trade accountId="U0000000" transactionID="1002" tradeID="1002" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" tradeDate="20250303" dateTime="20250303;100000" buySell="SELL" quantity="-20" tradePrice="18" proceeds="360" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="C" notes="" multiplier="1"/>)",
        R"(<CorporateAction accountId="U0000000" transactionID="4001" actionID="C9001" type="IC" assetCategory="STK" symbol="SYNO" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" dateTime="20240304;202500" reportDate="20240304" quantity="-20" proceeds="0" value="0"/>
        <CorporateAction accountId="U0000000" transactionID="4002" actionID="C9001" type="IC" assetCategory="STK" symbol="SYNN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" dateTime="20240304;202500" reportDate="20240304" quantity="20" proceeds="0" value="0"/>)");
    const std::vector<ParseResult> parsed{ibkr::IbkrParser{}.parse(xml, "ibkr.xml", 0)};
    DecisionSession session;
    session.start([&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2025, aDecisions);
    });

    // The rows are all dated 2024-03-04, so no other date is allowed.
    const std::vector<RejectedAnswer> rejections =
        session.answer({{"request-0001",
                         1,
                         IsinChangeAnswer{.mSameIssuer = true,
                                          .mNewShares = 1,
                                          .mOldShares = 1,
                                          .mEffectiveDate = day("2024-03-05")}}});

    ASSERT_EQ(rejections.size(), 1U);
    EXPECT_EQ(rejections.at(0).mReason, AnswerRejection::EffectiveDateOutOfRange);

    const std::vector<RejectedAnswer> zero =
        session.answer({{"request-0001",
                         1,
                         IsinChangeAnswer{.mSameIssuer = true,
                                          .mNewShares = 0,
                                          .mOldShares = 1,
                                          .mEffectiveDate = day("2024-03-04")}}});

    ASSERT_EQ(zero.size(), 1U);
    EXPECT_EQ(zero.at(0).mReason, AnswerRejection::RatioNotPositive);
}

TEST(CorporateActionIntegrationTest, ALossSaleBeforeAnIsinChangeIsComparedWithARebuyAfterIt) {
    // 10 units of the old ISIN bought at EUR 20 on 2024-01-10, sold at EUR 15 on 2024-02-20 (a
    // loss of EUR 50), the ISIN changes on 2024-03-04, and 10 units of the new ISIN are bought on
    // 2024-03-10, 19 days after the sale. After the change they are one instrument, so the new
    // purchase replaces the sold units: the sale is blocked (F10 false) and nothing is left to ask.
    // The sale stays listed under the old ISIN it had on its date.
    const std::string xml = ibkrStatement(
        R"(<Trade accountId="U0000000" transactionID="1004" tradeID="1004" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNO" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" tradeDate="20240110" dateTime="20240110;100000" buySell="BUY" quantity="10" tradePrice="20" proceeds="-200" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="O" notes="" multiplier="1"/>
        <Trade accountId="U0000000" transactionID="1005" tradeID="1005" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNO" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" tradeDate="20240220" dateTime="20240220;100000" buySell="SELL" quantity="-10" tradePrice="15" proceeds="150" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="C" notes="" multiplier="1"/>
        <Trade accountId="U0000000" transactionID="1006" tradeID="1006" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" tradeDate="20240310" dateTime="20240310;100000" buySell="BUY" quantity="10" tradePrice="16" proceeds="-160" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="O" notes="" multiplier="1"/>)",
        R"(<CorporateAction accountId="U0000000" transactionID="4001" actionID="C9001" type="IC" assetCategory="STK" symbol="SYNO" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" dateTime="20240304;202500" reportDate="20240304" quantity="-10" proceeds="0" value="0"/>
        <CorporateAction accountId="U0000000" transactionID="4002" actionID="C9001" type="IC" assetCategory="STK" symbol="SYNN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" dateTime="20240304;202500" reportDate="20240304" quantity="10" proceeds="0" value="0"/>)");
    const std::vector<ParseResult> parsed{ibkr::IbkrParser{}.parse(xml, "ibkr.xml", 0)};
    CapitalStages stages;
    DecisionSession session;
    session.start([&parsed, &stages](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions, &stages);
    });

    const std::vector<RejectedAnswer> rejections =
        session.answer({{"request-0001",
                         1,
                         IsinChangeAnswer{.mSameIssuer = true,
                                          .mNewShares = 1,
                                          .mOldShares = 1,
                                          .mEffectiveDate = day("2024-03-04")}}});

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(session.result().mRequests.empty());
    ASSERT_TRUE(stages.mBuilt.mReport.has_value());
    ASSERT_EQ(stages.mBuilt.mReport->mLists.size(), 1U);

    const KdvpInventoryList& list = stages.mBuilt.mReport->mLists.at(0);

    EXPECT_EQ(list.mIsin, "XX0000000134");
    EXPECT_EQ(list.mName, "OLD SYNTHETIC INC");

    const std::vector<KdvpRow>& rows = list.mRows;

    ASSERT_EQ(rows.size(), 2U);

    const KdvpSaleRow& sale = std::get<KdvpSaleRow>(rows.at(1).mEntry);

    EXPECT_EQ(sale.mDate, day("2024-02-20"));
    EXPECT_EQ(sale.mUnits, 1'000'000'000); // 10 units
    EXPECT_EQ(sale.mLossRule, KdvpLossRule::Blocked);
}

TEST(CorporateActionIntegrationTest, TheLossQuestionNamesTheOldIsinOfASaleMadeBeforeTheChange) {
    // 10 units of the old ISIN bought at EUR 20 on 2024-01-10, sold at EUR 15 on 2024-02-20, the
    // ISIN changes on 2024-03-04, and only 4 units of the new ISIN are bought on 2024-03-10. The
    // 4 units block 4 of the 10 loss units, so 6 stay open and the loss question is asked. It is
    // about the new ISIN (the pool); the sale says it was made as the old one.
    const std::string xml = ibkrStatement(
        R"(<Trade accountId="U0000000" transactionID="1004" tradeID="1004" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNO" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" tradeDate="20240110" dateTime="20240110;100000" buySell="BUY" quantity="10" tradePrice="20" proceeds="-200" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="O" notes="" multiplier="1"/>
        <Trade accountId="U0000000" transactionID="1005" tradeID="1005" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNO" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" tradeDate="20240220" dateTime="20240220;100000" buySell="SELL" quantity="-10" tradePrice="15" proceeds="150" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="C" notes="" multiplier="1"/>
        <Trade accountId="U0000000" transactionID="1006" tradeID="1006" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" tradeDate="20240310" dateTime="20240310;100000" buySell="BUY" quantity="4" tradePrice="16" proceeds="-64" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="O" notes="" multiplier="1"/>)",
        R"(<CorporateAction accountId="U0000000" transactionID="4001" actionID="C9001" type="IC" assetCategory="STK" symbol="SYNO" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" dateTime="20240304;202500" reportDate="20240304" quantity="-10" proceeds="0" value="0"/>
        <CorporateAction accountId="U0000000" transactionID="4002" actionID="C9001" type="IC" assetCategory="STK" symbol="SYNN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" dateTime="20240304;202500" reportDate="20240304" quantity="10" proceeds="0" value="0"/>)");
    const std::vector<ParseResult> parsed{ibkr::IbkrParser{}.parse(xml, "ibkr.xml", 0)};
    DecisionSession session;
    session.start([&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions, nullptr);
    });

    const std::vector<RejectedAnswer> rejections =
        session.answer({{"request-0001",
                         1,
                         IsinChangeAnswer{.mSameIssuer = true,
                                          .mNewShares = 1,
                                          .mOldShares = 1,
                                          .mEffectiveDate = day("2024-03-04")}}});

    EXPECT_TRUE(rejections.empty());
    ASSERT_EQ(session.result().mRequests.size(), 1U);

    const LossEligibilityQuestion& lossCheck =
        std::get<LossEligibilityQuestion>(session.result().mRequests.at(0).mData);

    EXPECT_EQ(lossCheck.mIsin, "XX0000000135");
    ASSERT_EQ(lossCheck.mSales.size(), 1U);
    EXPECT_EQ(lossCheck.mSales.at(0).mOpenUnits, 600'000'000); // 10 - 4 units
    ASSERT_TRUE(lossCheck.mSales.at(0).mSoldAs.has_value());
    EXPECT_EQ(lossCheck.mSales.at(0).mSoldAs->mIsin, "XX0000000134");
    EXPECT_EQ(lossCheck.mSales.at(0).mSoldAs->mName, "OLD SYNTHETIC INC");
}

TEST(CorporateActionIntegrationTest, ASaleBeforeAMergerRowMustFitTheUnitsAfterTheSplit) {
    // 10 units bought, a split row on 2024-03-01, a sale of 40 units on 2024-06-03 and a merger
    // row on 2024-06-05. The merger has no question that checks the sale, so the sale 2 days
    // before its row must fit: 2 / 1 gives 20 units, which do not cover 40.
    const std::string xml = ibkrStatement(
        R"(<Trade accountId="U0000000" transactionID="1001" tradeID="1001" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNS" description="SYNTHETIC SHARE INC" isin="XX0000000201" currency="EUR" tradeDate="20230201" dateTime="20230201;100000" buySell="BUY" quantity="10" tradePrice="10" proceeds="-100" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="O" notes="" multiplier="1"/>
        <Trade accountId="U0000000" transactionID="1002" tradeID="1002" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYNS" description="SYNTHETIC SHARE INC" isin="XX0000000201" currency="EUR" tradeDate="20240603" dateTime="20240603;100000" buySell="SELL" quantity="-40" tradePrice="5" proceeds="200" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="C" notes="" multiplier="1"/>)",
        R"(<CorporateAction accountId="U0000000" transactionID="4101" actionID="S9101" type="FS" assetCategory="STK" symbol="SYNS" description="SYNTHETIC SHARE INC" isin="XX0000000201" currency="EUR" dateTime="20240301;202500" reportDate="20240301" quantity="10" proceeds="0" value="0"/>
        <CorporateAction accountId="U0000000" transactionID="4102" actionID="M9101" type="TC" assetCategory="STK" symbol="SYNS" description="SYNTHETIC SHARE INC" isin="XX0000000201" currency="EUR" dateTime="20240605;202500" reportDate="20240605" quantity="-20" proceeds="0" value="0"/>)");
    const std::vector<ParseResult> parsed{ibkr::IbkrParser{}.parse(xml, "ibkr.xml", 0)};
    DecisionSession session;

    session.start([&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions);
    });

    ASSERT_EQ(session.result().mRequests.size(), 1U);

    const std::vector<RejectedAnswer> rejections =
        session.answer({{"request-0001",
                         1,
                         SplitRatioAnswer{.mNewShares = 2,
                                          .mOldShares = 1,
                                          .mEffectiveDate = day("2024-03-01"),
                                          .mConfirmedUnits = 2'000'000'000}}});

    ASSERT_EQ(rejections.size(), 1U);
    EXPECT_EQ(rejections.at(0).mReason, AnswerRejection::LaterSaleTooLarge);
}

TEST(CorporateActionIntegrationTest, AReverseSplitRowTenDaysAfterTheLossWindowIsAsked) {
    // 200 units bought at EUR 10, 100 sold at EUR 5 on 2024-12-10 (a loss; the window ends on
    // 2025-01-09), a reverse split that took effect on 2025-01-06 and was booked on 2025-01-15,
    // and 10 new units bought on 2025-01-08. The row is 6 days after the window end, so it is
    // asked; with 1 / 10 the 10 new units are 100 old ones, which block all 100 loss units.
    const TemporaryActionInputs files;
    const std::filesystem::path history = files.write(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,200,10,-2000,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-12-10T09:00:00Z,2024-12-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-100,5,500,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2025-01-08T09:00:00Z,2025-01-08,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,50,-500,,,EUR,,,,,synthetic-buy-2,,,,)",
            R"(2025-01-15T09:00:00Z,2025-01-15,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,-90,,,,,,,,,,synthetic-split-1,,,,)",
        });
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parseFile(history, 0)};
    CapitalStages stages;
    DecisionSession session;

    session.start([&parsed, &stages](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions, &stages);
    });

    ASSERT_EQ(session.result().mRequests.size(), 1U);
    EXPECT_EQ(session.result().mRequests.at(0).mType, RequestType::CorporateActionRatioRequired);

    // 100 units are held on 2025-01-06; 1 / 10 makes 10.
    const std::vector<RejectedAnswer> rejections =
        session.answer({{"request-0001",
                         1,
                         SplitRatioAnswer{.mNewShares = 1,
                                          .mOldShares = 10,
                                          .mEffectiveDate = day("2025-01-06"),
                                          .mConfirmedUnits = 1'000'000'000}}});

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(session.result().mRequests.empty());
    ASSERT_TRUE(stages.mBuilt.mReport.has_value());
    ASSERT_EQ(stages.mBuilt.mReport->mLists.size(), 1U);

    const std::vector<KdvpRow>& rows = stages.mBuilt.mReport->mLists.at(0).mRows;

    ASSERT_EQ(rows.size(), 2U);

    const KdvpSaleRow& sale = std::get<KdvpSaleRow>(rows.at(1).mEntry);

    EXPECT_EQ(sale.mUnits, 10'000'000'000); // 100 units
    EXPECT_EQ(sale.mLossRule, KdvpLossRule::Blocked);
}

TEST(CorporateActionIntegrationTest, TheRowsOfTwoAccountsAreOneIsinChangeWithTheDateOfTheAnswer) {
    // Two accounts book the change with one action ID: the old rows on 2024-03-04 (12 and 8 units
    // out), the new rows on 2024-03-06 (24 and 16 units in). One question lists the four rows,
    // and its date may be any day from 2024-03-04 to 2024-03-06. The answer 2 : 1 from 2024-03-05
    // turns the 20 units at EUR 15 into 40 units at EUR 7.50; 40 units sold at EUR 9 gain 60.
    const std::string xml = ibkrStatement(
        R"(<Trade accountId="U0000000" transactionID="2001" tradeID="2001" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYN" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" tradeDate="20230301" dateTime="20230301;100000" buySell="BUY" quantity="20" tradePrice="15" proceeds="-300" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="O" notes="" multiplier="1"/>
        <Trade accountId="U0000000" transactionID="2002" tradeID="2002" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" tradeDate="20250303" dateTime="20250303;100000" buySell="SELL" quantity="-40" tradePrice="9" proceeds="360" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="O" notes="" multiplier="1"/>)",
        R"(<CorporateAction accountId="U0000000" transactionID="5001" actionID="C9101" type="IC" assetCategory="STK" symbol="SYN" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" dateTime="20240304;202500" reportDate="20240304" quantity="-12" proceeds="0" value="0"/>
        <CorporateAction accountId="U0000001" transactionID="5002" actionID="C9101" type="IC" assetCategory="STK" symbol="SYN" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" dateTime="20240304;202500" reportDate="20240304" quantity="-8" proceeds="0" value="0"/>
        <CorporateAction accountId="U0000000" transactionID="5003" actionID="C9101" type="IC" assetCategory="STK" symbol="SYN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" dateTime="20240306;202500" reportDate="20240306" quantity="24" proceeds="0" value="0"/>
        <CorporateAction accountId="U0000001" transactionID="5004" actionID="C9101" type="IC" assetCategory="STK" symbol="SYN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" dateTime="20240306;202500" reportDate="20240306" quantity="16" proceeds="0" value="0"/>)");
    const std::vector<ParseResult> parsed{ibkr::IbkrParser{}.parse(xml, "ibkr.xml", 0)};
    CapitalStages stages;
    DecisionSession session;

    session.start([&parsed, &stages](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2025, aDecisions, &stages);
    });

    ASSERT_EQ(session.result().mRequests.size(), 1U);

    const IsinChangeQuestion& data =
        std::get<IsinChangeQuestion>(session.result().mRequests.at(0).mData);

    EXPECT_EQ(data.mSources.size(), 4U);
    EXPECT_EQ(data.mEffectiveDate, day("2024-03-04"));
    EXPECT_EQ(data.mEarliestDate, day("2024-03-04"));
    EXPECT_EQ(data.mLatestDate, day("2024-03-06"));

    const std::vector<RejectedAnswer> rejections =
        session.answer({{"request-0001",
                         1,
                         IsinChangeAnswer{.mSameIssuer = true,
                                          .mNewShares = 2,
                                          .mOldShares = 1,
                                          .mEffectiveDate = day("2024-03-05")}}});

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(session.result().mRequests.empty());
    ASSERT_TRUE(stages.mBuilt.mReport.has_value());
    ASSERT_EQ(stages.mBuilt.mReport->mLists.size(), 1U);

    const std::vector<KdvpRow>& rows = stages.mBuilt.mReport->mLists.at(0).mRows;

    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(std::get<KdvpPurchaseRow>(rows.at(0).mEntry).mUnits, 4'000'000'000);   // 40 units
    EXPECT_EQ(std::get<KdvpPurchaseRow>(rows.at(0).mEntry).mUnitPrice, 750'000'000); // EUR 7.50
    EXPECT_EQ(std::get<KdvpSaleRow>(rows.at(1).mEntry).mUnits, 4'000'000'000);

    // The warning of the answer names the rows of both accounts and the date of the answer.
    const auto warning = std::find_if(session.result().mDiagnostics.begin(),
                                      session.result().mDiagnostics.end(),
                                      [](const ApplicationDiagnostic& aDiagnostic) {
                                          return aDiagnostic.mMessageKey ==
                                                 MessageKey::CorporateActionAppliedIsinChange;
                                      });

    ASSERT_NE(warning, session.result().mDiagnostics.end());
    EXPECT_EQ(warning->mSources.size(), 4U);
}

TEST(CorporateActionIntegrationTest, ASaleBeforeAnIsinChangeStaysListedUnderTheOldIsin) {
    // The sale is of the old ISIN and the new ISIN has no trade, only its row: the new instrument
    // takes the class of the old one, so nothing about the class is asked. The sale of 2024-02-20
    // was made under the old ISIN, so it is listed under it, with the name it had.
    const std::string xml = ibkrStatement(
        R"(<Trade accountId="U0000000" transactionID="2003" tradeID="2003" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYN" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" tradeDate="20240110" dateTime="20240110;100000" buySell="BUY" quantity="10" tradePrice="10" proceeds="-100" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="O" notes="" multiplier="1"/>
        <Trade accountId="U0000000" transactionID="2004" tradeID="2004" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYN" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" tradeDate="20240220" dateTime="20240220;100000" buySell="SELL" quantity="-10" tradePrice="12" proceeds="120" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="O" notes="" multiplier="1"/>)",
        R"(<CorporateAction accountId="U0000000" transactionID="5101" actionID="C9101" type="IC" assetCategory="STK" symbol="SYN" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" dateTime="20240304;202500" reportDate="20240304" quantity="-10" proceeds="0" value="0"/>
        <CorporateAction accountId="U0000000" transactionID="5102" actionID="C9101" type="IC" assetCategory="STK" symbol="SYN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" dateTime="20240304;202500" reportDate="20240304" quantity="10" proceeds="0" value="0"/>)");
    const std::vector<ParseResult> parsed{ibkr::IbkrParser{}.parse(xml, "ibkr.xml", 0)};
    CapitalStages stages;
    DecisionSession session;

    session.start([&parsed, &stages](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions, &stages);
    });

    const std::vector<RejectedAnswer> rejections =
        session.answer({{"request-0001",
                         1,
                         IsinChangeAnswer{.mSameIssuer = true,
                                          .mNewShares = 1,
                                          .mOldShares = 1,
                                          .mEffectiveDate = day("2024-03-04")}}});

    EXPECT_TRUE(rejections.empty());
    EXPECT_TRUE(session.result().mRequests.empty());
    ASSERT_TRUE(stages.mBuilt.mReport.has_value());
    ASSERT_EQ(stages.mBuilt.mReport->mLists.size(), 1U);

    const KdvpInventoryList& list = stages.mBuilt.mReport->mLists.at(0);

    EXPECT_EQ(list.mIsin, "XX0000000134");
    EXPECT_EQ(list.mName, "OLD SYNTHETIC INC");
    ASSERT_EQ(list.mRows.size(), 2U);

    const KdvpPurchaseRow& purchase = std::get<KdvpPurchaseRow>(list.mRows.at(0).mEntry);
    const KdvpSaleRow& sale = std::get<KdvpSaleRow>(list.mRows.at(1).mEntry);

    EXPECT_EQ(purchase.mDate, day("2024-01-10"));
    EXPECT_EQ(purchase.mUnits, 1'000'000'000);     // 10 units
    EXPECT_EQ(purchase.mUnitPrice, 1'000'000'000); // EUR 10
    EXPECT_EQ(sale.mDate, day("2024-02-20"));
    EXPECT_EQ(sale.mUnits, 1'000'000'000);     // 10 units
    EXPECT_EQ(sale.mUnitPrice, 1'200'000'000); // EUR 12
}

TEST(CorporateActionIntegrationTest, SalesOnBothSidesOfAnIsinChangeAreTwoListsOfOnePool) {
    // 20 units at EUR 10 (2024-01-10). 10 are sold under the old ISIN at EUR 12 (2024-02-20), the
    // ISIN changes on 2024-03-04, and 10 are sold under the new ISIN at EUR 13 (2024-05-06). FIFO
    // is one pool: the first sale takes the first 10 units, the second the next 10 (the same
    // purchase). Each list has its own purchase row with only the units its sale uses, and its F8
    // starts from 0.
    const std::string xml = ibkrStatement(
        R"(<Trade accountId="U0000000" transactionID="2013" tradeID="2013" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYN" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" tradeDate="20240110" dateTime="20240110;100000" buySell="BUY" quantity="20" tradePrice="10" proceeds="-200" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="O" notes="" multiplier="1"/>
        <Trade accountId="U0000000" transactionID="2014" tradeID="2014" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYN" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" tradeDate="20240220" dateTime="20240220;100000" buySell="SELL" quantity="-10" tradePrice="12" proceeds="120" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="C" notes="" multiplier="1"/>
        <Trade accountId="U0000000" transactionID="2015" tradeID="2015" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" symbol="SYN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" tradeDate="20240506" dateTime="20240506;100000" buySell="SELL" quantity="-10" tradePrice="13" proceeds="130" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" openCloseIndicator="C" notes="" multiplier="1"/>)",
        R"(<CorporateAction accountId="U0000000" transactionID="5111" actionID="C9111" type="IC" assetCategory="STK" symbol="SYN" description="OLD SYNTHETIC INC" isin="XX0000000134" currency="EUR" dateTime="20240304;202500" reportDate="20240304" quantity="-10" proceeds="0" value="0"/>
        <CorporateAction accountId="U0000000" transactionID="5112" actionID="C9111" type="IC" assetCategory="STK" symbol="SYN" description="NEW SYNTHETIC INC" isin="XX0000000135" currency="EUR" dateTime="20240304;202500" reportDate="20240304" quantity="10" proceeds="0" value="0"/>)");
    const std::vector<ParseResult> parsed{ibkr::IbkrParser{}.parse(xml, "ibkr.xml", 0)};
    CapitalStages stages;
    DecisionSession session;

    session.start([&parsed, &stages](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions, &stages);
    });

    const std::vector<RejectedAnswer> rejections =
        session.answer({{"request-0001",
                         1,
                         IsinChangeAnswer{.mSameIssuer = true,
                                          .mNewShares = 1,
                                          .mOldShares = 1,
                                          .mEffectiveDate = day("2024-03-04")}}});

    EXPECT_TRUE(rejections.empty());
    ASSERT_EQ(stages.mFifo.mIsins.size(), 1U);
    ASSERT_TRUE(stages.mBuilt.mReport.has_value());
    ASSERT_EQ(stages.mBuilt.mReport->mLists.size(), 2U);

    const KdvpInventoryList& oldList = stages.mBuilt.mReport->mLists.at(0);
    const KdvpInventoryList& newList = stages.mBuilt.mReport->mLists.at(1);

    EXPECT_EQ(oldList.mIsin, "XX0000000134");
    EXPECT_EQ(oldList.mName, "OLD SYNTHETIC INC");
    EXPECT_EQ(newList.mIsin, "XX0000000135");
    EXPECT_EQ(newList.mName, "NEW SYNTHETIC INC");
    ASSERT_EQ(oldList.mRows.size(), 2U);
    ASSERT_EQ(newList.mRows.size(), 2U);

    const KdvpPurchaseRow& oldPurchase = std::get<KdvpPurchaseRow>(oldList.mRows.at(0).mEntry);
    const KdvpSaleRow& oldSale = std::get<KdvpSaleRow>(oldList.mRows.at(1).mEntry);
    const KdvpPurchaseRow& newPurchase = std::get<KdvpPurchaseRow>(newList.mRows.at(0).mEntry);
    const KdvpSaleRow& newSale = std::get<KdvpSaleRow>(newList.mRows.at(1).mEntry);

    EXPECT_EQ(oldPurchase.mDate, day("2024-01-10"));
    EXPECT_EQ(oldPurchase.mUnits, 1'000'000'000); // 10 of the 20 units
    EXPECT_EQ(oldList.mRows.at(0).mStock, 1'000'000'000);
    EXPECT_EQ(oldSale.mDate, day("2024-02-20"));
    EXPECT_EQ(oldSale.mUnitPrice, 1'200'000'000); // EUR 12
    EXPECT_EQ(oldList.mRows.at(1).mStock, 0);

    EXPECT_EQ(newPurchase.mDate, day("2024-01-10"));
    EXPECT_EQ(newPurchase.mUnits, 1'000'000'000); // the other 10 units
    EXPECT_EQ(newPurchase.mUnitPrice, 1'000'000'000);
    EXPECT_EQ(newList.mRows.at(0).mStock, 1'000'000'000);
    EXPECT_EQ(newSale.mDate, day("2024-05-06"));
    EXPECT_EQ(newSale.mUnitPrice, 1'300'000'000); // EUR 13
    EXPECT_EQ(newList.mRows.at(1).mStock, 0);
}

} // namespace
