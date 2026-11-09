#include "cli/decision_prompts.hpp"
#include "taxbroker/decision_session.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace taxbroker;

Date day(int aYear, unsigned aMonth, unsigned aDay) {
    return Date{std::chrono::sys_days{std::chrono::year{aYear} / aMonth / aDay}.time_since_epoch()};
}

/// A run that asks each question until it has an answer; any answer, a cancel too, closes it.
RunCalculation asksUntilAnswered(std::vector<DecisionQuestion> aQuestions) {
    return [aQuestions = std::move(aQuestions)](RunDecisions& aDecisions) {
        ApplicationResultInput input{.mReportingYear = 2024};

        for (const auto& question : aQuestions)
        {
            if (aDecisions.find(question) == nullptr)
            {
                input.mRequests.push_back(RequestInput{.mQuestion = question});
            }
        }

        return input;
    };
}

/// Asks the open questions of aSession with aInput as what the user types; returns what is
/// printed.
std::string ask(DecisionSession& aSession, std::string_view aInput) {
    std::istringstream in{std::string{aInput}};
    std::ostringstream out;

    cli::askOpenQuestions(aSession, in, out);

    return out.str();
}

/// The printed text from the first aMarker on, so a test can skip the long question text.
std::string textFrom(const std::string& aOutput, std::string_view aMarker) {
    const auto start = aOutput.find(aMarker);

    return start == std::string::npos ? std::string{} : aOutput.substr(start);
}

bool contains(const std::string& aText, std::string_view aPart) {
    return aText.find(aPart) != std::string::npos;
}

/// How many times aPart is in aText, without overlaps.
std::size_t countOf(const std::string& aText, std::string_view aPart) {
    std::size_t count = 0;

    for (auto position = aText.find(aPart); position != std::string::npos;
         position = aText.find(aPart, position + aPart.size()))
    {
        ++count;
    }

    return count;
}

// No question

TEST(DecisionPromptsTest, ARunWithoutQuestionsPrintsNothing) {
    DecisionSession session;
    session.start(asksUntilAnswered({}));

    const auto output = ask(session, "yes\n");

    EXPECT_EQ(output, "");
    EXPECT_TRUE(session.result().mDecisions.empty());
}

// The loss question

TEST(DecisionPromptsTest, ALossQuestionListsEachSaleAndItsPurchasesAndTakesNo) {
    // The first sale has no purchase in its window. In the second one the purchases of 1 and 0.5
    // units block 1.5 of the 4 loss units, so 4 - 1.5 = 2.5 stay open.
    DecisionSession session;
    session.start(asksUntilAnswered({DecisionQuestion{
        .mType = RequestType::LossEligibilityConfirmationRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mIsin = "XX0000000001",
        .mData =
            LossEligibilityQuestion{
                .mIsin = "XX0000000001",
                .mName = "Synthetic Share",
                .mSales =
                    {
                        {
                            .mSaleDate = day(2024, 3, 10),
                            .mSaleUnits = 1'000'000'000, // 10 units at 8 decimals
                            .mLossUnits = 1'000'000'000,
                            .mWindowStart = day(2024, 2, 9),
                            .mWindowEnd = day(2024, 4, 9),
                            .mKnownReplacements = {},
                            .mKnownIneligibleUnits = 0,
                            .mOpenUnits = 1'000'000'000,
                        },
                        {
                            .mSaleDate = day(2024, 5, 20),
                            .mSaleUnits = 400'000'000, // 4 units
                            .mLossUnits = 400'000'000,
                            .mWindowStart = day(2024, 4, 20),
                            .mWindowEnd = day(2024, 6, 19),
                            .mKnownReplacements =
                                {{.mDate = day(2024, 5, 25), .mUnits = 100'000'000},
                                 {.mDate = day(2024, 6, 1), .mUnits = 50'000'000}},
                            .mKnownIneligibleUnits = 150'000'000, // 1.5 units
                            .mOpenUnits = 250'000'000,            // 2.5 units
                        },
                    },
            },
    }}));

    const auto output = ask(session, "no\n");

    EXPECT_EQ(output,
              R"(Question request-0001: the 30-day loss rule for XX0000000001 Synthetic Share
  Your files cannot decide the loss of 2 sales:
    sale 2024-03-10: 10.0000 units, 10.0000 at a loss, 10.0000 open; window 2024-02-09 to 2024-04-09, no purchase in your files counts against it
    sale 2024-05-20: 4.0000 units, 4.0000 at a loss, 2.5000 open; window 2024-04-20 to 2024-06-19, purchases in your files that count against it, in the units of this sale: 2024-05-25 1.0000 units, 2024-06-01 0.5000 units
  Within 30 days before or after the sale, did you also buy or sell this investment
  anywhere that is not in your files (another broker, another account, or after the
  export)? yes: the open loss is not deductible (F10 false). no: it is deductible.
  cancel: no answer, and the sales cannot be reported.
Your answer (yes, no or cancel):

)");
    EXPECT_TRUE(session.result().mRequests.empty());
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer, AnswerValue{NoOtherActivityAnswer{}});
}

TEST(DecisionPromptsTest, AnInvalidLossAnswerIsAskedAgainAndCaseAndSpacesDoNotMatter) {
    DecisionSession session;
    session.start(asksUntilAnswered({DecisionQuestion{
        .mType = RequestType::LossEligibilityConfirmationRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mIsin = "XX0000000001",
        .mData =
            LossEligibilityQuestion{
                .mIsin = "XX0000000001",
                .mName = "Synthetic Share",
                .mSales = {{
                    .mSaleDate = day(2024, 3, 10),
                    .mSaleUnits = 100'000'000, // 1 unit
                    .mLossUnits = 100'000'000,
                    .mWindowStart = day(2024, 2, 9),
                    .mWindowEnd = day(2024, 4, 9),
                    .mKnownReplacements = {},
                    .mKnownIneligibleUnits = 0,
                    .mOpenUnits = 100'000'000,
                }},
            },
    }}));

    // "maybe" is no answer; " YES " with a tab and a carriage return is yes.
    const auto output = ask(session, "maybe\n YES \t\r\n");

    EXPECT_TRUE(contains(output, "  Your files cannot decide the loss of 1 sale:\n"));
    EXPECT_EQ(textFrom(output, "Your answer"),
              "Your answer (yes, no or cancel):\n"
              "Answer yes, no or cancel.\n"
              "Your answer (yes, no or cancel):\n"
              "\n");
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer, AnswerValue{OtherActivityAnswer{}});
}

TEST(DecisionPromptsTest, ACancelledLossQuestionRecordsTheCancel) {
    DecisionSession session;
    session.start(asksUntilAnswered({DecisionQuestion{
        .mType = RequestType::LossEligibilityConfirmationRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mIsin = "XX0000000001",
        .mData =
            LossEligibilityQuestion{
                .mIsin = "XX0000000001",
                .mName = "Synthetic Share",
                .mSales = {{
                    .mSaleDate = day(2024, 3, 10),
                    .mSaleUnits = 100'000'000, // 1 unit
                    .mLossUnits = 100'000'000,
                    .mWindowStart = day(2024, 2, 9),
                    .mWindowEnd = day(2024, 4, 9),
                    .mKnownReplacements = {},
                    .mKnownIneligibleUnits = 0,
                    .mOpenUnits = 100'000'000,
                }},
            },
    }}));

    const auto output = ask(session, "Cancel\n");

    EXPECT_EQ(textFrom(output, "Your answer"), "Your answer (yes, no or cancel):\n\n");
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer, AnswerValue{CancelAnswer{}});
}

TEST(DecisionPromptsTest, AnInputEndingAtTheLossQuestionLeavesItOpen) {
    DecisionSession session;
    session.start(asksUntilAnswered({DecisionQuestion{
        .mType = RequestType::LossEligibilityConfirmationRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mIsin = "XX0000000001",
        .mData =
            LossEligibilityQuestion{
                .mIsin = "XX0000000001",
                .mName = "Synthetic Share",
                .mSales = {{
                    .mSaleDate = day(2024, 3, 10),
                    .mSaleUnits = 100'000'000, // 1 unit
                    .mLossUnits = 100'000'000,
                    .mWindowStart = day(2024, 2, 9),
                    .mWindowEnd = day(2024, 4, 9),
                    .mKnownReplacements = {},
                    .mKnownIneligibleUnits = 0,
                    .mOpenUnits = 100'000'000,
                }},
            },
    }}));

    const auto output = ask(session, "");

    EXPECT_EQ(textFrom(output, "Your answer"),
              "Your answer (yes, no or cancel):\n"
              "Input ended: the open questions are listed in the summary.\n"
              "\n");
    EXPECT_EQ(session.result().mRequests.size(), 1U);
    EXPECT_TRUE(session.result().mDecisions.empty());
}

// The ratio question

TEST(DecisionPromptsTest, ARatioQuestionNamesEachSourceAndTakesAConfirmedRatio) {
    // An Interactive Brokers row without a transaction ID or a share value, and a row of an
    // unknown broker with only a transaction ID. The result sorts the sources by broker, so the
    // unknown broker comes first. 1 unit x 2 / 1 = 2 units after the split.
    DecisionSession session;
    session.start(asksUntilAnswered({DecisionQuestion{
        .mType = RequestType::CorporateActionRatioRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mIsin = "XX0000000001",
        .mData =
            CorporateActionRatioQuestion{
                .mIsin = "XX0000000001",
                .mName = "Synthetic Share",
                .mEffectiveDate = day(2024, 6, 1),
                .mSources = {{.mBroker = Broker::InteractiveBrokers,
                              .mTransactionId = std::nullopt,
                              .mSourceValue = std::nullopt},
                             {.mBroker = Broker::Unknown,
                              .mTransactionId = "synthetic-split",
                              .mSourceValue = std::nullopt}},
                .mPositionBefore = 100'000'000, // 1 unit
                .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                                 .mLatestDate = day(2024, 9, 1),
                                 .mPositionBefore = 100'000'000}},
            },
    }}));

    // The ratio 2 : 1, an empty line for the broker's date, and yes to the preview.
    const auto output = ask(session, "2 : 1\n\nyes\n");

    EXPECT_EQ(
        output,
        R"(Question request-0001: a corporate action of XX0000000001 Synthetic Share on 2024-06-01
  Source: an unknown broker, transaction synthetic-split
  Source: Interactive Brokers
  You held 1.0000 units just before it.
  Enter the ratio from the announcement as new : old, for example 10 : 1 (every old share
  becomes 10 shares; "nine additional shares for every share" is also 10 : 1; a reverse
  split is 1 : 10), or your shares just before and just after it as before -> after, for
  example 0,52 -> 5,2 (a decimal comma or point), or cancel.
Ratio:
Effective date from the announcement (YYYY-MM-DD, 2024-01-11 to 2024-09-01), or empty for 2024-06-01:
After it you hold 2.0000 units (2 : 1 from 2024-06-01).
Is this right? (yes or no):

)");
    EXPECT_TRUE(session.result().mRequests.empty());
    const AnswerValue expected = SplitRatioAnswer{.mNewShares = 2,
                                                  .mOldShares = 1,
                                                  .mEffectiveDate = day(2024, 6, 1),
                                                  .mConfirmedUnits = 200'000'000};

    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer, expected);
}

