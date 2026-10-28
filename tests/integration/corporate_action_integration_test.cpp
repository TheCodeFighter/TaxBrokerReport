#include "parsers/traderepublic_parser.hpp"
#include "processors/capital_gains_report_builder.hpp"
#include "processors/capital_report_gates.hpp"
#include "processors/corporate_actions.hpp"
#include "processors/fifo_matcher.hpp"
#include "processors/loss_eligibility.hpp"
#include "processors/tax_processor.hpp"
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

/// The capital-gains part of one run, as the CLI runs it, from the parsed files: merge, year,
/// FIFO, the corporate actions and the loss rule with the session's answers, the report builder
/// and the gates.
ApplicationResultInput calculateCapitalGains(const std::vector<ParseResult>& aParsed,
                                             int aYear,
                                             RunDecisions& aDecisions) {
    std::vector<StatementMergeInput> inputs;

    for (std::size_t index = 0; index < aParsed.size(); ++index)
    {
        inputs.push_back({index, aParsed[index]});
    }

    auto prepared = TaxProcessor::prepareReportingYear(
        {aYear, DeterministicStatementMerger{}.merge(std::move(inputs))});
    const auto& ledger = prepared.mLedger.value();
    const auto& statement = prepared.mMerged.mStatement;
    auto fifo = FifoMatcher::match(statement, ledger);
    auto actions = CorporateActionResolver::resolve(statement, ledger, fifo, aDecisions);
    auto loss = LossEligibilityChecker::check(statement, ledger, fifo, aDecisions);
    const auto built = CapitalGainsReportBuilder::build(statement, ledger, fifo, loss, {});
    auto gates = CapitalReportGates::check(statement, ledger, fifo, loss, built);

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
    // A later date would put the same-day purchase before the split.
    EXPECT_EQ(ratioCheckAt(result, 0).mEarliestDate, day("2024-01-11"));
    EXPECT_EQ(ratioCheckAt(result, 0).mLatestDate, day("2024-06-01"));

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

TEST(CorporateActionIntegrationTest, AConfirmedRatioIsRecordedButNotAppliedYet) {
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

    // Applying the ratio to the lots comes with the next step, so the report still fails.
    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Failed);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.at(0);

    EXPECT_EQ(diagnostic.mCode,
              ApplicationDiagnosticCode{ProcessingDiagnosticCode::CorporateActionNotApplied});
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::CorporateActionNotAppliedRatioConfirmed);
    EXPECT_EQ(
        diagnostic.mMessageParams,
        (std::vector<MessageParam>{numberParam("newShares", 2), numberParam("oldShares", 1)}));
    EXPECT_FALSE(diagnostic.mRequestId.has_value());

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
    // One confirmed action, one error for it.
    EXPECT_EQ(codesOf(session.result()),
              (std::vector<ApplicationDiagnosticCode>{
                  ProcessingDiagnosticCode::CorporateActionNotApplied}));
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

    const auto rejections = session.answer({{"request-0001",
                                             1,
                                             SplitRatioAnswer{.mNewShares = 2,
                                                              .mOldShares = 1,
                                                              .mEffectiveDate = day("2024-03-01"),
                                                              .mConfirmedUnits = 600'000'000}}});

    EXPECT_TRUE(rejections.empty());

    // March is confirmed; September still waits until March is applied to the lots.
    const auto& answered = session.result();

    EXPECT_TRUE(answered.mRequests.empty());
    EXPECT_EQ(answered.report(ReportType::CapitalGains).mStatus, ReportStatus::Failed);
    ASSERT_EQ(answered.mDiagnostics.size(), 2U);
    EXPECT_EQ(answered.mDiagnostics.at(0).mMessageKey,
              MessageKey::CorporateActionNotAppliedRatioConfirmed);
    EXPECT_EQ(answered.mDiagnostics.at(1).mMessageKey,
              MessageKey::CorporateActionNotAppliedWaitingForEarlierAction);
    EXPECT_FALSE(answered.mDiagnostics.at(1).mRequestId.has_value());
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

TEST(CorporateActionIntegrationTest, NoUnitsBeforeTheSplitIsNotAsked) {
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
    EXPECT_EQ(result.mDiagnostics.at(0).mMessageKey, MessageKey::CorporateActionNotApplied);
}

TEST(CorporateActionIntegrationTest, AnAnnouncementDateBeforeAPurchaseIsRejected) {
    // The announcement says the split took effect on 2025-11-10; the broker books the row on
    // 2025-11-14. The 0.1 units bought on 2025-11-12 lie between: with the row date they count
    // before the split. Only dates from 2025-11-13 keep the position of 0.62 units, so
    // 2025-11-10 is rejected and the investment stays blocked until the user closes the question.
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
    EXPECT_EQ(ratioCheckAt(session.result(), 0).mEarliestDate, day("2025-11-13"));
    EXPECT_EQ(ratioCheckAt(session.result(), 0).mLatestDate, day("2025-12-01"));

    // 0.62 units with 10 / 1 are 6.2 units.
    const auto rejections = session.answer({{"request-0001",
                                             1,
                                             SplitRatioAnswer{.mNewShares = 10,
                                                              .mOldShares = 1,
                                                              .mEffectiveDate = day("2025-11-10"),
                                                              .mConfirmedUnits = 620'000'000}}});

    ASSERT_EQ(rejections.size(), 1U);
    EXPECT_EQ(rejections.at(0).mReason, AnswerRejection::EffectiveDateOutOfRange);
    EXPECT_EQ(rejections.at(0).mMessageParams,
              (std::vector<MessageParam>{textParam("requestId", "request-0001"),
                                         textParam("earliestDate", "2025-11-13"),
                                         textParam("latestDate", "2025-12-01")}));
    EXPECT_TRUE(session.result().mDecisions.empty());
    EXPECT_EQ(session.result().report(ReportType::CapitalGains).mStatus, ReportStatus::NeedsInput);
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
    EXPECT_EQ(ratioCheckAt(session.result(), 0).mEarliestDate, day("2025-03-02"));

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

} // namespace
