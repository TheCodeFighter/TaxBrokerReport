#include "processors/application_summaries.hpp"
#include "taxbroker/official_rates.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <array>
#include <span>

namespace {
using namespace taxbroker;

TEST(ApplicationSummariesTest, BuiltInReferenceFilesAndRatesAreNamedWithTheirOrigin) {
    const auto rates = loadOfficialRates(std::span<const InputFile>{});

    const auto sources = dataSourcesOf(
        {CatalogueOrigin::BuiltIn, CatalogueOrigin::BuiltIn, CatalogueOrigin::BuiltIn},
        rates.mTable);

    ASSERT_EQ(sources.mReferenceFiles.size(), 3U);
    EXPECT_EQ(sources.mReferenceFiles[0].mFile, "payers.csv");
    EXPECT_EQ(sources.mReferenceFiles[1].mFile, "relief_statements.csv");
    EXPECT_EQ(sources.mReferenceFiles[2].mFile, "broker_payers.csv");

    for (const auto& file : sources.mReferenceFiles)
    {
        EXPECT_EQ(file.mOrigin, CatalogueOrigin::BuiltIn);
    }

    // Only the built-in payers carry the revision of the upstream data they come from.
    EXPECT_FALSE(sources.mReferenceFiles[0].mUpstreamRevision.empty());
    EXPECT_TRUE(sources.mReferenceFiles[1].mUpstreamRevision.empty());
    EXPECT_TRUE(sources.mReferenceFiles[2].mUpstreamRevision.empty());

    // The years 2016 to 2025 are built in.
    ASSERT_EQ(sources.mRateFiles.size(), 10U);
    EXPECT_EQ(sources.mRateFiles.front().mYear, 2016);
    EXPECT_EQ(sources.mRateFiles.back().mYear, 2025);

    for (const auto& file : sources.mRateFiles)
    {
        EXPECT_EQ(file.mOrigin, OfficialRateOrigin::BuiltIn);
        EXPECT_FALSE(file.mReplacesBuiltIn);
    }
}

TEST(ApplicationSummariesTest, ASuppliedPayersFileHasNoUpstreamRevisionAndASuppliedYearReplaces) {
    OfficialRateTable table;

    table.mFiles.push_back(OfficialRateFile{.mSourceFile = SourceFilename::fromPath("exr_2024.csv"),
                                            .mYear = 2024,
                                            .mFirstDate = *parseCalendarDate("2024-01-02"),
                                            .mLastDate = *parseCalendarDate("2024-12-31"),
                                            .mRateCount = 3,
                                            .mOrigin = OfficialRateOrigin::Supplied,
                                            .mReplacesBuiltIn = true});

    const auto sources = dataSourcesOf(
        {CatalogueOrigin::Supplied, CatalogueOrigin::BuiltIn, CatalogueOrigin::Supplied},
        table);

    EXPECT_EQ(sources.mReferenceFiles[0].mOrigin, CatalogueOrigin::Supplied);
    EXPECT_TRUE(sources.mReferenceFiles[0].mUpstreamRevision.empty());
    EXPECT_EQ(sources.mReferenceFiles[1].mOrigin, CatalogueOrigin::BuiltIn);
    EXPECT_EQ(sources.mReferenceFiles[2].mOrigin, CatalogueOrigin::Supplied);

    ASSERT_EQ(sources.mRateFiles.size(), 1U);
    EXPECT_EQ(sources.mRateFiles[0].mFile, "exr_2024.csv");
    EXPECT_EQ(sources.mRateFiles[0].mYear, 2024);
    EXPECT_EQ(sources.mRateFiles[0].mOrigin, OfficialRateOrigin::Supplied);
    EXPECT_TRUE(sources.mRateFiles[0].mReplacesBuiltIn);
}

} // namespace