TEST(DecisionPromptsTest, RowsOnTwoDatesAreConfirmedAsOneSplitBeforeTheRatio) {
    // Trade Republic on 2024-06-01, Interactive Brokers on 2024-06-05. yes: one split, then the
    // ratio 2 : 1 for it, once. 1 unit x 2 / 1 = 2 units.
    DecisionSession session;
    session.start(asksUntilAnswered({DecisionQuestion{
        .mType = RequestType::CorporateActionRatioRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mIsin = "XX0000000001",
        .mData =
            CorporateActionRatioQuestion{
                .mIsin = "XX0000000001",
                .mName = "Synthetic Share",
                .mEffectiveDate = day(2024, 6, 1),
                .mSources = {{.mBroker = Broker::TradeRepublic,
                              .mTransactionId = "synthetic-split",
                              .mDate = day(2024, 6, 1)},
                             {.mBroker = Broker::InteractiveBrokers, .mDate = day(2024, 6, 5)}},
                .mPositionBefore = 100'000'000, // 1 unit
                .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                                 .mLatestDate = day(2024, 9, 1),
                                 .mPositionBefore = 100'000'000}},
                .mAskSameEvent = true,
            },
    }}));

    const auto output = ask(session, "yes\n2 : 1\n\nyes\n");

    EXPECT_EQ(
        output,
        R"(Question request-0001: a corporate action of XX0000000001 Synthetic Share on 2024-06-01
  Source: Trade Republic, row of 2024-06-01, transaction synthetic-split
  Source: Interactive Brokers, row of 2024-06-05
  You held 1.0000 units just before it.
  Enter the ratio from the announcement as new : old, for example 10 : 1 (every old share
  becomes 10 shares; "nine additional shares for every share" is also 10 : 1; a reverse
  split is 1 : 10), or your shares just before and just after it as before -> after, for
  example 0,52 -> 5,2 (a decimal comma or point), or cancel.
  The rows are on different dates. If both brokers booked the same split, answer yes
  and enter it once. If they are two splits, answer no: two splits so close together
  are not supported yet, and the sales cannot be reported.
Are these rows one split? (yes, no or cancel):
Ratio:
Effective date from the announcement (YYYY-MM-DD, 2024-01-11 to 2024-09-01), or empty for 2024-06-01:
After it you hold 2.0000 units (2 : 1 from 2024-06-01).
Is this right? (yes or no):

)");
    const AnswerValue expected = SplitRatioAnswer{.mNewShares = 2,
                                                  .mOldShares = 1,
                                                  .mEffectiveDate = day(2024, 6, 1),
                                                  .mConfirmedUnits = 200'000'000};

    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer, expected);
    // The summary says that the two rows were confirmed as one split.
    EXPECT_EQ(cli::answerLine(session.result().mDecisions[0]),
              "XX0000000001 Synthetic Share, corporate action on 2024-06-01 (rows of 2024-06-01 "
              "and 2024-06-05): one split, 2 : 1 from 2024-06-01, 2.0000 units after it");
}

TEST(DecisionPromptsTest, RowsOnTwoDatesAnsweredAsTwoSplitsAskNoRatio) {
    // An unreadable answer is asked again; no: two splits, so no ratio is asked.
    DecisionSession session;
    session.start(asksUntilAnswered({DecisionQuestion{
        .mType = RequestType::CorporateActionRatioRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mIsin = "XX0000000001",
        .mData =
            CorporateActionRatioQuestion{
                .mIsin = "XX0000000001",
                .mName = "Synthetic Share",
                .mEffectiveDate = day(2024, 6, 1),
                .mSources = {{.mBroker = Broker::TradeRepublic,
                              .mTransactionId = "synthetic-split",
                              .mDate = day(2024, 6, 1)},
                             {.mBroker = Broker::InteractiveBrokers, .mDate = day(2024, 6, 5)}},
                .mPositionBefore = 100'000'000, // 1 unit
                .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                                 .mLatestDate = day(2024, 9, 1),
                                 .mPositionBefore = 100'000'000}},
                .mAskSameEvent = true,
            },
    }}));

    const auto output = ask(session, "maybe\nno\n");

    EXPECT_EQ(textFrom(output, "Are these rows"),
              "Are these rows one split? (yes, no or cancel):\n"
              "Answer yes, no or cancel.\n"
              "Are these rows one split? (yes, no or cancel):\n"
              "\n");

    const AnswerValue expected = SeparateEventsAnswer{};

    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer, expected);
    EXPECT_EQ(cli::answerLine(session.result().mDecisions[0]),
              "XX0000000001 Synthetic Share, corporate action on 2024-06-01 (rows of 2024-06-01 "
              "and 2024-06-05): no, the rows are two splits");
}

TEST(DecisionPromptsTest, ACancelAtTheOneSplitQuestionRecordsTheCancel) {
    DecisionSession session;
    session.start(asksUntilAnswered({DecisionQuestion{
        .mType = RequestType::CorporateActionRatioRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mIsin = "XX0000000001",
        .mData =
            CorporateActionRatioQuestion{
                .mIsin = "XX0000000001",
                .mName = "Synthetic Share",
                .mEffectiveDate = day(2024, 6, 1),
                .mSources = {{.mBroker = Broker::TradeRepublic,
                              .mTransactionId = "synthetic-split",
                              .mDate = day(2024, 6, 1)},
                             {.mBroker = Broker::InteractiveBrokers, .mDate = day(2024, 6, 5)}},
                .mPositionBefore = 100'000'000, // 1 unit
                .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                                 .mLatestDate = day(2024, 9, 1),
                                 .mPositionBefore = 100'000'000}},
                .mAskSameEvent = true,
            },
    }}));

    static_cast<void>(ask(session, "cancel\n"));

    const AnswerValue expected = CancelAnswer{};

    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer, expected);
}

TEST(DecisionPromptsTest, AnInputEndingAtTheOneSplitQuestionLeavesItOpen) {
    DecisionSession session;
    session.start(asksUntilAnswered({DecisionQuestion{
        .mType = RequestType::CorporateActionRatioRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mIsin = "XX0000000001",
        .mData =
            CorporateActionRatioQuestion{
                .mIsin = "XX0000000001",
                .mName = "Synthetic Share",
                .mEffectiveDate = day(2024, 6, 1),
                .mSources = {{.mBroker = Broker::TradeRepublic,
                              .mTransactionId = "synthetic-split",
                              .mDate = day(2024, 6, 1)},
                             {.mBroker = Broker::InteractiveBrokers, .mDate = day(2024, 6, 5)}},
                .mPositionBefore = 100'000'000, // 1 unit
                .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                                 .mLatestDate = day(2024, 9, 1),
                                 .mPositionBefore = 100'000'000}},
                .mAskSameEvent = true,
            },
    }}));

    static_cast<void>(ask(session, ""));

    EXPECT_TRUE(session.result().mDecisions.empty());
    EXPECT_EQ(session.result().mRequests.size(), 1U);
}

TEST(DecisionPromptsTest, SeveralDateRangesAreListedAndThePreviewTakesTheUnitsOfTheDate) {
    // 1 unit just before the row date 2024-06-01; 0.5 more units were bought on 2024-07-01, so
    // a date from 2024-07-02 has 1.5 units before the split. The announcement date 2024-08-01
    // gives 1.5 x 2 / 1 = 3 units.
    DecisionSession session;
    session.start(asksUntilAnswered({DecisionQuestion{
        .mType = RequestType::CorporateActionRatioRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mIsin = "XX0000000001",
        .mData =
            CorporateActionRatioQuestion{
                .mIsin = "XX0000000001",
                .mName = "Synthetic Share",
                .mEffectiveDate = day(2024, 6, 1),
                .mSources = {{.mBroker = Broker::TradeRepublic,
                              .mTransactionId = "synthetic-split",
                              .mSourceValue = "1"}},
                .mPositionBefore = 100'000'000, // 1 unit
                .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                                 .mLatestDate = day(2024, 7, 1),
                                 .mPositionBefore = 100'000'000},
                                {.mEarliestDate = day(2024, 7, 2),
                                 .mLatestDate = day(2024, 12, 31),
                                 .mPositionBefore = 150'000'000}},
            },
    }}));

    const auto output = ask(session, "2 : 1\n2024-08-01\nyes\n");

    EXPECT_EQ(textFrom(output, "Effective date"),
              "Effective date from the announcement (YYYY-MM-DD, 2024-01-11 to 2024-07-01 with "
              "1.0000 units, or 2024-07-02 to 2024-12-31 with 1.5000 units), or empty for "
              "2024-06-01:\n"
              "After it you hold 3.0000 units (2 : 1 from 2024-08-01).\n"
              "Is this right? (yes or no):\n\n");

    const AnswerValue expected = SplitRatioAnswer{.mNewShares = 2,
                                                  .mOldShares = 1,
                                                  .mEffectiveDate = day(2024, 8, 1),
                                                  .mConfirmedUnits = 300'000'000};

    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer, expected);
}

