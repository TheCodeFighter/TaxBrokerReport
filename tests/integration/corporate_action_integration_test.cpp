#include "parsers/traderepublic_parser.hpp"
#include "processors/capital_gains_report_builder.hpp"
#include "processors/capital_report_gates.hpp"
#include "processors/corporate_actions.hpp"
#include "processors/fifo_matcher.hpp"
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
    const auto& ledger = prepared.mLedger.value();
    const auto& statement = prepared.mMerged.mStatement;
    auto pools = matchCapitalPools(statement, ledger, aDecisions);
    auto& fifo = pools.mFifo;
    auto& actions = pools.mActions;
    auto loss = LossEligibilityChecker::check(statement, ledger, fifo, aDecisions);
    const auto built = CapitalGainsReportBuilder::build(statement, ledger, fifo, loss, {});
    auto gates = CapitalReportGates::check(statement, ledger, fifo, loss, built);

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
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parse(history, 0)};
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
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parse(history, 0)};
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
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parse(history, 0)};
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
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parse(history, 0)};
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
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parse(history, 0)};
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
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parse(history, 0)};
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
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parse(first, 0),
                                          tr::TradeRepublicParser{}.parse(second, 1)};
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
    // One action applied once (one warning), and the report is written.
    EXPECT_EQ(
        codesOf(session.result()),
        (std::vector<ApplicationDiagnosticCode>{ProcessingDiagnosticCode::CorporateActionApplied}));
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
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parse(history, 0)};
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

TEST(CorporateActionIntegrationTest, TwoSplitRowsOnOneDayAreAmbiguous) {
    const TemporaryActionInputs files;
    const auto history = files.write(
        "history.csv",
        {
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,3,10,-30,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-06-01T09:00:00Z,2024-06-01,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,3,,,,,,,,,,synthetic-split-1,,,,)",
            R"(2024-06-01T09:00:00Z,2024-06-01,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,3,,,,,,,,,,synthetic-split-2,,,,)",
            R"(2024-09-01T09:00:00Z,2024-09-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-1,6,6,,,EUR,,,,,synthetic-sell-1,,,,)",
        });
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parse(history, 0)};
    DecisionSession session;

    const auto& result = session.start([&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions);
    });

    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Failed);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey, MessageKey::CorporateActionAmbiguousSameDay);
    EXPECT_EQ(result.mDiagnostics.at(0).mSources.size(), 2U);
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
    const std::vector<ParseResult> before{tr::TradeRepublicParser{}.parse(history, 0)};
    const std::vector<ParseResult> after{tr::TradeRepublicParser{}.parse(history, 0),
                                         tr::TradeRepublicParser{}.parse(added, 1)};
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
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parse(history, 0)};
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
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parse(history, 0)};
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
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parse(history, 0)};
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
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parse(history, 0)};
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
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parse(history, 0)};
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
    const std::vector<ParseResult> parsed{tr::TradeRepublicParser{}.parse(history, 0)};
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

    // Only the warning that the user gave the ratio; the sale used no changed lot.
    EXPECT_TRUE(session.result().mRequests.empty());
    EXPECT_EQ(
        codesOf(session.result()),
        (std::vector<ApplicationDiagnosticCode>{ProcessingDiagnosticCode::CorporateActionApplied}));

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

} // namespace
