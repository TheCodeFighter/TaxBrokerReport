#include "taxbroker/logging.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
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
            const char* old = std::getenv(name);

            mOld.emplace_back(old != nullptr ? std::optional<std::string>{old} : std::nullopt);
            unsetenv(name);
        }
    }

    void TearDown() override {
        for (std::size_t index = 0; index < kNames.size(); ++index)
        {
            if (mOld[index].has_value())
            {
                setenv(kNames[index], mOld[index]->c_str(), 1);
            }
            else
            {
                unsetenv(kNames[index]);
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
    setenv("TBR_LOG_LEVEL", "warn", 1);
    setenv("TBR_LOG_FILE_MODE", "truncate", 1);
    setenv("TBR_LOG_ASYNC", "0", 1);
    setenv("TBR_LOG_QUEUE_SIZE", "65536", 1); // the largest allowed value
    setenv("TBR_LOG_THREADS", "8", 1);        // the largest allowed value
    setenv("TBR_LOG_STDOUT", "true", 1);

    EXPECT_TRUE(InvalidLogSettings().empty());
}

TEST_F(LogEnvironment, EachInvalidValueIsReportedByNameOnly) {
    setenv("TBR_LOG_LEVEL", "loud", 1);
    setenv("TBR_LOG_FILE_MODE", "sometimes", 1);
    setenv("TBR_LOG_ASYNC", "yes", 1);
    setenv("TBR_LOG_QUEUE_SIZE", "65537", 1); // one over the largest allowed value
    setenv("TBR_LOG_THREADS", "9", 1);        // one over the largest allowed value
    setenv("TBR_LOG_STDOUT", "2", 1);

    const std::vector<std::string> expected{"TBR_LOG_LEVEL",
                                            "TBR_LOG_FILE_MODE",
                                            "TBR_LOG_ASYNC",
                                            "TBR_LOG_QUEUE_SIZE",
                                            "TBR_LOG_THREADS",
                                            "TBR_LOG_STDOUT"};

    EXPECT_EQ(InvalidLogSettings(), expected);
}

TEST_F(LogEnvironment, ZeroAndTextAreInvalidSizes) {
    setenv("TBR_LOG_QUEUE_SIZE", "0", 1);
    setenv("TBR_LOG_THREADS", "four", 1);

    const std::vector<std::string> expected{"TBR_LOG_QUEUE_SIZE", "TBR_LOG_THREADS"};

    EXPECT_EQ(InvalidLogSettings(), expected);
}

} // namespace
} // namespace taxbroker