TEST(DecisionPromptsTest, AQuestionBeforeASaleNamesTheSaleAndHasNoDefaultDate) {
    // The sale of 2024-06-01 needs more units than were held, and the split row is dated
    // 2024-06-03: the row date is in no range, so an empty line is no answer. 10 units x 2 / 1 =
    // 20 units on 2024-05-30.
    DecisionSession session;
    session.start(asksUntilAnswered({DecisionQuestion{
        .mType = RequestType::CorporateActionRatioRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mIsin = "XX0000000001",
        .mData =
            CorporateActionRatioQuestion{
                .mIsin = "XX0000000001",
                .mName = "Synthetic Share",
                .mEffectiveDate = day(2024, 6, 3),
                .mSources = {{.mBroker = Broker::TradeRepublic,
                              .mTransactionId = "synthetic-split",
                              .mSourceValue = "10"}},
                .mPositionBefore = std::nullopt,
                .mEarlierSaleDate = day(2024, 6, 1),
                .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                                 .mLatestDate = day(2024, 6, 1),
                                 .mPositionBefore = 1'000'000'000,       // 10 units
                                 .mLowestChangeAfter = -2'000'000'000}}, // the sale of 20
            },
    }}));

    const auto output = ask(session, "2 : 1\n\n2024-05-30\nyes\n");

    EXPECT_EQ(
        output,
        R"(Question request-0001: a corporate action of XX0000000001 Synthetic Share on 2024-06-03
  Source: Trade Republic, transaction synthetic-split, shares 10 (the broker's own value, not a ratio)
  The sale on 2024-06-01 needs more units than you held. If the action took effect on
  or before that sale, enter it; if not, cancel: earlier purchases are then missing.
  Enter the ratio from the announcement as new : old, for example 10 : 1 (every old share
  becomes 10 shares; "nine additional shares for every share" is also 10 : 1; a reverse
  split is 1 : 10), or your shares just before and just after it as before -> after, for
  example 0,52 -> 5,2 (a decimal comma or point), or cancel.
Ratio:
Effective date from the announcement (YYYY-MM-DD, 2024-01-11 to 2024-06-01):
Enter the date as YYYY-MM-DD.
Effective date from the announcement (YYYY-MM-DD, 2024-01-11 to 2024-06-01):
After it you hold 20.0000 units (2 : 1 from 2024-05-30).
Is this right? (yes or no):

)");

    const AnswerValue expected = SplitRatioAnswer{.mNewShares = 2,
                                                  .mOldShares = 1,
                                                  .mEffectiveDate = day(2024, 5, 30),
                                                  .mConfirmedUnits = 2'000'000'000};

    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer, expected);
}

TEST(DecisionPromptsTest, ShareCountsWithAPointAndACommaGiveTheirRatio) {
    DecisionSession session;
    session.start(asksUntilAnswered({DecisionQuestion{
        .mType = RequestType::CorporateActionRatioRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mIsin = "XX0000000001",
        .mData =
            CorporateActionRatioQuestion{
                .mIsin = "XX0000000001",
                .mName = "Synthetic Share",
                .mEffectiveDate = day(2024, 6, 1),
                .mSources = {{.mBroker = Broker::TradeRepublic,
                              .mTransactionId = "synthetic-split",
                              .mSourceValue = "1"}},
                .mPositionBefore = 50'000'000, // 0.5 units
                .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                                 .mLatestDate = day(2024, 9, 1),
                                 .mPositionBefore = 50'000'000}},
            },
    }}));

    // 0.5 before and 1.5 after is 150000000 / 50000000 in ticks of 10^-8, reduced to 3 : 1.
    const auto output = ask(session, " 0.5 -> 1,5 \n\nyes\n");

    EXPECT_EQ(textFrom(output, "Ratio:"),
              "Ratio:\n"
              "Effective date from the announcement (YYYY-MM-DD, 2024-01-11 to 2024-09-01), or "
              "empty for 2024-06-01:\n"
              "After it you hold 1.5000 units (3 : 1 from 2024-06-01).\n"
              "Is this right? (yes or no):\n"
              "\n");
    const AnswerValue expected = SplitRatioAnswer{.mNewShares = 3,
                                                  .mOldShares = 1,
                                                  .mEffectiveDate = day(2024, 6, 1),
                                                  .mConfirmedUnits = 150'000'000};

    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer, expected);
}

TEST(DecisionPromptsTest, AnUnreadableRatioIsAskedAgainWithoutAnAnswer) {
    DecisionSession session;
    session.start(asksUntilAnswered({DecisionQuestion{
        .mType = RequestType::CorporateActionRatioRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mIsin = "XX0000000001",
        .mData =
            CorporateActionRatioQuestion{
                .mIsin = "XX0000000001",
                .mName = "Synthetic Share",
                .mEffectiveDate = day(2024, 6, 1),
                .mSources = {{.mBroker = Broker::TradeRepublic,
                              .mTransactionId = "synthetic-split",
                              .mSourceValue = "1"}},
                .mPositionBefore = 100'000'000, // 1 unit
                .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                                 .mLatestDate = day(2024, 9, 1),
                                 .mPositionBefore = 100'000'000}},
            },
    }}));

    // Words, a sign and decimals in a ratio; a comma and a point in one share count, a share
    // count of 0 and a word as a share count; a slash instead of a colon; then cancel.
    const auto output = ask(session,
                            "ten : 1\n"
                            "-2 : 1\n"
                            "2.5 : 1\n"
                            "1.000,5 -> 2\n"
                            "0 -> 1\n"
                            "two -> 1\n"
                            "2 / 1\n"
                            "CANCEL\n");

    EXPECT_EQ(textFrom(output, "Ratio:"),
              "Ratio:\n"
              "Enter whole numbers, for example 10 : 1.\n"
              "Ratio:\n"
              "Enter whole numbers, for example 10 : 1.\n"
              "Ratio:\n"
              "Enter whole numbers, for example 10 : 1.\n"
              "Ratio:\n"
              "Enter the shares as numbers above 0 with up to 8 decimals, for example 0,52 -> "
              "5,2.\n"
              "Ratio:\n"
              "Enter the shares as numbers above 0 with up to 8 decimals, for example 0,52 -> "
              "5,2.\n"
              "Ratio:\n"
              "Enter the shares as numbers above 0 with up to 8 decimals, for example 0,52 -> "
              "5,2.\n"
              "Ratio:\n"
              "Enter the ratio as new : old, the shares as before -> after, or cancel.\n"
              "Ratio:\n"
              "\n");
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer, AnswerValue{CancelAnswer{}});
}

TEST(DecisionPromptsTest, ARatioWithAZeroIsPassedOnAndTheSessionRejectsIt) {
    DecisionSession session;
    session.start(asksUntilAnswered({DecisionQuestion{
        .mType = RequestType::CorporateActionRatioRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mIsin = "XX0000000001",
        .mData =
            CorporateActionRatioQuestion{
                .mIsin = "XX0000000001",
                .mName = "Synthetic Share",
                .mEffectiveDate = day(2024, 6, 1),
                .mSources = {{.mBroker = Broker::TradeRepublic,
                              .mTransactionId = "synthetic-split",
                              .mSourceValue = "1"}},
                .mPositionBefore = 100'000'000, // 1 unit
                .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                                 .mLatestDate = day(2024, 9, 1),
                                 .mPositionBefore = 100'000'000}},
            },
    }}));

    const auto output = ask(session, "0 : 1\ncancel\n");

    // 0 : 1 has no preview, so neither the date nor the confirmation is asked. The rejection is
    // printed and the question is shown again after a blank line.
    EXPECT_FALSE(contains(output, "Effective date"));
    EXPECT_TRUE(contains(output,
                         "Ratio:\n"
                         "The number in newShares of the answer to request-0001 must be a whole "
                         "number above 0.\n"
                         "\n"
                         "Question request-0001: a corporate action of XX0000000001 Synthetic "
                         "Share on 2024-06-01\n"));
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer, AnswerValue{CancelAnswer{}});
}

TEST(DecisionPromptsTest, ARatioWhosePositionDoesNotFitIsPassedOnAndTheSessionRejectsIt) {
    DecisionSession session;
    session.start(asksUntilAnswered({DecisionQuestion{
        .mType = RequestType::CorporateActionRatioRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mIsin = "XX0000000001",
        .mData =
            CorporateActionRatioQuestion{
                .mIsin = "XX0000000001",
                .mName = "Synthetic Share",
                .mEffectiveDate = day(2024, 6, 1),
                .mSources = {{.mBroker = Broker::TradeRepublic,
                              .mTransactionId = "synthetic-split",
                              .mSourceValue = "1"}},
                .mPositionBefore = 1, // 0.00000001 units
                .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                                 .mLatestDate = day(2024, 9, 1),
                                 .mPositionBefore = 1}},
            },
    }}));

    // 0.00000001 x 1 / 3 rounds to 0 units, so there is no preview to confirm.
    const auto output = ask(session, "1 : 3\n\ncancel\n");

    EXPECT_FALSE(contains(output, "Is this right?"));
    EXPECT_TRUE(contains(output, "  You held 0.00000001 units just before it.\n"));
    EXPECT_TRUE(contains(output,
                         "or empty for 2024-06-01:\n"
                         "The ratio in the answer to request-0001 gives a position that cannot be "
                         "represented. Check the numbers.\n"
                         "\n"
                         "Question request-0001:"));
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer, AnswerValue{CancelAnswer{}});
}

TEST(DecisionPromptsTest, AnUnreadableEffectiveDateIsAskedAgain) {
    DecisionSession session;
    session.start(asksUntilAnswered({DecisionQuestion{
        .mType = RequestType::CorporateActionRatioRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mIsin = "XX0000000001",
        .mData =
            CorporateActionRatioQuestion{
                .mIsin = "XX0000000001",
                .mName = "Synthetic Share",
                .mEffectiveDate = day(2024, 6, 1),
                .mSources = {{.mBroker = Broker::TradeRepublic,
                              .mTransactionId = "synthetic-split",
                              .mSourceValue = "1"}},
                .mPositionBefore = 100'000'000, // 1 unit
                .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                                 .mLatestDate = day(2024, 9, 1),
                                 .mPositionBefore = 100'000'000}},
            },
    }}));

    // Month 13 is no date; 2024-07-01 with spaces around it is taken instead of the broker's date.
    const auto output = ask(session, "2 : 1\n2024-13-01\n 2024-07-01 \nyes\n");

    EXPECT_EQ(textFrom(output, "Ratio:"),
              "Ratio:\n"
              "Effective date from the announcement (YYYY-MM-DD, 2024-01-11 to 2024-09-01), or "
              "empty for 2024-06-01:\n"
              "Enter the date as YYYY-MM-DD.\n"
              "Effective date from the announcement (YYYY-MM-DD, 2024-01-11 to 2024-09-01), or "
              "empty for 2024-06-01:\n"
              "After it you hold 2.0000 units (2 : 1 from 2024-07-01).\n"
              "Is this right? (yes or no):\n"
              "\n");
    const AnswerValue expected = SplitRatioAnswer{.mNewShares = 2,
                                                  .mOldShares = 1,
                                                  .mEffectiveDate = day(2024, 7, 1),
                                                  .mConfirmedUnits = 200'000'000};

    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer, expected);
}

