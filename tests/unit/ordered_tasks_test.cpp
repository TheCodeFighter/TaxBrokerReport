#include "service/ordered_tasks.hpp"
#include "service/parse_failure.hpp"
#include "taxbroker/diagnostic_messages.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <exception>
#include <latch>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace taxbroker {
namespace {

TEST(OrderedTasksTest, WorkerCountIsBoundedByFilesLimitAndHardware) {
    EXPECT_EQ(parseWorkerCount(0, 4, 8), 0U);  // No file, no worker.
    EXPECT_EQ(parseWorkerCount(1, 4, 8), 1U);  // One file needs one worker.
    EXPECT_EQ(parseWorkerCount(3, 4, 8), 3U);  // Fewer files than the limit.
    EXPECT_EQ(parseWorkerCount(8, 4, 8), 4U);  // The limit.
    EXPECT_EQ(parseWorkerCount(8, 4, 2), 2U);  // Two hardware threads.
    EXPECT_EQ(parseWorkerCount(8, 4, 0), 1U);  // Hardware threads unknown.
    EXPECT_EQ(parseWorkerCount(8, 1, 8), 1U);  // Limit 1 is the sequential mode.
    EXPECT_EQ(parseWorkerCount(8, 0, 8), 4U);  // Limit 0 is the default limit, 4.
    EXPECT_EQ(parseWorkerCount(8, 16, 8), 8U); // A high limit stops at the file count.
}

TEST(OrderedTasksTest, ResultsAreInTaskOrder) {
    const std::vector<TaskOutcome<int>> outcomes =
        runTasksInOrder(5, 3, [](const std::size_t aIndex) {
            return static_cast<int>(aIndex) * 10;
        });

    ASSERT_EQ(outcomes.size(), 5U);

    EXPECT_EQ(outcomes[0].mValue.value(), 0);
    EXPECT_EQ(outcomes[1].mValue.value(), 10);
    EXPECT_EQ(outcomes[2].mValue.value(), 20);
    EXPECT_EQ(outcomes[3].mValue.value(), 30);
    EXPECT_EQ(outcomes[4].mValue.value(), 40);
}

TEST(OrderedTasksTest, ResultsStayInTaskOrderWhenTasksFinishInReverse) {
    // Task 0 waits for task 1 and task 1 waits for task 2, so they finish as 2, 1, 0. Three
    // workers hold the three tasks at the same time.
    std::latch task1Done{1};
    std::latch task2Done{1};
    std::mutex mutex;
    std::vector<std::size_t> finished;

    const std::vector<TaskOutcome<std::string>> outcomes =
        runTasksInOrder(3, 3, [&](const std::size_t aIndex) {
            if (aIndex == 0)
            {
                task1Done.wait();
            }

            if (aIndex == 1)
            {
                task2Done.wait();
            }

            {
                const std::lock_guard<std::mutex> lock{mutex};
                finished.push_back(aIndex);
            }

            if (aIndex == 1)
            {
                task1Done.count_down();
            }

            if (aIndex == 2)
            {
                task2Done.count_down();
            }

            return std::string(1, static_cast<char>('a' + aIndex));
        });

    ASSERT_EQ(finished.size(), 3U);

    EXPECT_EQ(finished[0], 2U);
    EXPECT_EQ(finished[1], 1U);
    EXPECT_EQ(finished[2], 0U);

    ASSERT_EQ(outcomes.size(), 3U);

    EXPECT_EQ(outcomes[0].mValue.value(), "a");
    EXPECT_EQ(outcomes[1].mValue.value(), "b");
    EXPECT_EQ(outcomes[2].mValue.value(), "c");
}

TEST(OrderedTasksTest, OneWorkerRunsOnTheCallingThread) {
    std::vector<std::thread::id> threadIds;
    std::vector<std::size_t> order;

    const std::vector<TaskOutcome<std::size_t>> outcomes =
        runTasksInOrder(3, 1, [&](const std::size_t aIndex) {
            threadIds.push_back(std::this_thread::get_id());
            order.push_back(aIndex);

            return aIndex;
        });

    ASSERT_EQ(outcomes.size(), 3U);
    ASSERT_EQ(threadIds.size(), 3U);

    EXPECT_EQ(threadIds[0], std::this_thread::get_id());
    EXPECT_EQ(threadIds[1], std::this_thread::get_id());
    EXPECT_EQ(threadIds[2], std::this_thread::get_id());

    EXPECT_EQ(order[0], 0U);
    EXPECT_EQ(order[1], 1U);
    EXPECT_EQ(order[2], 2U);
}

TEST(OrderedTasksTest, NoMoreTasksRunAtOnceThanWorkers) {
    std::atomic<int> running{0};
    std::atomic<int> peak{0};

    const std::vector<TaskOutcome<std::size_t>> outcomes =
        runTasksInOrder(8, 2, [&](const std::size_t aIndex) {
            const int now = ++running;
            int seenPeak = peak.load();

            while (now > seenPeak && !peak.compare_exchange_weak(seenPeak, now))
            {
            }

            std::this_thread::yield();
            --running;

            return aIndex;
        });

    ASSERT_EQ(outcomes.size(), 8U);

    EXPECT_LE(peak.load(), 2);

    for (std::size_t index = 0; index < outcomes.size(); ++index)
    {
        EXPECT_EQ(outcomes[index].mValue.value(), index);
    }
}

TEST(OrderedTasksTest, ThrowingTaskKeepsTheOtherResults) {
    const std::vector<TaskOutcome<int>> outcomes =
        runTasksInOrder(3, 2, [](const std::size_t aIndex) {
            if (aIndex == 1)
            {
                throw std::logic_error{"task 1 failed"};
            }

            return static_cast<int>(aIndex);
        });

    ASSERT_EQ(outcomes.size(), 3U);

    EXPECT_EQ(outcomes[0].mValue.value(), 0);
    EXPECT_FALSE(outcomes[0].mException);

    EXPECT_FALSE(outcomes[1].mValue.has_value());
    ASSERT_TRUE(outcomes[1].mException);
    EXPECT_THROW(std::rethrow_exception(outcomes[1].mException), std::logic_error);

    EXPECT_EQ(outcomes[2].mValue.value(), 2);
    EXPECT_FALSE(outcomes[2].mException);
}

TEST(OrderedTasksTest, NoTaskGivesNoOutcome) {
    const std::vector<TaskOutcome<std::size_t>> outcomes =
        runTasksInOrder(0, 0, [](const std::size_t aIndex) { return aIndex; });

    EXPECT_TRUE(outcomes.empty());
}

TEST(ParseFailureResultTest, GivesNoEventsAndOneFileErrorWithTheFileName) {
    const BrokerInputFile file{
        .mBroker = Broker::InteractiveBrokers,
        .mFile = InputFile{.mName = "b.xml", .mContent = "<FlexQueryResponse/>"},
    };

    const ParseResult result = parseFailureResult(file);

    EXPECT_EQ(result.mBroker, Broker::InteractiveBrokers);
    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const ParseDiagnostic& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::ParseError);
    EXPECT_EQ(diagnostic.mSourceFile, "b.xml");
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::ParseErrorInternal);
    EXPECT_EQ(diagnostic.mMessage, "The file could not be read because of an internal error.");
    EXPECT_FALSE(diagnostic.mRowIndex.has_value());
}

