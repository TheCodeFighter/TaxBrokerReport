#include "taxbroker/logging.hpp"

#include "../support/test_environment.hpp"

#include "taxbroker/environment.hpp"

#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace taxbroker {
namespace {

/// Sets the six `TBR_LOG_*` variables for one test and restores the old values at the end.
class LogEnvironment : public ::testing::Test {
  protected:
    void SetUp() override {
        for (const char* name : kNames)
        {
            mOld.emplace_back(readEnvironment(name));
            test::unsetTestEnvironment(name);
        }
    }

    void TearDown() override {
        for (std::size_t index = 0; index < kNames.size(); ++index)
        {
            if (mOld[index].has_value())
            {
                test::setTestEnvironment(kNames[index], *mOld[index]);
            }
            else
            {
                test::unsetTestEnvironment(kNames[index]);
            }
        }
    }

    static constexpr std::array<const char*, 6> kNames{"TBR_LOG_LEVEL",
                                                       "TBR_LOG_FILE_MODE",
                                                       "TBR_LOG_ASYNC",
                                                       "TBR_LOG_QUEUE_SIZE",
                                                       "TBR_LOG_THREADS",
                                                       "TBR_LOG_STDOUT"};

    std::vector<std::optional<std::string>> mOld;
};

TEST_F(LogEnvironment, NothingSetMeansNothingInvalid) {
    EXPECT_TRUE(InvalidLogSettings().empty());
}

TEST_F(LogEnvironment, ValidValuesAreNotReported) {
    test::setTestEnvironment("TBR_LOG_LEVEL", "warn");
    test::setTestEnvironment("TBR_LOG_FILE_MODE", "truncate");
    test::setTestEnvironment("TBR_LOG_ASYNC", "0");
    test::setTestEnvironment("TBR_LOG_QUEUE_SIZE", "65536"); // the largest allowed value
    test::setTestEnvironment("TBR_LOG_THREADS", "8");        // the largest allowed value
    test::setTestEnvironment("TBR_LOG_STDOUT", "true");

    EXPECT_TRUE(InvalidLogSettings().empty());
}

TEST_F(LogEnvironment, EachInvalidValueIsReportedByNameOnly) {
    test::setTestEnvironment("TBR_LOG_LEVEL", "loud");
    test::setTestEnvironment("TBR_LOG_FILE_MODE", "sometimes");
    test::setTestEnvironment("TBR_LOG_ASYNC", "yes");
    test::setTestEnvironment("TBR_LOG_QUEUE_SIZE", "65537"); // one over the largest allowed value
    test::setTestEnvironment("TBR_LOG_THREADS", "9");        // one over the largest allowed value
    test::setTestEnvironment("TBR_LOG_STDOUT", "2");

    const std::vector<std::string> expected{"TBR_LOG_LEVEL",
                                            "TBR_LOG_FILE_MODE",
                                            "TBR_LOG_ASYNC",
                                            "TBR_LOG_QUEUE_SIZE",
                                            "TBR_LOG_THREADS",
                                            "TBR_LOG_STDOUT"};

    EXPECT_EQ(InvalidLogSettings(), expected);
}

TEST_F(LogEnvironment, ZeroAndTextAreInvalidSizes) {
    test::setTestEnvironment("TBR_LOG_QUEUE_SIZE", "0");
    test::setTestEnvironment("TBR_LOG_THREADS", "four");

    const std::vector<std::string> expected{"TBR_LOG_QUEUE_SIZE", "TBR_LOG_THREADS"};

    EXPECT_EQ(InvalidLogSettings(), expected);
}

} // namespace
} // namespace taxbroker