TEST(DecisionPromptsTest, ARatioNotConfirmedIsAskedAgain) {
    DecisionSession session;
    session.start(asksUntilAnswered({DecisionQuestion{
        .mType = RequestType::CorporateActionRatioRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mIsin = "XX0000000001",
        .mData =
            CorporateActionRatioQuestion{
                .mIsin = "XX0000000001",
                .mName = "Synthetic Share",
                .mEffectiveDate = day(2024, 6, 1),
                .mSources = {{.mBroker = Broker::TradeRepublic,
                              .mTransactionId = "synthetic-split",
                              .mSourceValue = "1"}},
                .mPositionBefore = 100'000'000, // 1 unit
                .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                                 .mLatestDate = day(2024, 9, 1),
                                 .mPositionBefore = 100'000'000}},
            },
    }}));

    // "maybe" is no answer to the preview; No goes back to the ratio, and 3 : 1 is confirmed.
    const auto output = ask(session, "2 : 1\n\nmaybe\nNo\n3 : 1\n\nyes\n");

    EXPECT_EQ(textFrom(output, "Ratio:"),
              "Ratio:\n"
              "Effective date from the announcement (YYYY-MM-DD, 2024-01-11 to 2024-09-01), or "
              "empty for 2024-06-01:\n"
              "After it you hold 2.0000 units (2 : 1 from 2024-06-01).\n"
              "Is this right? (yes or no):\n"
              "Answer yes or no.\n"
              "Is this right? (yes or no):\n"
              "Ratio:\n"
              "Effective date from the announcement (YYYY-MM-DD, 2024-01-11 to 2024-09-01), or "
              "empty for 2024-06-01:\n"
              "After it you hold 3.0000 units (3 : 1 from 2024-06-01).\n"
              "Is this right? (yes or no):\n"
              "\n");
    const AnswerValue expected = SplitRatioAnswer{.mNewShares = 3,
                                                  .mOldShares = 1,
                                                  .mEffectiveDate = day(2024, 6, 1),
                                                  .mConfirmedUnits = 300'000'000};

    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer, expected);
}

TEST(DecisionPromptsTest, AnUnusualRatioIsWarnedAboutBeforeThePreview) {
    DecisionSession session;
    session.start(asksUntilAnswered({DecisionQuestion{
        .mType = RequestType::CorporateActionRatioRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mIsin = "XX0000000001",
        .mData =
            CorporateActionRatioQuestion{
                .mIsin = "XX0000000001",
                .mName = "Synthetic Share",
                .mEffectiveDate = day(2024, 6, 1),
                .mSources = {{.mBroker = Broker::TradeRepublic,
                              .mTransactionId = "synthetic-split",
                              .mSourceValue = "1"}},
                .mPositionBefore = 100'000'000, // 1 unit
                .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                                 .mLatestDate = day(2024, 9, 1),
                                 .mPositionBefore = 100'000'000}},
            },
    }}));

    // 1001 is above the limit of 1000; 1 unit x 1001 / 1 = 1001 units.
    const auto output = ask(session, "1001 : 1\n\nyes\n");

    EXPECT_TRUE(contains(output,
                         "This ratio looks unusual: check the numbers.\n"
                         "After it you hold 1001.0000 units (1001 : 1 from 2024-06-01).\n"));
    const AnswerValue expected = SplitRatioAnswer{.mNewShares = 1001,
                                                  .mOldShares = 1,
                                                  .mEffectiveDate = day(2024, 6, 1),
                                                  .mConfirmedUnits = 100'100'000'000};

    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer, expected);
}

TEST(DecisionPromptsTest, AnInputEndingAtTheRatioLeavesTheQuestionOpen) {
    DecisionSession session;
    session.start(asksUntilAnswered({DecisionQuestion{
        .mType = RequestType::CorporateActionRatioRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mIsin = "XX0000000001",
        .mData =
            CorporateActionRatioQuestion{
                .mIsin = "XX0000000001",
                .mName = "Synthetic Share",
                .mEffectiveDate = day(2024, 6, 1),
                .mSources = {{.mBroker = Broker::TradeRepublic,
                              .mTransactionId = "synthetic-split",
                              .mSourceValue = "1"}},
                .mPositionBefore = 100'000'000, // 1 unit
                .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                                 .mLatestDate = day(2024, 9, 1),
                                 .mPositionBefore = 100'000'000}},
            },
    }}));

    const auto output = ask(session, "");

    EXPECT_EQ(textFrom(output, "Ratio:"),
              "Ratio:\n"
              "Input ended: the open questions are listed in the summary.\n"
              "\n");
    EXPECT_EQ(session.result().mRequests.size(), 1U);
    EXPECT_TRUE(session.result().mDecisions.empty());
}

TEST(DecisionPromptsTest, AnInputEndingAtTheEffectiveDateLeavesTheQuestionOpen) {
    DecisionSession session;
    session.start(asksUntilAnswered({DecisionQuestion{
        .mType = RequestType::CorporateActionRatioRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mIsin = "XX0000000001",
        .mData =
            CorporateActionRatioQuestion{
                .mIsin = "XX0000000001",
                .mName = "Synthetic Share",
                .mEffectiveDate = day(2024, 6, 1),
                .mSources = {{.mBroker = Broker::TradeRepublic,
                              .mTransactionId = "synthetic-split",
                              .mSourceValue = "1"}},
                .mPositionBefore = 100'000'000, // 1 unit
                .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                                 .mLatestDate = day(2024, 9, 1),
                                 .mPositionBefore = 100'000'000}},
            },
    }}));

    const auto output = ask(session, "2 : 1\n");

    EXPECT_EQ(textFrom(output, "Ratio:"),
              "Ratio:\n"
              "Effective date from the announcement (YYYY-MM-DD, 2024-01-11 to 2024-09-01), or "
              "empty for 2024-06-01:\n"
              "Input ended: the open questions are listed in the summary.\n"
              "\n");
    EXPECT_EQ(session.result().mRequests.size(), 1U);
    EXPECT_TRUE(session.result().mDecisions.empty());
}

TEST(DecisionPromptsTest, AnInputEndingAtTheConfirmationLeavesTheQuestionOpen) {
    DecisionSession session;
    session.start(asksUntilAnswered({DecisionQuestion{
        .mType = RequestType::CorporateActionRatioRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mIsin = "XX0000000001",
        .mData =
            CorporateActionRatioQuestion{
                .mIsin = "XX0000000001",
                .mName = "Synthetic Share",
                .mEffectiveDate = day(2024, 6, 1),
                .mSources = {{.mBroker = Broker::TradeRepublic,
                              .mTransactionId = "synthetic-split",
                              .mSourceValue = "1"}},
                .mPositionBefore = 100'000'000, // 1 unit
                .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                                 .mLatestDate = day(2024, 9, 1),
                                 .mPositionBefore = 100'000'000}},
            },
    }}));

    const auto output = ask(session, "2 : 1\n\n");

    EXPECT_EQ(textFrom(output, "Ratio:"),
              "Ratio:\n"
              "Effective date from the announcement (YYYY-MM-DD, 2024-01-11 to 2024-09-01), or "
              "empty for 2024-06-01:\n"
              "After it you hold 2.0000 units (2 : 1 from 2024-06-01).\n"
              "Is this right? (yes or no):\n"
              "Input ended: the open questions are listed in the summary.\n"
              "\n");
    EXPECT_EQ(session.result().mRequests.size(), 1U);
    EXPECT_TRUE(session.result().mDecisions.empty());
}

// Two questions

TEST(DecisionPromptsTest, TwoQuestionsAreAskedInRequestOrderWithABlankLineBetween) {
    DecisionSession session;
    session.start(asksUntilAnswered({
        DecisionQuestion{
            .mType = RequestType::LossEligibilityConfirmationRequired,
            .mAffectedReports = {ReportType::CapitalGains},
            .mIsin = "XX0000000001",
            .mData =
                LossEligibilityQuestion{
                    .mIsin = "XX0000000001",
                    .mName = "Synthetic Share",
                    .mSales = {{
                        .mSaleDate = day(2024, 3, 10),
                        .mSaleUnits = 100'000'000, // 1 unit
                        .mLossUnits = 100'000'000,
                        .mWindowStart = day(2024, 2, 9),
                        .mWindowEnd = day(2024, 4, 9),
                        .mKnownReplacements = {},
                        .mKnownIneligibleUnits = 0,
                        .mOpenUnits = 100'000'000,
                    }},
                },
        },
        DecisionQuestion{
            .mType = RequestType::CorporateActionRatioRequired,
            .mAffectedReports = {ReportType::CapitalGains},
            .mIsin = "XX0000000002",
            .mData =
                CorporateActionRatioQuestion{
                    .mIsin = "XX0000000002",
                    .mName = "Synthetic Fund",
                    .mEffectiveDate = day(2024, 6, 1),
                    .mSources = {{.mBroker = Broker::TradeRepublic,
                                  .mTransactionId = "synthetic-split",
                                  .mSourceValue = "1"}},
                    .mPositionBefore = 100'000'000, // 1 unit
                    .mDateRanges = {{.mEarliestDate = day(2024, 1, 11),
                                     .mLatestDate = day(2024, 9, 1),
                                     .mPositionBefore = 100'000'000}},
                },
        },
    }));

    const auto output = ask(session, "no\ncancel\n");

    // The answer to the first question gives a new revision, so the ratio question is again the
    // first request.
    EXPECT_TRUE(output.starts_with(
        "Question request-0001: the 30-day loss rule for XX0000000001 Synthetic Share\n"));
    EXPECT_TRUE(contains(output,
                         "Your answer (yes, no or cancel):\n"
                         "\n"
                         "Question request-0001: a corporate action of XX0000000002 Synthetic "
                         "Fund on 2024-06-01\n"));
    EXPECT_TRUE(output.ends_with("Ratio:\n\n"));
    EXPECT_TRUE(session.result().mRequests.empty());
    ASSERT_EQ(session.result().mDecisions.size(), 2U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer, AnswerValue{NoOtherActivityAnswer{}});
    EXPECT_EQ(session.result().mDecisions[1].mAnswer, AnswerValue{CancelAnswer{}});
}

// The failure question

TEST(DecisionPromptsTest, TheFailureQuestionListsEveryInvestmentAndTakesExclude) {
    DecisionSession session;
    session.start(asksUntilAnswered({DecisionQuestion{
        .mType = RequestType::CapitalGainsFailureResolutionRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mData =
            FailureResolutionQuestion{
                .mInvestments =
                    {
                        {.mIsin = "XX0000000002",
                         .mName = "Synthetic Other",
                         .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory}},
                        {.mIsin = "XX0000000005",
                         .mName = "Synthetic Coin",
                         .mReasonCodes = {ProcessingDiagnosticCode::UnsupportedTaxTreatment,
                                          ProcessingDiagnosticCode::MissingExchangeRate}},
                    },
            },
    }}));

    const auto output = ask(session, "exclude\n");

    EXPECT_EQ(output,
              R"(Question request-0001: Doh-KDVP cannot include 2 investments:
  XX0000000002 Synthetic Other: incomplete_history
  XX0000000005 Synthetic Coin: unsupported_tax_treatment, missing_exchange_rate
  exclude: write Doh-KDVP without every investment listed here; they are not in the
  XML, and the summary lists them. fail: write no Doh-KDVP. cancel: no answer, and no
  Doh-KDVP.
Your answer (exclude, fail or cancel):

)");
    EXPECT_TRUE(session.result().mRequests.empty());
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer,
              AnswerValue{FailureResolutionAnswer{FailureResolution::ExcludeFailedInstruments}});
}

