#include "utils/csv_text_reader.hpp"

#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace taxbroker;

constexpr std::array<std::string_view, 2> kAB{"a", "b"};

TEST(CsvTextReaderTest, ReadsRowsWithLogicalRowNumbers) {
    const auto table = readCsvText("a,b\n1,2\n3,4\n", ',', kAB);

    EXPECT_FALSE(table.mFault.has_value());
    EXPECT_EQ(table.mHeader, (std::vector<std::string>{"a", "b"}));
    ASSERT_EQ(table.mRows.size(), 2U);

    EXPECT_EQ(table.mRows[0].mRowNumber, 2U); // the header is row 1
    EXPECT_EQ(table.mRows[0].mFields, (std::vector<std::string>{"1", "2"}));
    EXPECT_EQ(table.mRows[1].mRowNumber, 3U);
    EXPECT_EQ(table.mRows[1].mFields, (std::vector<std::string>{"3", "4"}));
}

TEST(CsvTextReaderTest, KeepsACommaInsideAQuotedField) {
    constexpr std::array<std::string_view, 2> columns{"name", "text"};

    const auto table = readCsvText("name,text\nx,\"One, two\"\n", ',', columns);

    EXPECT_FALSE(table.mFault.has_value());
    ASSERT_EQ(table.mRows.size(), 1U);
    EXPECT_EQ(table.mRows[0].mFields, (std::vector<std::string>{"x", "One, two"})); // 2 fields
}

TEST(CsvTextReaderTest, ReadsWindowsLineEnds) {
    const auto table = readCsvText("a,b\r\n1,2\r\n", ',', kAB);

    EXPECT_FALSE(table.mFault.has_value());
    ASSERT_EQ(table.mRows.size(), 1U);
    EXPECT_EQ(table.mRows[0].mFields, (std::vector<std::string>{"1", "2"}));
}

TEST(CsvTextReaderTest, ReadsAnotherDelimiter) {
    const auto table = readCsvText("a;b\n1;2\n", ';', kAB);

    EXPECT_FALSE(table.mFault.has_value());
    ASSERT_EQ(table.mRows.size(), 1U);
    EXPECT_EQ(table.mRows[0].mFields, (std::vector<std::string>{"1", "2"}));
}

TEST(CsvTextReaderTest, ReadsOnlyTheTextInsideTheView) {
    const std::string_view text{"a,b\n1,2\n3,4\n"};

    // The first 8 characters are "a,b\n1,2\n"; the row 3,4 is not in the view.
    const auto table = readCsvText(text.substr(0, 8), ',', kAB);

    EXPECT_FALSE(table.mFault.has_value());
    ASSERT_EQ(table.mRows.size(), 1U);
    EXPECT_EQ(table.mRows[0].mRowNumber, 2U);
    EXPECT_EQ(table.mRows[0].mFields, (std::vector<std::string>{"1", "2"}));
}

TEST(CsvTextReaderTest, RejectsAnUnclosedQuote) {
    // The quote before the 2 is never closed.
    const auto table = readCsvText("a,b\n1,\"2\n", ',', kAB);

    ASSERT_TRUE(table.mFault.has_value());
    EXPECT_EQ(table.mFault->mKind, CsvFaultKind::UnclosedQuote);
    EXPECT_EQ(table.mFault->mRowNumber, std::nullopt);
    EXPECT_TRUE(table.mRows.empty());
}

TEST(CsvTextReaderTest, RejectsALineBreakInsideAField) {
    // The quotes are even, but the field "x<newline>y" holds a line break. It is row 2.
    const auto table = readCsvText("a,b\n1,\"x\ny\"\n3,4\n", ',', kAB);

    ASSERT_TRUE(table.mFault.has_value());
    EXPECT_EQ(table.mFault->mKind, CsvFaultKind::LineBreakInField);
    EXPECT_EQ(table.mFault->mRowNumber, std::optional<std::size_t>{2});
    EXPECT_TRUE(table.mRows.empty());
}

TEST(CsvTextReaderTest, ReportsALineBreakBeforeAWrongFieldCountInTheSameRow) {
    // Row 2 has 3 fields instead of 2, and one of them holds a line break: the line break wins.
    const auto table = readCsvText("a,b\n1,\"x\ny\",z\n", ',', kAB);

    ASSERT_TRUE(table.mFault.has_value());
    EXPECT_EQ(table.mFault->mKind, CsvFaultKind::LineBreakInField);
    EXPECT_EQ(table.mFault->mRowNumber, std::optional<std::size_t>{2});
}

