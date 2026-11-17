#include "../support/log_capture.hpp"
#include "taxbroker/build_info.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {
using namespace taxbroker;

TEST(BuildInfoTest, TheVersionIsTheProjectVersion) {
    EXPECT_EQ(buildVersion(), "2.0.0");
    EXPECT_EQ(buildInfoText(), "TaxBrokerReport 2.0.0");
}

TEST(BuildInfoTest, ThePlatformTextHoldsOnlyCompileTimeFacts) {
    const std::string text = buildPlatformText();

    // Tests run on Linux: the build type, then the operating system and the processor.
    EXPECT_TRUE(text.starts_with("debug, linux ") || text.starts_with("release, linux ")) << text;
    EXPECT_TRUE(text.ends_with(" x86_64") || text.ends_with(" aarch64")) << text;
}

TEST(BuildInfoTest, TheBuildTypeAndTheSystemAreTheTwoPartsOfThePlatformText) {
    // The tests run on Linux, in a debug or a release build.
    EXPECT_TRUE(buildTypeName() == "debug" || buildTypeName() == "release");
    EXPECT_TRUE(buildSystemName() == "linux x86_64" || buildSystemName() == "linux aarch64")
        << buildSystemName();
    EXPECT_EQ(buildPlatformText(), std::string{buildTypeName()} + ", " + buildSystemName());
}

TEST(BuildInfoTest, TheStartLineNamesTheProgramAndNothingPrivate) {
    LogCapture log;

    logProgramStart("report");

    EXPECT_EQ(log.count("TaxBrokerReport 2.0.0 started: report ("), 1U);
    EXPECT_EQ(log.count("/"), 0U); // no path
    EXPECT_EQ(log.count(", linux "), 1U);
}

} // namespace