TEST(DecisionPromptsTest, TheFailureQuestionOfDohDivNamesDohDiv) {
    DecisionSession session;
    session.start(asksUntilAnswered({DecisionQuestion{
        .mType = RequestType::DividendsFailureResolutionRequired,
        .mAffectedReports = {ReportType::Dividends},
        .mData =
            FailureResolutionQuestion{
                .mInvestments =
                    {{.mIsin = "XX0000000010",
                      .mName = "Synthetic Payer A",
                      .mReasonCodes = {ProcessingDiagnosticCode::MissingExchangeRate}}}},
    }}));

    EXPECT_EQ(ask(session, "cancel\n"),
              R"(Question request-0001: Doh-Div cannot include 1 investment:
  XX0000000010 Synthetic Payer A: missing_exchange_rate
  exclude: write Doh-Div without every investment listed here; they are not in the
  XML, and the summary lists them. fail: write no Doh-Div. cancel: no answer, and no
  Doh-Div.
Your answer (exclude, fail or cancel):

)");
}

TEST(DecisionPromptsTest, TheFailureQuestionOfDohObrNamesDohObr) {
    DecisionSession session;
    session.start(asksUntilAnswered({DecisionQuestion{
        .mType = RequestType::OtherInterestFailureResolutionRequired,
        .mAffectedReports = {ReportType::OtherInterest},
        .mData =
            FailureResolutionQuestion{
                .mInvestments =
                    {{.mIsin = "XX0000000010",
                      .mName = "Synthetic Bond A",
                      .mReasonCodes = {ProcessingDiagnosticCode::MissingExchangeRate}}}},
    }}));

    EXPECT_EQ(ask(session, "cancel\n"),
              R"(Question request-0001: Doh-Obr cannot include 1 investment:
  XX0000000010 Synthetic Bond A: missing_exchange_rate
  exclude: write Doh-Obr without every investment listed here; they are not in the
  XML, and the summary lists them. fail: write no Doh-Obr. cancel: no answer, and no
  Doh-Obr.
Your answer (exclude, fail or cancel):

)");
}

TEST(DecisionPromptsTest, TheFailureQuestionOfOneInvestmentTakesFail) {
    DecisionSession session;
    session.start(asksUntilAnswered({DecisionQuestion{
        .mType = RequestType::CapitalGainsFailureResolutionRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mData =
            FailureResolutionQuestion{
                .mInvestments = {{.mIsin = "XX0000000002",
                                  .mName = "Synthetic Other",
                                  .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory}}},
            },
    }}));

    const auto output = ask(session, "fail\n");

    EXPECT_TRUE(output.starts_with("Question request-0001: Doh-KDVP cannot include 1 investment:\n"
                                   "  XX0000000002 Synthetic Other: incomplete_history\n"));
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer,
              AnswerValue{FailureResolutionAnswer{FailureResolution::FailReport}});
}

TEST(DecisionPromptsTest, AnInvalidFailureAnswerIsAskedAgainAndCaseAndSpacesDoNotMatter) {
    DecisionSession session;
    session.start(asksUntilAnswered({DecisionQuestion{
        .mType = RequestType::CapitalGainsFailureResolutionRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mData =
            FailureResolutionQuestion{
                .mInvestments =
                    {
                        {.mIsin = "XX0000000002",
                         .mName = "Synthetic Other",
                         .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory}},
                        {.mIsin = "XX0000000005",
                         .mName = "Synthetic Coin",
                         .mReasonCodes = {ProcessingDiagnosticCode::UnsupportedTaxTreatment,
                                          ProcessingDiagnosticCode::MissingExchangeRate}},
                    },
            },
    }}));

    const auto output = ask(session, "yes\n\n  EXCLUDE  \n");

    EXPECT_TRUE(output.ends_with("Your answer (exclude, fail or cancel):\n"
                                 "Answer exclude, fail or cancel.\n"
                                 "Your answer (exclude, fail or cancel):\n"
                                 "Answer exclude, fail or cancel.\n"
                                 "Your answer (exclude, fail or cancel):\n"
                                 "\n"));
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer,
              AnswerValue{FailureResolutionAnswer{FailureResolution::ExcludeFailedInstruments}});
}

TEST(DecisionPromptsTest, ACancelledFailureQuestionRecordsTheCancel) {
    DecisionSession session;
    session.start(asksUntilAnswered({DecisionQuestion{
        .mType = RequestType::CapitalGainsFailureResolutionRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mData =
            FailureResolutionQuestion{
                .mInvestments =
                    {
                        {.mIsin = "XX0000000002",
                         .mName = "Synthetic Other",
                         .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory}},
                        {.mIsin = "XX0000000005",
                         .mName = "Synthetic Coin",
                         .mReasonCodes = {ProcessingDiagnosticCode::UnsupportedTaxTreatment,
                                          ProcessingDiagnosticCode::MissingExchangeRate}},
                    },
            },
    }}));

    ask(session, "cancel\n");

    EXPECT_TRUE(session.result().mRequests.empty());
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer, AnswerValue{CancelAnswer{}});
}

TEST(DecisionPromptsTest, AnInputEndingAtTheFailureQuestionLeavesItOpen) {
    DecisionSession session;
    session.start(asksUntilAnswered({DecisionQuestion{
        .mType = RequestType::CapitalGainsFailureResolutionRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mData =
            FailureResolutionQuestion{
                .mInvestments =
                    {
                        {.mIsin = "XX0000000002",
                         .mName = "Synthetic Other",
                         .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory}},
                        {.mIsin = "XX0000000005",
                         .mName = "Synthetic Coin",
                         .mReasonCodes = {ProcessingDiagnosticCode::UnsupportedTaxTreatment,
                                          ProcessingDiagnosticCode::MissingExchangeRate}},
                    },
            },
    }}));

    const auto output = ask(session, "");

    EXPECT_TRUE(output.ends_with("Your answer (exclude, fail or cancel):\n"
                                 "Input ended: the open questions are listed in the summary.\n"
                                 "\n"));
    ASSERT_EQ(session.result().mRequests.size(), 1U);
    EXPECT_TRUE(session.result().mDecisions.empty());
}

TEST(DecisionPromptsTest, TheFailureQuestionIsAskedAfterTheQuestionsOfOtherInvestments) {
    // The failure question lists XX0000000002 and XX0000000005. The loss question is of
    // XX0000000001, which is not listed, and an answer to it (a cancel) could add that investment
    // to the list, so it comes first although the failure question has the earlier date.
    DecisionSession session;
    session.start(asksUntilAnswered(
        {DecisionQuestion{
             .mType = RequestType::CapitalGainsFailureResolutionRequired,
             .mAffectedReports = {ReportType::CapitalGains},
             .mTaxDate = day(2024, 1, 1),
             .mData =
                 FailureResolutionQuestion{
                     .mInvestments =
                         {
                             {.mIsin = "XX0000000002",
                              .mName = "Synthetic Other",
                              .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory}},
                             {.mIsin = "XX0000000005",
                              .mName = "Synthetic Coin",
                              .mReasonCodes = {ProcessingDiagnosticCode::UnsupportedTaxTreatment,
                                               ProcessingDiagnosticCode::MissingExchangeRate}},
                         },
                 },
         },
         DecisionQuestion{
             .mType = RequestType::LossEligibilityConfirmationRequired,
             .mAffectedReports = {ReportType::CapitalGains},
             .mTaxDate = day(2024, 3, 10),
             .mIsin = "XX0000000001",
             .mData =
                 LossEligibilityQuestion{
                     .mIsin = "XX0000000001",
                     .mName = "Synthetic Share",
                     .mSales = {{
                         .mSaleDate = day(2024, 3, 10),
                         .mSaleUnits = 100'000'000, // 1 unit
                         .mLossUnits = 100'000'000,
                         .mWindowStart = day(2024, 2, 9),
                         .mWindowEnd = day(2024, 4, 9),
                         .mKnownReplacements = {},
                         .mKnownIneligibleUnits = 0,
                         .mOpenUnits = 100'000'000,
                     }},
                 },
         }}));

    ASSERT_EQ(session.result().mRequests.size(), 2U);
    EXPECT_EQ(session.result().mRequests[0].mType,
              RequestType::CapitalGainsFailureResolutionRequired);

    const auto output = ask(session, "no\nexclude\n");

    EXPECT_TRUE(output.starts_with(
        "Question request-0002: the 30-day loss rule for XX0000000001 Synthetic Share\n"));
    EXPECT_TRUE(contains(output,
                         "Your answer (yes, no or cancel):\n"
                         "\n"
                         "Question request-0001: Doh-KDVP cannot include 2 investments:\n"));
    EXPECT_TRUE(session.result().mRequests.empty());
    ASSERT_EQ(session.result().mDecisions.size(), 2U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer, AnswerValue{NoOtherActivityAnswer{}});
    EXPECT_EQ(session.result().mDecisions[1].mAnswer,
              AnswerValue{FailureResolutionAnswer{FailureResolution::ExcludeFailedInstruments}});
}

TEST(DecisionPromptsTest, ALossQuestionOfAListedInvestmentIsAskedAfterTheFailureQuestion) {
    // The loss question is of XX0000000005, which the failure question lists. The failure
    // question comes first although its date is later, because excluding XX0000000005 would make
    // the loss answer useless. Here the answer is fail, so the loss question is still asked.
    DecisionSession session;
    session.start(asksUntilAnswered(
        {DecisionQuestion{
             .mType = RequestType::CapitalGainsFailureResolutionRequired,
             .mAffectedReports = {ReportType::CapitalGains},
             .mTaxDate = day(2024, 6, 1),
             .mData =
                 FailureResolutionQuestion{
                     .mInvestments =
                         {
                             {.mIsin = "XX0000000002",
                              .mName = "Synthetic Other",
                              .mReasonCodes = {ProcessingDiagnosticCode::IncompleteHistory}},
                             {.mIsin = "XX0000000005",
                              .mName = "Synthetic Coin",
                              .mReasonCodes = {ProcessingDiagnosticCode::UnsupportedTaxTreatment,
                                               ProcessingDiagnosticCode::MissingExchangeRate}},
                         },
                 },
         },
         DecisionQuestion{
             .mType = RequestType::LossEligibilityConfirmationRequired,
             .mAffectedReports = {ReportType::CapitalGains},
             .mTaxDate = day(2024, 3, 10),
             .mIsin = "XX0000000005",
             .mData =
                 LossEligibilityQuestion{
                     .mIsin = "XX0000000005",
                     .mName = "Synthetic Coin",
                     .mSales = {{
                         .mSaleDate = day(2024, 3, 10),
                         .mSaleUnits = 100'000'000, // 1 unit
                         .mLossUnits = 100'000'000,
                         .mWindowStart = day(2024, 2, 9),
                         .mWindowEnd = day(2024, 4, 9),
                         .mKnownReplacements = {},
                         .mKnownIneligibleUnits = 0,
                         .mOpenUnits = 100'000'000,
                     }},
                 },
         }}));

    ASSERT_EQ(session.result().mRequests.size(), 2U);
    EXPECT_EQ(session.result().mRequests[0].mType,
              RequestType::LossEligibilityConfirmationRequired);

    const auto output = ask(session, "fail\nno\n");

    EXPECT_TRUE(
        output.starts_with("Question request-0002: Doh-KDVP cannot include 2 investments:\n"));
    EXPECT_TRUE(
        contains(output,
                 "Your answer (exclude, fail or cancel):\n"
                 "\n"
                 "Question request-0001: the 30-day loss rule for XX0000000005 Synthetic Coin\n"));
    EXPECT_TRUE(session.result().mRequests.empty());
    // The decisions are listed by request type, not in the order they were answered.
    ASSERT_EQ(session.result().mDecisions.size(), 2U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer,
              AnswerValue{FailureResolutionAnswer{FailureResolution::FailReport}});
    EXPECT_EQ(session.result().mDecisions[1].mAnswer, AnswerValue{NoOtherActivityAnswer{}});
}