TEST(ParseFailureResultTest, AnOutcomeWithAValueGivesThatValue) {
    const BrokerInputFile file{
        .mBroker = Broker::TradeRepublic,
        .mFile = InputFile{.mName = "a.csv", .mContent = "synthetic"},
    };
    TaskOutcome<ParseResult> outcome;
    outcome.mValue.emplace(ParseResult{.mBroker = Broker::TradeRepublic});

    const ParseResult result = parseResultOfOutcome(std::move(outcome), file, 0);

    EXPECT_EQ(result.mBroker, Broker::TradeRepublic);
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(ParseFailureResultTest, AnOutcomeWithAnExceptionGivesTheFileError) {
    const BrokerInputFile file{
        .mBroker = Broker::TradeRepublic,
        .mFile = InputFile{.mName = "exports/b.csv", .mContent = "synthetic"},
    };
    TaskOutcome<ParseResult> outcome;
    outcome.mException = std::make_exception_ptr(std::logic_error{"a programming error"});

    const ParseResult result = parseResultOfOutcome(std::move(outcome), file, 1);

    EXPECT_EQ(result.mBroker, Broker::TradeRepublic);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics.front().mSourceFile, "b.csv"); // The folder is dropped.
    EXPECT_EQ(result.mDiagnostics.front().mMessageKey, MessageKey::ParseErrorInternal);
}

} // namespace
} // namespace taxbroker