TEST(CsvTextReaderTest, KeepsTheRowsBeforeALineBreakInsideAField) {
    const auto table = readCsvText("a,b\n1,2\n3,\"x\ny\"\n", ',', kAB);

    ASSERT_TRUE(table.mFault.has_value());
    EXPECT_EQ(table.mFault->mKind, CsvFaultKind::LineBreakInField);
    EXPECT_EQ(table.mFault->mRowNumber, std::optional<std::size_t>{3});
    ASSERT_EQ(table.mRows.size(), 1U);
    EXPECT_EQ(table.mRows.at(0).mFields, (std::vector<std::string>{"1", "2"}));
}

TEST(CsvTextReaderTest, ReportsMissingColumnsInRequiredOrder) {
    constexpr std::array<std::string_view, 4> columns{"a", "b", "c", "d"};

    // The header has c and x. a, b and d are missing.
    const auto table = readCsvText("c,x\n1,2\n", ',', columns);

    ASSERT_TRUE(table.mFault.has_value());
    EXPECT_EQ(table.mFault->mKind, CsvFaultKind::MissingColumns);
    EXPECT_EQ(table.mFault->mRowNumber, std::nullopt);
    EXPECT_EQ(table.mFault->mColumns, (std::vector<std::string>{"a", "b", "d"}));
}

TEST(CsvTextReaderTest, ReportsMissingColumnsOfAnEmptyText) {
    const auto table = readCsvText("", ',', kAB);

    ASSERT_TRUE(table.mFault.has_value());
    EXPECT_EQ(table.mFault->mKind, CsvFaultKind::MissingColumns);
    EXPECT_EQ(table.mFault->mColumns, (std::vector<std::string>{"a", "b"}));
}

TEST(CsvTextReaderTest, ReportsTheFirstRepeatedRequiredColumn) {
    // b comes first in the header, but a is first in the required order.
    const auto table = readCsvText("b,a,b,a\n1,2,3,4\n", ',', kAB);

    ASSERT_TRUE(table.mFault.has_value());
    EXPECT_EQ(table.mFault->mKind, CsvFaultKind::RepeatedColumn);
    EXPECT_EQ(table.mFault->mRowNumber, std::nullopt);
    EXPECT_EQ(table.mFault->mColumns, (std::vector<std::string>{"a"}));
}

TEST(CsvTextReaderTest, KeepsTheRowsBeforeAFieldCountFault) {
    // Row 3 has 1 field instead of 2. Row 4 is not read.
    const auto table = readCsvText("a,b\n1,2\n3\n5,6\n", ',', kAB);

    ASSERT_TRUE(table.mFault.has_value());
    EXPECT_EQ(table.mFault->mKind, CsvFaultKind::FieldCountMismatch);
    EXPECT_EQ(table.mFault->mRowNumber, std::optional<std::size_t>{3});
    EXPECT_EQ(table.mFault->mExpectedFields, 2U);
    EXPECT_EQ(table.mFault->mFoundFields, 1U);

    ASSERT_EQ(table.mRows.size(), 1U);
    EXPECT_EQ(table.mRows[0].mRowNumber, 2U);
    EXPECT_EQ(table.mRows[0].mFields, (std::vector<std::string>{"1", "2"}));
}

TEST(CsvTextReaderTest, FindsAColumnByName) {
    constexpr std::array<std::string_view, 1> columns{"a"};

    const auto table = readCsvText("x,a\n1,2\n", ',', columns);

    EXPECT_EQ(table.columnIndex("a"), 1U); // the header is x,a
    EXPECT_THROW((void)table.columnIndex("z"), std::logic_error);
}

// A *LargeInputTest suite takes minutes under Valgrind (`scripts/valgrind.sh --part csv`).
TEST(CsvTextReaderLargeInputTest, KeepsTheRowsBeforeAnUnreadableRow) {
    // The library cannot read a row longer than two 10 MB chunks. Row 3 has a 21,000,000
    // character field; row 2 is read before it.
    const std::string text = "a,b\n1,2\n3," + std::string(21'000'000, 'x') + "\n";

    const auto table = readCsvText(text, ',', kAB);

    ASSERT_TRUE(table.mFault.has_value());
    EXPECT_EQ(table.mFault->mKind, CsvFaultKind::Unreadable);
    EXPECT_EQ(table.mFault->mRowNumber, std::nullopt);

    ASSERT_EQ(table.mRows.size(), 1U);
    EXPECT_EQ(table.mRows[0].mRowNumber, 2U);
    EXPECT_EQ(table.mRows[0].mFields, (std::vector<std::string>{"1", "2"}));
}

} // namespace