// The classification question

DecisionQuestion classificationQuestion(const Isin& aIsin,
                                        const std::string& aName,
                                        std::vector<std::string> aLabels) {
    return DecisionQuestion{
        .mType = RequestType::TaxClassificationRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mIsin = aIsin,
        .mData = TaxClassificationQuestion{.mIsin = aIsin,
                                           .mName = aName,
                                           .mClassLabels = std::move(aLabels)},
    };
}

TEST(DecisionPromptsTest, AClassificationQuestionListsTheLabelsAndTakesCapital) {
    DecisionSession session;
    session.start(asksUntilAnswered(
        {classificationQuestion("XX0000000001", "Synthetic Share", {"", "FUND", "STOCK"})}));

    const auto output = ask(session, "capital\n");

    EXPECT_EQ(output,
              "Question request-0001: the asset class of XX0000000001 Synthetic Share is "
              "missing, not recognized or differs between rows (class in the files: (blank), "
              "FUND, STOCK).\n"
              "  capital: report it as shares or fund units in Doh-KDVP.\n"
              "  exclude: leave it out of the reports; the summary lists it, and you report it\n"
              "  yourself if it is taxable. cancel: no answer, and no Doh-KDVP.\n"
              "Your answer (capital, exclude or cancel):\n"
              "\n");
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer, AnswerValue{CapitalGainsAnswer{}});
}

TEST(DecisionPromptsTest, AnInvalidClassificationAnswerIsAskedAgainAndExcludeIsRecorded) {
    DecisionSession session;
    session.start(asksUntilAnswered(
        {classificationQuestion("XX0000000001", "Synthetic Share", {"MYSTERY"})}));

    // "shares" is no answer; " EXCLUDE " with a tab and a carriage return is exclude.
    const auto output = ask(session, "shares\n EXCLUDE \t\r\n");

    EXPECT_EQ(textFrom(output, "Your answer"),
              "Your answer (capital, exclude or cancel):\n"
              "Answer capital, exclude or cancel.\n"
              "Your answer (capital, exclude or cancel):\n"
              "\n");
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer, AnswerValue{ExcludeFromReportsAnswer{}});
}

TEST(DecisionPromptsTest, ACancelledClassificationQuestionRecordsTheCancel) {
    DecisionSession session;
    session.start(
        asksUntilAnswered({classificationQuestion("XX0000000001", "Synthetic Share", {""})}));

    (void)ask(session, "cancel\n");

    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer, AnswerValue{CancelAnswer{}});
}

TEST(DecisionPromptsTest, AnInputEndingAtTheClassificationQuestionLeavesItOpen) {
    DecisionSession session;
    session.start(
        asksUntilAnswered({classificationQuestion("XX0000000001", "Synthetic Share", {""})}));

    const auto output = ask(session, "");

    EXPECT_TRUE(contains(output, "Input ended: the open questions are listed in the summary.\n"));
    EXPECT_EQ(session.result().mRequests.size(), 1U);
    EXPECT_TRUE(session.result().mDecisions.empty());
}

TEST(DecisionPromptsTest, AClassificationQuestionIsAskedBeforeALossQuestion) {
    // The exclusion of an investment drops its other questions, so the class is asked first even
    // though the loss question comes first in request order (it has the earlier tax date).
    DecisionSession session;
    session.start(asksUntilAnswered({
        DecisionQuestion{
            .mType = RequestType::LossEligibilityConfirmationRequired,
            .mAffectedReports = {ReportType::CapitalGains},
            .mTaxDate = day(2024, 1, 1),
            .mIsin = "XX0000000001",
            .mData =
                LossEligibilityQuestion{
                    .mIsin = "XX0000000001",
                    .mName = "Synthetic Share",
                    .mSales = {{
                        .mSaleDate = day(2024, 3, 10),
                        .mSaleUnits = 100'000'000, // 1 unit
                        .mLossUnits = 100'000'000,
                        .mWindowStart = day(2024, 2, 9),
                        .mWindowEnd = day(2024, 4, 9),
                        .mKnownReplacements = {},
                        .mKnownIneligibleUnits = 0,
                        .mOpenUnits = 100'000'000,
                    }},
                },
        },
        classificationQuestion("XX0000000002", "Synthetic Mystery", {""}),
    }));

    ASSERT_EQ(session.result().mRequests.size(), 2U);
    EXPECT_EQ(session.result().mRequests[0].mType,
              RequestType::LossEligibilityConfirmationRequired);

    const auto output = ask(session, "exclude\nno\n");

    EXPECT_TRUE(output.starts_with("Question request-0002: the asset class of XX0000000002 "));
    EXPECT_TRUE(contains(output, "Question request-0001: the 30-day loss rule for XX0000000001"));
    ASSERT_EQ(session.result().mDecisions.size(), 2U);
}

TEST(DecisionPromptsTest, TheAnswersOfAClassificationQuestionAreListedInTheSummary) {
    DecisionSession session;
    session.start(asksUntilAnswered({
        classificationQuestion("XX0000000001", "Synthetic Share", {""}),
        classificationQuestion("XX0000000002", "Synthetic Other", {""}),
        classificationQuestion("XX0000000003", "Synthetic Third", {""}),
    }));

    (void)ask(session, "capital\nexclude\ncancel\n");

    ASSERT_EQ(session.result().mDecisions.size(), 3U);
    EXPECT_EQ(cli::answerLine(session.result().mDecisions[0]),
              "XX0000000001 Synthetic Share, asset class: report it as shares or fund units "
              "(Doh-KDVP)");
    EXPECT_EQ(cli::answerLine(session.result().mDecisions[1]),
              "XX0000000002 Synthetic Other, asset class: leave it out of the reports");
    EXPECT_EQ(cli::answerLine(session.result().mDecisions[2]),
              "XX0000000003 Synthetic Third, asset class: cancelled");
}

// The quantity question

/// A sale of 1 unit on 1 March 2024 that the earlier purchases cover with 0.99999999 units.
DecisionQuestion quantityQuestion() {
    return DecisionQuestion{
        .mType = RequestType::QuantityCorrectionRequired,
        .mAffectedReports = {ReportType::CapitalGains},
        .mTaxDate = day(2024, 3, 1),
        .mIsin = "XX0000000001",
        .mData = QuantityCorrectionQuestion{.mIsin = "XX0000000001",
                                            .mName = "Synthetic Share",
                                            .mSaleDate = day(2024, 3, 1),
                                            .mOriginalUnits = 100'000'000,
                                            .mOpenUnits = 99'999'999,
                                            .mUnitValue = ExactRational{WideInteger{36}}},
    };
}

TEST(DecisionPromptsTest, AQuantityQuestionShowsTheSaleAndEnterTakesTheUnitsLeft) {
    DecisionSession session;
    session.start(asksUntilAnswered({quantityQuestion()}));

    const auto output = ask(session, "\n");

    EXPECT_EQ(output,
              "Question request-0001: the sale of XX0000000001 Synthetic Share on 2024-03-01 is "
              "1.0000 units, but your earlier purchases left only 0.99999999 units.\n"
              "  The difference is below 0.00000100 units, but the files do not show that it is\n"
              "  rounding. Enter the quantity of the sale to use (at most the units left, with up "
              "to\n"
              "  8 decimals), press Enter for the units left, or cancel: no answer, and the\n"
              "  investment is blocked.\n"
              "Your answer (a quantity, Enter or cancel):\n"
              "\n");
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer,
              AnswerValue{QuantityCorrectionAnswer{.mCorrectedUnits = 99'999'999}});
}

TEST(DecisionPromptsTest, ATypedQuantityWithADecimalCommaIsRecorded) {
    DecisionSession session;
    session.start(asksUntilAnswered({quantityQuestion()}));

    (void)ask(session, "0,99999998\n");

    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer,
              AnswerValue{QuantityCorrectionAnswer{.mCorrectedUnits = 99'999'998}});
}

TEST(DecisionPromptsTest, AnInvalidQuantityIsAskedAgain) {
    // Not a number, zero and nine decimals are refused before the session sees them.
    DecisionSession session;
    session.start(asksUntilAnswered({quantityQuestion()}));

    const auto output = ask(session, "abc\n0\n0.999999985\n\n");
    const auto hint = std::string{"Enter a quantity above 0 with at most 8 decimals, press Enter "
                                  "or answer cancel.\n"};

    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer,
              AnswerValue{QuantityCorrectionAnswer{.mCorrectedUnits = 99'999'999}});

    std::size_t hints = 0;

    for (auto position = output.find(hint); position != std::string::npos;
         position = output.find(hint, position + 1))
    {
        ++hints;
    }

    EXPECT_EQ(hints, 3U); // one for each refused line
}

TEST(DecisionPromptsTest, ACancelledQuantityQuestionRecordsTheCancel) {
    DecisionSession session;
    session.start(asksUntilAnswered({quantityQuestion()}));

    (void)ask(session, "Cancel\n");

    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer, AnswerValue{CancelAnswer{}});
}

TEST(DecisionPromptsTest, AQuantityThatTheSessionRejectsIsAskedAgainWithItsReason) {
    // 1 unit is more than the 0.99999999 units left.
    DecisionSession session;
    session.start(asksUntilAnswered({quantityQuestion()}));

    const auto output = ask(session, "1\ncancel\n");

    EXPECT_TRUE(contains(output,
                         "The corrected quantity in the answer to request-0001 is above the "
                         "units that the earlier purchases left."));
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer, AnswerValue{CancelAnswer{}});
}

TEST(DecisionPromptsTest, AnInputEndingAtTheQuantityQuestionLeavesItOpen) {
    DecisionSession session;
    session.start(asksUntilAnswered({quantityQuestion()}));

    const auto output = ask(session, "");

    EXPECT_TRUE(contains(output, "Input ended: the open questions are listed in the summary."));
    EXPECT_EQ(session.result().mRequests.size(), 1U);
}

TEST(DecisionPromptsTest, TheAnswersOfAQuantityQuestionAreListedInTheSummary) {
    DecisionSession session;
    session.start(asksUntilAnswered({quantityQuestion()}));

    (void)ask(session, "0.99999998\n");

    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(cli::answerLine(session.result().mDecisions[0]),
              "XX0000000001 Synthetic Share, sale on 2024-03-01: sale quantity 0.99999998 units");
}

// The payer choice question

/// Two reference rows of one synthetic payer.
DecisionQuestion payerQuestion() {
    return DecisionQuestion{
        .mType = RequestType::IncomeDetailsRequired,
        .mAffectedReports = {ReportType::Dividends},
        .mIsin = "XX0000000010",
        .mData =
            PayerChoiceQuestion{
                .mIsin = "XX0000000010",
                .mName = "Synthetic Payer",
                .mOptions = {PayerChoiceOption{.mName = "Payer X",
                                               .mAddress = "1 Main Street",
                                               .mCountry = "DE",
                                               .mTaxNumber = std::nullopt,
                                               .mSource = {.mSource = "manual",
                                                           .mSourceRevision = "",
                                                           .mFile = "payers.csv",
                                                           .mRowNumber = 2,
                                                           .mOrigin = CatalogueOrigin::BuiltIn}},
                             PayerChoiceOption{.mName = "Payer Y",
                                               .mAddress = "2 Main Street",
                                               .mCountry = "FR",
                                               .mTaxNumber = std::nullopt,
                                               .mSource = {.mSource = "manual",
                                                           .mSourceRevision = "",
                                                           .mFile = "payers.csv",
                                                           .mRowNumber = 3,
                                                           .mOrigin = CatalogueOrigin::BuiltIn}}},
            },
    };
}

TEST(DecisionPromptsTest, APayerChoiceQuestionListsTheRowsAndTheNumberPicksOne) {
    DecisionSession session;
    session.start(asksUntilAnswered({payerQuestion()}));

    const auto output = ask(session, "2\n");

    EXPECT_TRUE(cli::canAsk(payerQuestion().mData));
    EXPECT_TRUE(contains(output,
                         "Question request-0001: the payer of XX0000000010 Synthetic Payer has "
                         "several reference rows that differ:\n"
                         "  1: Payer X, 1 Main Street, DE\n"
                         "  2: Payer Y, 2 Main Street, FR\n"));
    EXPECT_TRUE(session.result().mRequests.empty());
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer,
              AnswerValue{PayerChoiceAnswer{.mPickedOption = 1}}); // row 2 is position 1
}

TEST(DecisionPromptsTest, APayerRowWithoutAnAddressIsShownWithAPlaceholder) {
    auto question = payerQuestion();
    auto& data = std::get<PayerChoiceQuestion>(question.mData);

    data.mOptions[0].mAddress = "";

    DecisionSession session;
    session.start(asksUntilAnswered({question}));

    const auto output = ask(session, "1\n");

    EXPECT_TRUE(contains(output,
                         "  1: Payer X, (no address), DE\n"
                         "  2: Payer Y, 2 Main Street, FR\n"));
}

TEST(DecisionPromptsTest, ASkipAnswersAPayerChoiceQuestionWithNoRow) {
    DecisionSession session;
    session.start(asksUntilAnswered({payerQuestion()}));

    (void)ask(session, "SKIP\n");

    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer,
              AnswerValue{PayerChoiceAnswer{.mPickedOption = std::nullopt}});
}

TEST(DecisionPromptsTest, ARowNumberOutsideThePayerRowsIsAskedAgain) {
    // The question has 2 rows, so 0, 3 and a word are not answers.
    DecisionSession session;
    session.start(asksUntilAnswered({payerQuestion()}));

    const auto output = ask(session, "0\n3\nfirst\n1\n");

    EXPECT_EQ(countOf(output, "Answer a row number from 1 to 2 or skip.\n"), 3U);
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer,
              AnswerValue{PayerChoiceAnswer{.mPickedOption = 0}});
}

TEST(DecisionPromptsTest, AnInputEndingAtThePayerQuestionLeavesItOpen) {
    DecisionSession session;
    session.start(asksUntilAnswered({payerQuestion()}));

    const auto output = ask(session, "");

    EXPECT_TRUE(contains(output, "Input ended: the open questions are listed in the summary."));
    EXPECT_EQ(session.result().mRequests.size(), 1U);
}

TEST(DecisionPromptsTest, ThePayerAnswersAreListedInTheSummary) {
    DecisionSession session;
    session.start(asksUntilAnswered({payerQuestion()}));

    const auto picked = session.answer({DecisionAnswer{
        .mRequestId = "request-0001",
        .mRunRevision = session.result().mRunRevision,
        .mValue = PayerChoiceAnswer{.mPickedOption = 1},
    }});

    ASSERT_TRUE(picked.empty());
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(cli::answerLine(session.result().mDecisions[0]),
              "XX0000000010 Synthetic Payer, payer: payer row 2");

    // A new run with the pick deleted asks again; the skip is the second answer.
    const auto deleted = session.deleteAllAnswers(session.result().mRunRevision);

    ASSERT_TRUE(deleted.empty());

    const auto skipped = session.answer({DecisionAnswer{
        .mRequestId = "request-0001",
        .mRunRevision = session.result().mRunRevision,
        .mValue = PayerChoiceAnswer{.mPickedOption = std::nullopt},
    }});

    ASSERT_TRUE(skipped.empty());
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(cli::answerLine(session.result().mDecisions[0]),
              "XX0000000010 Synthetic Payer, payer: skipped, the payer is completed in "
              "eDavki");
}

/// The imported EUR 900.00 of deposit interest of 2024.
DecisionQuestion depositQuestion() {
    return DecisionQuestion{
        .mType = RequestType::DepositInterestTotalRequired,
        .mAffectedReports = {ReportType::BankDepositInterest},
        .mData = DepositInterestTotalQuestion{.mYear = 2024, .mTotal = 9'000'000},
    };
}

TEST(DecisionPromptsTest, TheDepositInterestQuestionNamesTheImportedTotalAndYesMakesDohDho) {
    DecisionSession session;
    session.start(asksUntilAnswered({depositQuestion()}));

    const auto output = ask(session, "yes\n");

    EXPECT_TRUE(cli::canAsk(depositQuestion().mData));
    EXPECT_TRUE(contains(output,
                         "Question request-0001: the bank-deposit interest imported for 2024 is "
                         "EUR 900.00, not above the EUR 1,000 allowance.\n"));
    EXPECT_TRUE(session.result().mRequests.empty());
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer,
              AnswerValue{DepositInterestTotalAnswer{.mAboveLimit = true}});
}

TEST(DecisionPromptsTest, NoAnswersTheDepositInterestQuestionWithNoDohDho) {
    DecisionSession session;
    session.start(asksUntilAnswered({depositQuestion()}));

    (void)ask(session, "No\n");

    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer,
              AnswerValue{DepositInterestTotalAnswer{.mAboveLimit = false}});
}

TEST(DecisionPromptsTest, TheDepositInterestQuestionHasNoCancelAndAsksAgain) {
    // The session accepts only yes or no here, so "cancel" is a wrong word.
    DecisionSession session;
    session.start(asksUntilAnswered({depositQuestion()}));

    const auto output = ask(session, "cancel\nmaybe\nyes\n");

    EXPECT_EQ(countOf(output, "Answer yes or no.\n"), 2U);
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer,
              AnswerValue{DepositInterestTotalAnswer{.mAboveLimit = true}});
}

TEST(DecisionPromptsTest, AnInputEndingAtTheDepositInterestQuestionLeavesItOpen) {
    DecisionSession session;
    session.start(asksUntilAnswered({depositQuestion()}));

    const auto output = ask(session, "");

    EXPECT_TRUE(contains(output, "Input ended: the open questions are listed in the summary."));
    EXPECT_EQ(session.result().mRequests.size(), 1U);
}

TEST(DecisionPromptsTest, TheDepositInterestAnswersAreListedInTheSummary) {
    DecisionSession session;
    session.start(asksUntilAnswered({depositQuestion()}));

    const auto yes = session.answer({DecisionAnswer{
        .mRequestId = "request-0001",
        .mRunRevision = session.result().mRunRevision,
        .mValue = DepositInterestTotalAnswer{.mAboveLimit = true},
    }});

    ASSERT_TRUE(yes.empty());
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(cli::answerLine(session.result().mDecisions[0]),
              "bank-deposit interest 2024: above EUR 1,000 with the other banks");

    const auto deleted = session.deleteAllAnswers(session.result().mRunRevision);

    ASSERT_TRUE(deleted.empty());

    const auto no = session.answer({DecisionAnswer{
        .mRequestId = "request-0001",
        .mRunRevision = session.result().mRunRevision,
        .mValue = DepositInterestTotalAnswer{.mAboveLimit = false},
    }});

    ASSERT_TRUE(no.empty());
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(cli::answerLine(session.result().mDecisions[0]),
              "bank-deposit interest 2024: not above EUR 1,000 with the other banks");
}

/// An interest payment of unknown type of EUR 12.00 on 2025-03-10.
DecisionQuestion incomeQuestion() {
    return DecisionQuestion{
        .mType = RequestType::TaxClassificationRequired,
        .mAffectedReports = {ReportType::Dividends,
                             ReportType::BankDepositInterest,
                             ReportType::OtherInterest},
        .mIsin = "XX0000000001",
        .mData = IncomeClassificationQuestion{.mEventKind = IncomeEventKind::Interest,
                                              .mDate = day(2025, 3, 10),
                                              .mName = "Synthetic Payer",
                                              .mIsin = "XX0000000001",
                                              .mClassLabels = {"STOCK"},
                                              .mGrossAmount = 120'000, // EUR 12.00
                                              .mCurrency = Currency::EUR},
    };
}

TEST(DecisionPromptsTest, AnIncomeClassificationQuestionShowsThePaymentAndTheWordsToAnswer) {
    DecisionSession session;
    session.start(asksUntilAnswered({incomeQuestion()}));

    const auto output = ask(session, "broker_cash_interest\n");

    EXPECT_TRUE(cli::canAsk(incomeQuestion().mData));
    EXPECT_TRUE(contains(output,
                         "Question request-0001: the interest payment of XX0000000001 Synthetic "
                         "Payer on 2025-03-10, 12.00 EUR, has a type that is missing, not "
                         "recognized or\n"
                         "  differs between rows (class in the files: STOCK). What is it?\n"
                         "  dividends: a dividend (Doh-Div).\n"
                         "  bank_deposit_interest: interest on a bank deposit (Doh-DHO).\n"
                         "  broker_cash_interest: interest on cash held by a broker (Doh-Obr).\n"
                         "  bond_coupon: a bond coupon (Doh-Obr).\n"
                         "  exclude: leave it out of the reports; the summary lists it, and you "
                         "report it\n"
                         "  yourself if it is taxable.\n"
                         "  cancel: no answer, and the forms it may belong to are blocked.\n"));
    EXPECT_TRUE(session.result().mRequests.empty());
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer,
              AnswerValue{IncomeTreatmentAnswer{IncomeTreatment::BrokerCashInterest}});
}

TEST(DecisionPromptsTest, EachIncomeWordGivesItsTreatmentAndCancelGivesNoTreatment) {
    DecisionSession dividends;
    DecisionSession deposit;
    DecisionSession coupon;
    DecisionSession cancelled;

    dividends.start(asksUntilAnswered({incomeQuestion()}));
    deposit.start(asksUntilAnswered({incomeQuestion()}));
    coupon.start(asksUntilAnswered({incomeQuestion()}));
    cancelled.start(asksUntilAnswered({incomeQuestion()}));

    (void)ask(dividends, "Dividends\n");
    (void)ask(deposit, "bank_deposit_interest\n");
    (void)ask(coupon, "bond_coupon\n");
    (void)ask(cancelled, "cancel\n");

    EXPECT_EQ(dividends.result().mDecisions.at(0).mAnswer,
              AnswerValue{IncomeTreatmentAnswer{IncomeTreatment::Dividends}});
    EXPECT_EQ(deposit.result().mDecisions.at(0).mAnswer,
              AnswerValue{IncomeTreatmentAnswer{IncomeTreatment::BankDepositInterest}});
    EXPECT_EQ(coupon.result().mDecisions.at(0).mAnswer,
              AnswerValue{IncomeTreatmentAnswer{IncomeTreatment::BondCoupon}});
    EXPECT_EQ(cancelled.result().mDecisions.at(0).mAnswer, AnswerValue{CancelAnswer{}});
}

TEST(DecisionPromptsTest, ExcludeAnswersAnIncomeClassificationQuestionWithNoTreatment) {
    DecisionSession session;
    session.start(asksUntilAnswered({incomeQuestion()}));

    (void)ask(session, "EXCLUDE\n");

    EXPECT_TRUE(session.result().mRequests.empty());
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer, AnswerValue{ExcludeFromReportsAnswer{}});
}

TEST(DecisionPromptsTest, APaymentWithoutAnIsinIsNotOfferedTheDividendOrCouponWords) {
    // A dividend row and a coupon need an ISIN, so with none only the two interest words are
    // offered and "dividends" is a wrong word.
    auto question = incomeQuestion();
    auto& data = std::get<IncomeClassificationQuestion>(question.mData);

    data.mIsin = std::nullopt;
    question.mIsin = std::nullopt;

    DecisionSession session;
    session.start(asksUntilAnswered({question}));

    const auto output = ask(session, "dividends\nbond_coupon\nbank_deposit_interest\n");

    EXPECT_FALSE(contains(output, "  dividends: a dividend (Doh-Div).\n"));
    EXPECT_FALSE(contains(output, "  bond_coupon: a bond coupon (Doh-Obr).\n"));
    EXPECT_EQ(countOf(output, "Answer one of the words above.\n"), 2U);
    EXPECT_EQ(session.result().mDecisions.at(0).mAnswer,
              AnswerValue{IncomeTreatmentAnswer{IncomeTreatment::BankDepositInterest}});
}

TEST(DecisionPromptsTest, AnInputEndingAtTheIncomeClassificationQuestionLeavesItOpen) {
    DecisionSession session;
    session.start(asksUntilAnswered({incomeQuestion()}));

    const auto output = ask(session, "");

    EXPECT_TRUE(contains(output, "Input ended: the open questions are listed in the summary."));
    EXPECT_EQ(session.result().mRequests.size(), 1U);
}

DecisionQuestion bondTypeQuestion() {
    return DecisionQuestion{
        .mType = RequestType::IncomeDetailsRequired,
        .mAffectedReports = {ReportType::OtherInterest},
        .mIsin = "XX0000000010",
        .mData = BondTypeQuestion{.mIsin = "XX0000000010",
                                  .mName = "Synthetic Bond A",
                                  .mSoldUnits = 40'000'000'000, // 400 units
                                  .mMaturedUnits = 0,
                                  .mMaturityDate = std::nullopt},
    };
}

TEST(DecisionPromptsTest, ABondTypeQuestionShowsTheUnitsAndOrdinaryAnswersIt) {
    DecisionSession session;
    session.start(asksUntilAnswered({bondTypeQuestion()}));

    const auto output = ask(session, "ordinary\n");

    EXPECT_TRUE(cli::canAsk(bondTypeQuestion().mData));
    EXPECT_TRUE(contains(output,
                         "Question request-0001: the type of the bond XX0000000010 Synthetic Bond "
                         "A: sold 400.0000 and matured 0.0000 units in the year.\n"
                         "  ordinary: pays coupons; they are Doh-Obr rows.\n"));
    EXPECT_FALSE(contains(output, "(maturity "));
    EXPECT_TRUE(session.result().mRequests.empty());
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer,
              AnswerValue{BondTypeAnswer{BondType::Ordinary}});
}

TEST(DecisionPromptsTest, TheMaturityDateOfABondIsShownWhenItHasOne) {
    auto question = bondTypeQuestion();
    auto& data = std::get<BondTypeQuestion>(question.mData);

    data.mSoldUnits = 0;
    data.mMaturedUnits = 100'000'000'000; // 1000 units
    data.mMaturityDate = day(2025, 8, 14);

    DecisionSession session;
    session.start(asksUntilAnswered({question}));

    const auto output = ask(session, "discount\n");

    EXPECT_TRUE(contains(output,
                         "sold 0.0000 and matured 1000.0000 units in the year (maturity "
                         "2025-08-14).\n"));
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer,
              AnswerValue{BondTypeAnswer{BondType::DiscountOrZeroCoupon}});
}

TEST(DecisionPromptsTest, ACancelledBondTypeQuestionAndAWrongWord) {
    DecisionSession session;
    session.start(asksUntilAnswered({bondTypeQuestion()}));

    const auto output = ask(session, "zero\nCANCEL\n");

    EXPECT_EQ(countOf(output, "Answer ordinary, discount or cancel.\n"), 1U);
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(session.result().mDecisions[0].mAnswer, AnswerValue{CancelAnswer{}});
}

TEST(DecisionPromptsTest, AnInputEndingAtTheBondTypeQuestionLeavesItOpen) {
    DecisionSession session;
    session.start(asksUntilAnswered({bondTypeQuestion()}));

    const auto output = ask(session, "");

    EXPECT_TRUE(contains(output, "Input ended: the open questions are listed in the summary."));
    EXPECT_EQ(session.result().mRequests.size(), 1U);
}

TEST(DecisionPromptsTest, TheBondTypeAnswerIsListedInTheSummary) {
    DecisionSession session;
    session.start(asksUntilAnswered({bondTypeQuestion()}));

    const auto rejections = session.answer({DecisionAnswer{
        .mRequestId = "request-0001",
        .mRunRevision = session.result().mRunRevision,
        .mValue = BondTypeAnswer{BondType::DiscountOrZeroCoupon},
    }});

    ASSERT_TRUE(rejections.empty());
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(cli::answerLine(session.result().mDecisions[0]),
              "XX0000000010 Synthetic Bond A, bond type: a discount or zero-coupon bond (issued "
              "below its nominal value)");
}

TEST(DecisionPromptsTest, ADividendAnswerIsListedInTheSummary) {
    DecisionSession session;
    session.start(asksUntilAnswered({incomeQuestion()}));

    const auto rejections = session.answer({DecisionAnswer{
        .mRequestId = "request-0001",
        .mRunRevision = session.result().mRunRevision,
        .mValue = IncomeTreatmentAnswer{IncomeTreatment::Dividends},
    }});

    ASSERT_TRUE(rejections.empty());
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(cli::answerLine(session.result().mDecisions[0]),
              "XX0000000001 Synthetic Payer, payment on 2025-03-10: a dividend (Doh-Div)");
}

TEST(DecisionPromptsTest, ABankDepositAnswerIsListedInTheSummary) {
    DecisionSession session;
    session.start(asksUntilAnswered({incomeQuestion()}));

    const auto rejections = session.answer({DecisionAnswer{
        .mRequestId = "request-0001",
        .mRunRevision = session.result().mRunRevision,
        .mValue = IncomeTreatmentAnswer{IncomeTreatment::BankDepositInterest},
    }});

    ASSERT_TRUE(rejections.empty());
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(
        cli::answerLine(session.result().mDecisions[0]),
        "XX0000000001 Synthetic Payer, payment on 2025-03-10: bank-deposit interest (Doh-DHO)");
}

TEST(DecisionPromptsTest, ABrokerCashAnswerIsListedInTheSummary) {
    DecisionSession session;
    session.start(asksUntilAnswered({incomeQuestion()}));

    const auto rejections = session.answer({DecisionAnswer{
        .mRequestId = "request-0001",
        .mRunRevision = session.result().mRunRevision,
        .mValue = IncomeTreatmentAnswer{IncomeTreatment::BrokerCashInterest},
    }});

    ASSERT_TRUE(rejections.empty());
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(
        cli::answerLine(session.result().mDecisions[0]),
        "XX0000000001 Synthetic Payer, payment on 2025-03-10: interest on broker cash (Doh-Obr)");
}

TEST(DecisionPromptsTest, ABondCouponAnswerIsListedInTheSummary) {
    DecisionSession session;
    session.start(asksUntilAnswered({incomeQuestion()}));

    const auto rejections = session.answer({DecisionAnswer{
        .mRequestId = "request-0001",
        .mRunRevision = session.result().mRunRevision,
        .mValue = IncomeTreatmentAnswer{IncomeTreatment::BondCoupon},
    }});

    ASSERT_TRUE(rejections.empty());
    ASSERT_EQ(session.result().mDecisions.size(), 1U);
    EXPECT_EQ(cli::answerLine(session.result().mDecisions[0]),
              "XX0000000001 Synthetic Payer, payment on 2025-03-10: a bond coupon (Doh-Obr)");
}

} // namespace
