#include "processors/income_payment_netting.hpp"

#include "parsers/ibkr_parser.hpp"
#include "processors/statement_merger.hpp"
#include "taxbroker/currency.hpp"
#include "taxbroker/diagnostic_messages.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// The dividend values of the fixture tests are the tables in tests/test_data/ibkr/README.md. Rows
// written in a test are synthetic: made-up names and ISINs, no real value. Amounts are stored with
// four decimals, so USD 12.00 is 120'000.

namespace {

using namespace taxbroker;

Date date(std::string_view aText) {
    return *parseCalendarDate(aText);
}

SourceReference ibkrSource(std::size_t aFile, std::size_t aRow, std::string aTransactionId) {
    return SourceReference{
        .mBroker = Broker::InteractiveBrokers,
        .mFilename = SourceFilename::fromPath("synthetic.xml"),
        .mSourceRow = aRow,
        .mTransactionId = std::move(aTransactionId),
        .mInputSequence = {.mSourceIndex = aFile, .mEventIndex = aRow},
    };
}

SourceReference
tradeRepublicSource(std::size_t aFile, std::size_t aRow, std::string aTransactionId) {
    return SourceReference{
        .mBroker = Broker::TradeRepublic,
        .mFilename = SourceFilename::fromPath("synthetic.csv"),
        .mSourceRow = aRow,
        .mTransactionId = std::move(aTransactionId),
        .mInputSequence = {.mSourceIndex = aFile, .mEventIndex = aRow},
    };
}

std::filesystem::path fixtureFolder() {
    return std::filesystem::path{__FILE__}.parent_path().parent_path() / "test_data" / "ibkr";
}

/// Parses the fixture files with the real parser and merges them, in the order given.
StatementMergeResult mergeFixtures(const std::vector<std::string>& aFiles) {
    ibkr::IbkrParser parser;
    std::vector<StatementMergeInput> inputs;

    for (std::size_t index = 0; index < aFiles.size(); ++index)
    {
        inputs.push_back({index, parser.parseFile(fixtureFolder() / aFiles[index], index)});
    }

    return DeterministicStatementMerger{}.merge(std::move(inputs));
}

StatementMergeResult mergeOne(ParseResult aResult) {
    std::vector<StatementMergeInput> inputs;

    inputs.push_back({0, std::move(aResult)});

    return DeterministicStatementMerger{}.merge(std::move(inputs));
}

/// Merges one payment row of file 0 and one of file 1, as two exports of the same transaction.
StatementMergeResult mergeTwoCopies(IncomePaymentRow aFirst, IncomePaymentRow aSecond) {
    std::vector<StatementMergeInput> inputs;

    inputs.push_back({0,
                      ParseResult{.mBroker = Broker::InteractiveBrokers,
                                  .mStatement = {.mIncomePaymentRows = {std::move(aFirst)}}}});
    inputs.push_back({1,
                      ParseResult{.mBroker = Broker::InteractiveBrokers,
                                  .mStatement = {.mIncomePaymentRows = {std::move(aSecond)}}}});

    return DeterministicStatementMerger{}.merge(std::move(inputs));
}

std::vector<MergeDiagnostic> mergeDiagnosticsOf(const StatementMergeResult& aMerged,
                                                MergeDiagnosticCode aCode) {
    std::vector<MergeDiagnostic> diagnostics;

    for (const auto& diagnostic : aMerged.mDiagnostics)
    {
        const auto* merge = std::get_if<MergeDiagnostic>(&diagnostic);

        if (merge != nullptr && merge->mCode == aCode)
        {
            diagnostics.push_back(*merge);
        }
    }

    return diagnostics;
}

const DividendTransaction*
findDividend(const StatementMergeResult& aMerged, std::string_view aIsin, const Date aTaxDate) {
    const auto& instruments = aMerged.mStatement.mPresentation.mDividendInstruments;
    const auto instrument = std::find_if(
        instruments.begin(),
        instruments.end(),
        [aIsin](const DividendInstrument& aInstrument) { return aInstrument.mIsin == aIsin; });

    if (instrument == instruments.end())
    {
        return nullptr;
    }

    const auto dividend = std::find_if(instrument->mTransactions.begin(),
                                       instrument->mTransactions.end(),
                                       [aTaxDate](const DividendTransaction& aDividend) {
                                           return aDividend.mMetadata.mTaxDate == aTaxDate;
                                       });

    return dividend == instrument->mTransactions.end() ? nullptr : &*dividend;
}

std::size_t dividendCount(const StatementMergeResult& aMerged) {
    const auto& instruments = aMerged.mStatement.mPresentation.mDividendInstruments;

    return std::accumulate(instruments.begin(),
                           instruments.end(),
                           std::size_t{0},
                           [](std::size_t aCount, const DividendInstrument& aInstrument) {
                               return aCount + aInstrument.mTransactions.size();
                           });
}

// netIncomePayments: rows written in the test

TEST(IncomePaymentNettingTest, DividendTaxAndNoCorrectionGiveOneDividend) {
    const std::vector<IncomePaymentRow> rows{
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 5, "1")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 120'000, // USD 12.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1"},
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 6, "2")}},
         .mKind = IncomePaymentKind::Tax,
         .mIsin = "US0000000010",
         .mAmount = 18'000, // USD 1.80
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1"},
    };

    const auto result = netIncomePayments(rows);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mDividends.size(), 1U);

    const auto& dividend = result.mDividends.front();

    EXPECT_EQ(dividend.mIsin, "US0000000010");
    EXPECT_EQ(dividend.mEvent.mMetadata.mTaxDate, date("2024-03-15"));
    EXPECT_EQ(dividend.mEvent.mMetadata.mSources.size(), 2U);
    EXPECT_EQ(dividend.mEvent.mGrossAmount, 120'000);
    EXPECT_EQ(dividend.mEvent.mTaxPaid, 18'000);
    EXPECT_EQ(dividend.mEvent.mCurrency, Currency::USD);
    EXPECT_EQ(dividend.mEvent.mTaxCurrency, Currency::USD);
    EXPECT_FALSE(dividend.mEvent.mExchangeRate.has_value());
    EXPECT_FALSE(dividend.mEvent.mMetadata.mSourceTimestamp.has_value());
}

TEST(IncomePaymentNettingTest, ADividendWithoutTaxRowsHasNoTax) {
    const std::vector<IncomePaymentRow> rows{
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 5, "1")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 120'000, // USD 12.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1"},
    };

    const auto result = netIncomePayments(rows);

    ASSERT_EQ(result.mDividends.size(), 1U);
    EXPECT_FALSE(result.mDividends.front().mEvent.mTaxPaid.has_value()); // blank: no tax paid
    EXPECT_EQ(result.mDividends.front().mEvent.mTaxCurrency, Currency::USD);
}

TEST(IncomePaymentNettingTest, TaxThatIsRefundedInFullIsAnExplicitZero) {
    const std::vector<IncomePaymentRow> rows{
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 5, "1")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 100'000, // USD 10.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1"},
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 6, "2")}},
         .mKind = IncomePaymentKind::Tax,
         .mIsin = "US0000000010",
         .mAmount = 15'000, // USD 1.50
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1"},
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 7, "3")}},
         .mKind = IncomePaymentKind::TaxRefund,
         .mIsin = "US0000000010",
         .mAmount = 15'000, // USD 1.50
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-19"),
         .mPaymentGroupId = "G1"},
    };

    const auto result = netIncomePayments(rows);

    ASSERT_EQ(result.mDividends.size(), 1U);
    EXPECT_EQ(result.mDividends.front().mEvent.mGrossAmount, 100'000);
    ASSERT_TRUE(result.mDividends.front().mEvent.mTaxPaid.has_value());
    EXPECT_EQ(*result.mDividends.front().mEvent.mTaxPaid, 0); // 1.50 - 1.50
}

TEST(IncomePaymentNettingTest, ARefundWithoutItsTaxRowIsRefusedBecauseTheTaxWouldBeBelowZero) {
    const std::vector<IncomePaymentRow> rows{
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 5, "1")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 100'000, // USD 10.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1"},
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 6, "2")}},
         .mKind = IncomePaymentKind::TaxRefund,
         .mIsin = "US0000000010",
         .mAmount = 15'000, // USD 1.50, and no tax row at all
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-19"),
         .mPaymentGroupId = "G1"},
    };

    const auto result = netIncomePayments(rows);

    EXPECT_TRUE(result.mDividends.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, MergeDiagnosticCode::IncomePaymentRowsDisagree);
}

TEST(IncomePaymentNettingTest, ARefundAboveTheTaxIsAnError) {
    const std::vector<IncomePaymentRow> rows{
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 5, "1")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 100'000, // USD 10.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1"},
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 6, "2")}},
         .mKind = IncomePaymentKind::Tax,
         .mIsin = "US0000000010",
         .mAmount = 10'000, // USD 1.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1"},
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 7, "3")}},
         .mKind = IncomePaymentKind::TaxRefund,
         .mIsin = "US0000000010",
         .mAmount = 15'000, // USD 1.50, more than the tax of 1.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-19"),
         .mPaymentGroupId = "G1"},
    };

    const auto result = netIncomePayments(rows);

    EXPECT_TRUE(result.mDividends.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mCode, MergeDiagnosticCode::IncomePaymentRowsDisagree);
    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::IncomePaymentRowsDisagree);
    EXPECT_EQ(diagnostic.mEventKinds,
              std::vector<StatementEventKind>{StatementEventKind::Dividend});
    EXPECT_EQ(diagnostic.mIsin, "US0000000010");
    EXPECT_EQ(diagnostic.mTaxDate, date("2024-03-15"));
    EXPECT_EQ(diagnostic.mSources.size(), 3U); // every row of the payment
}

TEST(IncomePaymentNettingTest, TwoDividendCurrenciesAreAnError) {
    const std::vector<IncomePaymentRow> rows{
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 5, "1")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 50'000, // USD 5.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1"},
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 6, "2")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 10'000, // EUR 1.00
         .mCurrency = Currency::EUR,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1"},
    };

    const auto result = netIncomePayments(rows);

    EXPECT_TRUE(result.mDividends.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, MergeDiagnosticCode::IncomePaymentRowsDisagree);
}

TEST(IncomePaymentNettingTest, AReversalInAnotherCurrencyThanTheDividendIsAnError) {
    const std::vector<IncomePaymentRow> rows{
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 5, "1")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 50'000, // USD 5.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1"},
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 6, "2")}},
         .mKind = IncomePaymentKind::DividendReversal,
         .mIsin = "US0000000010",
         .mAmount = 50'000, // EUR 5.00
         .mCurrency = Currency::EUR,
         .mBookedDate = date("2024-03-19"),
         .mPaymentGroupId = "G1"},
    };

    const auto result = netIncomePayments(rows);

    EXPECT_TRUE(result.mDividends.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, MergeDiagnosticCode::IncomePaymentRowsDisagree);
}

TEST(IncomePaymentNettingTest, TwoDividendPayDatesAreAnError) {
    const std::vector<IncomePaymentRow> rows{
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 5, "1")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 50'000, // USD 5.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1"},
        {.mMetadata = {.mTaxDate = date("2024-03-16"), .mSources = {ibkrSource(0, 6, "2")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 10'000, // USD 1.00, paid a day later
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1"},
    };

    const auto result = netIncomePayments(rows);

    EXPECT_TRUE(result.mDividends.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, MergeDiagnosticCode::IncomePaymentRowsDisagree);
}

TEST(IncomePaymentNettingTest, TwoTaxCurrenciesAreAnError) {
    const std::vector<IncomePaymentRow> rows{
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 5, "1")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 100'000, // USD 10.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1"},
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 6, "2")}},
         .mKind = IncomePaymentKind::Tax,
         .mIsin = "US0000000010",
         .mAmount = 15'000, // USD 1.50
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1"},
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 7, "3")}},
         .mKind = IncomePaymentKind::Tax,
         .mIsin = "US0000000010",
         .mAmount = 10'000, // EUR 1.00
         .mCurrency = Currency::EUR,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1"},
    };

    const auto result = netIncomePayments(rows);

    EXPECT_TRUE(result.mDividends.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, MergeDiagnosticCode::IncomePaymentRowsDisagree);
}

TEST(IncomePaymentNettingTest, AmountsThatDoNotFitInTheFixedPointAreAnError) {
    const std::vector<IncomePaymentRow> rows{
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 5, "1")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = std::numeric_limits<Money>::max(),
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1"},
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 6, "2")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 1, // one more than the largest amount
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1"},
    };

    const auto result = netIncomePayments(rows);

    EXPECT_TRUE(result.mDividends.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, MergeDiagnosticCode::IncomePaymentRowsDisagree);
}

TEST(IncomePaymentNettingTest, ADividendAndItsFullReversalGiveNoDividend) {
    const std::vector<IncomePaymentRow> rows{
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 5, "1")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 50'000, // USD 5.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1"},
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 6, "2")}},
         .mKind = IncomePaymentKind::DividendReversal,
         .mIsin = "US0000000010",
         .mAmount = 50'000, // USD 5.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-19"),
         .mPaymentGroupId = "G1"},
    };

    const auto result = netIncomePayments(rows);

    EXPECT_TRUE(result.mDividends.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U); // 5.00 - 5.00 = 0, so one warning

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mCode, MergeDiagnosticCode::DividendNetNotPositive);
    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::DividendNetNotPositive);
    EXPECT_EQ(diagnostic.mEventKinds,
              std::vector<StatementEventKind>{StatementEventKind::Dividend});
}

TEST(IncomePaymentNettingTest, AReversalAboveTheDividendGivesNoDividend) {
    const std::vector<IncomePaymentRow> rows{
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 5, "1")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 50'000, // USD 5.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1"},
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 6, "2")}},
         .mKind = IncomePaymentKind::DividendReversal,
         .mIsin = "US0000000010",
         .mAmount = 60'000, // USD 6.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-19"),
         .mPaymentGroupId = "G1"},
    };

    const auto result = netIncomePayments(rows);

    EXPECT_TRUE(result.mDividends.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U); // 5.00 - 6.00 is below 0
    EXPECT_EQ(result.mDiagnostics.front().mCode, MergeDiagnosticCode::DividendNetNotPositive);
}

TEST(IncomePaymentNettingTest, AReversalWithoutADividendRowIsOneWarning) {
    const std::vector<IncomePaymentRow> rows{
        {.mMetadata = {.mTaxDate = date("2024-12-03"), .mSources = {ibkrSource(1, 9, "7")}},
         .mKind = IncomePaymentKind::DividendReversal,
         .mIsin = "US0000000010",
         .mAmount = 40'000, // USD 4.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2025-01-10"),
         .mPaymentGroupId = "G2"},
    };

    const auto result = netIncomePayments(rows);

    EXPECT_TRUE(result.mDividends.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U); // and no "net not positive" warning too

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mCode, MergeDiagnosticCode::DividendReversalWithoutPayment);
    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::DividendReversalWithoutPayment);
    EXPECT_EQ(diagnostic.mTaxDate, date("2024-12-03"));
    EXPECT_EQ(diagnostic.mIsin, "US0000000010");
}

TEST(IncomePaymentNettingTest, ATaxRefundWithoutADividendRowIsOneWarning) {
    const std::vector<IncomePaymentRow> rows{
        {.mMetadata = {.mTaxDate = date("2024-12-03"), .mSources = {ibkrSource(0, 9, "7")}},
         .mKind = IncomePaymentKind::TaxRefund,
         .mIsin = "US0000000010",
         .mAmount = 1'500, // USD 0.15
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-12-04"),
         .mPaymentGroupId = "G2"},
    };

    const auto result = netIncomePayments(rows);

    EXPECT_TRUE(result.mDividends.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode,
              MergeDiagnosticCode::DividendReversalWithoutPayment);
}

TEST(IncomePaymentNettingTest, ATaxRowWithoutADividendRowIsOneWarning) {
    const std::vector<IncomePaymentRow> rows{
        {.mMetadata = {.mTaxDate = date("2024-12-03"), .mSources = {ibkrSource(0, 9, "7")}},
         .mKind = IncomePaymentKind::Tax,
         .mIsin = "US0000000010",
         .mAmount = 1'500, // USD 0.15
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-12-04"),
         .mPaymentGroupId = "G2"},
    };

    const auto result = netIncomePayments(rows);

    EXPECT_TRUE(result.mDividends.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode,
              MergeDiagnosticCode::DividendReversalWithoutPayment);
}

TEST(IncomePaymentNettingTest, APaymentInLieuIsOneDividendWithOneWarning) {
    const std::vector<IncomePaymentRow> rows{
        {.mMetadata = {.mTaxDate = date("2024-06-14"), .mSources = {ibkrSource(0, 31, "2006")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 30'000, // USD 3.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-06-17"),
         .mPaymentGroupId = "A9003",
         .mSubstitutePayment = true},
    };

    const auto result = netIncomePayments(rows);

    ASSERT_EQ(result.mDividends.size(), 1U);
    EXPECT_EQ(result.mDividends.front().mEvent.mGrossAmount, 30'000);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, MergeDiagnosticCode::SubstituteDividendPayment);
    EXPECT_EQ(result.mDiagnostics.front().mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(result.mDiagnostics.front().mMessageKey, MessageKey::SubstituteDividendPayment);
}

TEST(IncomePaymentNettingTest, ARowBookedInTheNextYearGivesOneWarningWithBothYears) {
    const std::vector<IncomePaymentRow> rows{
        {.mMetadata = {.mTaxDate = date("2024-12-20"), .mSources = {ibkrSource(0, 5, "1")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 100'000, // USD 10.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-12-23"),
         .mPaymentGroupId = "G1"},
        {.mMetadata = {.mTaxDate = date("2024-12-20"), .mSources = {ibkrSource(1, 5, "2")}},
         .mKind = IncomePaymentKind::Tax,
         .mIsin = "US0000000010",
         .mAmount = 15'000, // USD 1.50
         .mCurrency = Currency::USD,
         .mBookedDate = date("2025-01-02"),
         .mPaymentGroupId = "G1"},
        {.mMetadata = {.mTaxDate = date("2024-12-20"), .mSources = {ibkrSource(1, 6, "3")}},
         .mKind = IncomePaymentKind::TaxRefund,
         .mIsin = "US0000000010",
         .mAmount = 5'000, // USD 0.50
         .mCurrency = Currency::USD,
         .mBookedDate = date("2026-02-03"),
         .mPaymentGroupId = "G1"},
    };

    const auto result = netIncomePayments(rows);

    ASSERT_EQ(result.mDividends.size(), 1U);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mCode, MergeDiagnosticCode::IncomeCorrectionLaterYear);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::IncomeCorrectionLaterYear);
    // The pay year is 2024; the latest booked year is 2026 (the refund), not 2025.
    EXPECT_EQ(
        diagnostic.mMessageParams,
        (std::vector<MessageParam>{numberParam("payYear", 2024), numberParam("bookedYear", 2026)}));
    EXPECT_EQ(diagnostic.mSources.size(), 3U);
}

TEST(IncomePaymentNettingTest, ARowBookedInTheSameYearGivesNoWarning) {
    const std::vector<IncomePaymentRow> rows{
        {.mMetadata = {.mTaxDate = date("2024-12-20"), .mSources = {ibkrSource(0, 5, "1")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 100'000, // USD 10.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-12-31"),
         .mPaymentGroupId = "G1"},
    };

    const auto result = netIncomePayments(rows);

    ASSERT_EQ(result.mDividends.size(), 1U);
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(IncomePaymentNettingTest, ARowWithoutABookedDateGivesNoYearWarning) {
    const std::vector<IncomePaymentRow> rows{
        {.mMetadata = {.mTaxDate = date("2024-12-20"), .mSources = {ibkrSource(0, 5, "1")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 100'000, // USD 10.00
         .mCurrency = Currency::USD,
         .mPaymentGroupId = "G1"},
    };

    const auto result = netIncomePayments(rows);

    ASSERT_EQ(result.mDividends.size(), 1U);
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(IncomePaymentNettingTest, TheSameGroupIdWithAnotherIsinIsAnotherPayment) {
    const std::vector<IncomePaymentRow> rows{
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 5, "1")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 20'000, // USD 2.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1"},
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 6, "2")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000036",
         .mAmount = 30'000, // USD 3.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1"},
    };

    const auto result = netIncomePayments(rows);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mDividends.size(), 2U);
    EXPECT_EQ(result.mDividends.at(0).mIsin, "US0000000010");
    EXPECT_EQ(result.mDividends.at(0).mEvent.mGrossAmount, 20'000);
    EXPECT_EQ(result.mDividends.at(1).mIsin, "US0000000036");
    EXPECT_EQ(result.mDividends.at(1).mEvent.mGrossAmount, 30'000);
}

TEST(IncomePaymentNettingTest, TheSameGroupIdAndIsinOfTwoBrokersAreTwoPayments) {
    const std::vector<IncomePaymentRow> rows{
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 5, "1")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 20'000, // USD 2.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1"},
        {.mMetadata = {.mTaxDate = date("2024-03-15"),
                       .mSources = {tradeRepublicSource(1, 5, "1")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 30'000, // USD 3.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1"},
    };

    const auto result = netIncomePayments(rows);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mDividends.size(), 2U);
    // Trade Republic comes before Interactive Brokers in the broker order.
    EXPECT_EQ(result.mDividends.at(0).mEvent.mGrossAmount, 30'000);
    EXPECT_EQ(result.mDividends.at(1).mEvent.mGrossAmount, 20'000);
}

TEST(IncomePaymentNettingTest, TheInstrumentNameAndTheClassEvidenceOfTheRowsAreKept) {
    const std::vector<IncomePaymentRow> rows{
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 5, "1")}},
         .mKind = IncomePaymentKind::Dividend,
         .mName = "Synthetic A",
         .mIsin = "US0000000010",
         .mAmount = 20'000, // USD 2.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1",
         .mInstrumentEvidence = {{.mSource = ibkrSource(0, 5, "1"),
                                  .mName = "Synthetic A",
                                  .mAssetClass = AssetClass::Stock,
                                  .mAssetClassLabel = "STK"}}},
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 6, "2")}},
         .mKind = IncomePaymentKind::Tax,
         .mIsin = "US0000000010",
         .mAmount = 3'000, // USD 0.30
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1",
         .mInstrumentEvidence = {{.mSource = ibkrSource(0, 6, "2"),
                                  .mName = "",
                                  .mAssetClass = AssetClass::Stock,
                                  .mAssetClassLabel = "STK"}}},
    };

    const auto result = netIncomePayments(rows);

    ASSERT_EQ(result.mDividends.size(), 1U);
    EXPECT_EQ(result.mDividends.front().mName, "Synthetic A");
    EXPECT_EQ(result.mDividends.front().mEvent.mInstrumentEvidence.size(), 2U);
}

// The country of the payer: dividend and reversal rows name it, tax rows are not read

TEST(IncomePaymentNettingTest, TwoRowsWithTheSameCountryGiveThatCountry) {
    const std::vector<IncomePaymentRow> rows{
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 5, "1")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 120'000, // USD 12.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1",
         .mPayerCountry = "US"},
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 6, "2")}},
         .mKind = IncomePaymentKind::DividendReversal,
         .mIsin = "US0000000010",
         .mAmount = 20'000, // USD 2.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1",
         .mPayerCountry = "US"},
    };

    const auto result = netIncomePayments(rows);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mDividends.size(), 1U);
    EXPECT_EQ(result.mDividends.front().mEvent.mPayerCountry, "US");
}

TEST(IncomePaymentNettingTest, ARowWithoutACountryTakesTheCountryOfAnotherRow) {
    const std::vector<IncomePaymentRow> rows{
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 5, "1")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 120'000, // USD 12.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1",
         .mPayerCountry = std::nullopt},
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 6, "2")}},
         .mKind = IncomePaymentKind::DividendReversal,
         .mIsin = "US0000000010",
         .mAmount = 20'000, // USD 2.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1",
         .mPayerCountry = "US"},
    };

    const auto result = netIncomePayments(rows);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mDividends.size(), 1U);
    EXPECT_EQ(result.mDividends.front().mEvent.mPayerCountry, "US");
}

TEST(IncomePaymentNettingTest, NoRowWithACountryGivesNoCountry) {
    const std::vector<IncomePaymentRow> rows{
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 5, "1")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 120'000, // USD 12.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1",
         .mPayerCountry = std::nullopt},
    };

    const auto result = netIncomePayments(rows);

    ASSERT_EQ(result.mDividends.size(), 1U);
    EXPECT_FALSE(result.mDividends.front().mEvent.mPayerCountry.has_value());
}

TEST(IncomePaymentNettingTest, ADividendAndItsReversalWithDifferentCountriesAreAnError) {
    const std::vector<IncomePaymentRow> rows{
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 5, "1")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "DE0000000058",
         .mAmount = 50'000, // EUR 5.00
         .mCurrency = Currency::EUR,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1",
         .mPayerCountry = "DE"},
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 6, "2")}},
         .mKind = IncomePaymentKind::DividendReversal,
         .mIsin = "DE0000000058",
         .mAmount = 10'000, // EUR 1.00
         .mCurrency = Currency::EUR,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1",
         .mPayerCountry = "AT"},
    };

    const auto result = netIncomePayments(rows);

    EXPECT_TRUE(result.mDividends.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, MergeDiagnosticCode::IncomePaymentRowsDisagree);
    EXPECT_EQ(result.mDiagnostics.front().mSeverity, DiagnosticSeverity::Error);
}

TEST(IncomePaymentNettingTest, TheCountryOfATaxRowIsIgnored) {
    const std::vector<IncomePaymentRow> rows{
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 5, "1")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "DE0000000033",
         .mAmount = 100'000, // EUR 10.00
         .mCurrency = Currency::EUR,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1",
         .mPayerCountry = "DE"},
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 6, "2")}},
         .mKind = IncomePaymentKind::Tax,
         .mIsin = "DE0000000033",
         .mAmount = 26'400, // EUR 2.64
         .mCurrency = Currency::EUR,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1",
         .mPayerCountry = "CA"},
    };

    const auto result = netIncomePayments(rows);

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mDividends.size(), 1U);
    EXPECT_EQ(result.mDividends.front().mEvent.mPayerCountry, "DE"); // not the tax row's `CA`
}

// The merger with the fixtures

TEST(IncomePaymentMergeTest, ADividendItsReversalAndTheCorrectedDividendOfOneYearAreOneDividend) {
    const auto merged = mergeFixtures({"contract_example.xml"});

    // Dividend 2003 USD 5.00, reversal 2004 USD 5.00 and corrected dividend 2005 USD 4.50.
    const auto* dividend = findDividend(merged, "US0000000036", date("2024-05-10"));

    ASSERT_NE(dividend, nullptr);
    EXPECT_EQ(dividend->mGrossAmount, 45'000); // 5.00 - 5.00 + 4.50
    EXPECT_FALSE(dividend->mTaxPaid.has_value());
    EXPECT_EQ(dividend->mCurrency, Currency::USD);
    EXPECT_EQ(dividend->mMetadata.mSources.size(), 3U);
}

TEST(IncomePaymentMergeTest, ATaxRefundBookedInTheNextYearLowersTheTaxAndWarns) {
    const auto merged = mergeFixtures({"contract_example.xml"});

    // Dividend 2001 USD 12.00, tax 2002 USD 1.80, refund 2013 USD 0.30 booked on 2025-02-10.
    const auto* dividend = findDividend(merged, "US0000000010", date("2024-03-15"));

    ASSERT_NE(dividend, nullptr);
    EXPECT_EQ(dividend->mGrossAmount, 120'000);
    ASSERT_TRUE(dividend->mTaxPaid.has_value());
    EXPECT_EQ(*dividend->mTaxPaid, 15'000); // 1.80 - 0.30
    EXPECT_EQ(dividend->mMetadata.mSources.size(), 3U);

    const auto warnings =
        mergeDiagnosticsOf(merged, MergeDiagnosticCode::IncomeCorrectionLaterYear);

    ASSERT_EQ(warnings.size(), 1U);
    EXPECT_EQ(
        warnings.front().mMessageParams,
        (std::vector<MessageParam>{numberParam("payYear", 2024), numberParam("bookedYear", 2025)}));
}

TEST(IncomePaymentMergeTest, ThePaymentInLieuIsADividendWithOneWarning) {
    const auto merged = mergeFixtures({"supported.xml"});

    const auto* dividend = findDividend(merged, "US0000000010", date("2024-06-14"));

    ASSERT_NE(dividend, nullptr);
    EXPECT_EQ(dividend->mGrossAmount, 30'000);    // USD 3.00
    EXPECT_FALSE(dividend->mTaxPaid.has_value()); // no tax row: blank
    EXPECT_EQ(mergeDiagnosticsOf(merged, MergeDiagnosticCode::SubstituteDividendPayment).size(),
              1U);

    const auto* taxed = findDividend(merged, "US0000000010", date("2024-03-15"));

    ASSERT_NE(taxed, nullptr);
    EXPECT_EQ(taxed->mGrossAmount, 120'000); // USD 12.00
    ASSERT_TRUE(taxed->mTaxPaid.has_value());
    EXPECT_EQ(*taxed->mTaxPaid, 18'000); // USD 1.80
}

TEST(IncomePaymentMergeTest, TheCorrectionsOfTwoFilesAreOneDividendAndOneWarning) {
    const auto merged = mergeFixtures({"corrections_2024.xml", "corrections_2025.xml"});

    // Dividend 2101 USD 10.00 and tax 2102 USD 1.50 in 2024; reversal 2103 USD 10.00, corrected
    // dividend 2104 USD 9.00 and tax refund 2105 USD 0.15 in 2025.
    const auto* dividend = findDividend(merged, "US0000000036", date("2024-11-15"));

    ASSERT_NE(dividend, nullptr);
    EXPECT_EQ(dividend->mGrossAmount, 90'000); // 10.00 - 10.00 + 9.00
    ASSERT_TRUE(dividend->mTaxPaid.has_value());
    EXPECT_EQ(*dividend->mTaxPaid, 13'500); // 1.50 - 0.15
    EXPECT_EQ(dividend->mMetadata.mSources.size(), 5U);

    const auto laterYear =
        mergeDiagnosticsOf(merged, MergeDiagnosticCode::IncomeCorrectionLaterYear);

    ASSERT_EQ(laterYear.size(), 1U);
    EXPECT_EQ(
        laterYear.front().mMessageParams,
        (std::vector<MessageParam>{numberParam("payYear", 2024), numberParam("bookedYear", 2025)}));

    // The buy 6001 is in both files with the same facts: one trade.
    ASSERT_EQ(merged.mStatement.mPresentation.mTradeInstruments.size(), 1U);
    EXPECT_EQ(merged.mStatement.mPresentation.mTradeInstruments.front().mTransactions.size(), 1U);
}

TEST(IncomePaymentMergeTest, AReversalWhoseDividendIsInNoFileIsOneWarningAndNoDividend) {
    const auto merged = mergeFixtures({"corrections_2024.xml", "corrections_2025.xml"});

    // Reversal 2106 USD 4.00 of A9102 (US0000000010, paid 2024-12-03): the dividend is in no file.
    EXPECT_EQ(findDividend(merged, "US0000000010", date("2024-12-03")), nullptr);

    const auto warnings =
        mergeDiagnosticsOf(merged, MergeDiagnosticCode::DividendReversalWithoutPayment);

    ASSERT_EQ(warnings.size(), 1U);
    EXPECT_EQ(warnings.front().mIsin, "US0000000010");
    EXPECT_EQ(warnings.front().mTaxDate, date("2024-12-03"));
    EXPECT_TRUE(mergeDiagnosticsOf(merged, MergeDiagnosticCode::DividendNetNotPositive).empty());
    EXPECT_EQ(dividendCount(merged), 1U); // only the dividend of A9101
}

TEST(IncomePaymentMergeTest, TheSameFileLoadedTwiceGivesTheSameDividendOnce) {
    const auto merged = mergeFixtures({"supported.xml", "supported.xml"});

    const auto* dividend = findDividend(merged, "US0000000010", date("2024-03-15"));

    ASSERT_NE(dividend, nullptr);
    EXPECT_EQ(dividend->mGrossAmount, 120'000); // USD 12.00, not 24.00
    ASSERT_TRUE(dividend->mTaxPaid.has_value());
    EXPECT_EQ(*dividend->mTaxPaid, 18'000);             // USD 1.80, not 3.60
    EXPECT_EQ(dividend->mMetadata.mSources.size(), 4U); // two rows, each in two files
    EXPECT_TRUE(mergeDiagnosticsOf(merged, MergeDiagnosticCode::ConflictingDuplicate).empty());
    EXPECT_EQ(mergeDiagnosticsOf(merged, MergeDiagnosticCode::SubstituteDividendPayment).size(),
              1U);
}

// The merger with rows written in the test

TEST(IncomePaymentMergeTest, TheSameTransactionWithAnotherAmountInTwoFilesIsAConflict) {
    std::vector<StatementMergeInput> inputs;

    inputs.push_back(
        {0,
         ParseResult{.mBroker = Broker::InteractiveBrokers,
                     .mStatement = {.mIncomePaymentRows = {
                                        {.mMetadata = {.mTaxDate = date("2024-03-15"),
                                                       .mSources = {ibkrSource(0, 5, "2001")}},
                                         .mKind = IncomePaymentKind::Dividend,
                                         .mIsin = "US0000000010",
                                         .mAmount = 120'000, // USD 12.00
                                         .mCurrency = Currency::USD,
                                         .mBookedDate = date("2024-03-18"),
                                         .mPaymentGroupId = "G1"}}}}});
    inputs.push_back(
        {1,
         ParseResult{.mBroker = Broker::InteractiveBrokers,
                     .mStatement = {.mIncomePaymentRows = {
                                        {.mMetadata = {.mTaxDate = date("2024-03-15"),
                                                       .mSources = {ibkrSource(1, 5, "2001")}},
                                         .mKind = IncomePaymentKind::Dividend,
                                         .mIsin = "US0000000010",
                                         .mAmount = 130'000, // USD 13.00
                                         .mCurrency = Currency::USD,
                                         .mBookedDate = date("2024-03-18"),
                                         .mPaymentGroupId = "G1"}}}}});

    const auto merged = DeterministicStatementMerger{}.merge(std::move(inputs));
    const auto conflicts = mergeDiagnosticsOf(merged, MergeDiagnosticCode::ConflictingDuplicate);

    ASSERT_EQ(conflicts.size(), 1U);
    EXPECT_EQ(conflicts.front().mEventKinds,
              std::vector<StatementEventKind>{StatementEventKind::Dividend}); // scope: Doh-Div
    EXPECT_EQ(conflicts.front().mSources.size(), 2U);
    EXPECT_EQ(dividendCount(merged), 0U);
}

// The conflict rejects only the rows of that transaction. The other rows of the payment are still
// added up, so the dividend is made without the rejected tax row. The conflict error fails Doh-Div,
// so the partial dividend reaches no file.

TEST(IncomePaymentMergeTest, AConflictOnTheTaxRowLeavesTheDividendWithoutTax) {
    std::vector<StatementMergeInput> inputs;

    inputs.push_back(
        {0,
         ParseResult{.mBroker = Broker::InteractiveBrokers,
                     .mStatement = {.mIncomePaymentRows = {
                                        {.mMetadata = {.mTaxDate = date("2024-03-15"),
                                                       .mSources = {ibkrSource(0, 5, "2001")}},
                                         .mKind = IncomePaymentKind::Dividend,
                                         .mIsin = "US0000000010",
                                         .mAmount = 120'000, // USD 12.00
                                         .mCurrency = Currency::USD,
                                         .mBookedDate = date("2024-03-18"),
                                         .mPaymentGroupId = "G1"},
                                        {.mMetadata = {.mTaxDate = date("2024-03-15"),
                                                       .mSources = {ibkrSource(0, 6, "2002")}},
                                         .mKind = IncomePaymentKind::Tax,
                                         .mIsin = "US0000000010",
                                         .mAmount = 18'000, // USD 1.80
                                         .mCurrency = Currency::USD,
                                         .mBookedDate = date("2024-03-18"),
                                         .mPaymentGroupId = "G1"}}}}});
    inputs.push_back(
        {1,
         ParseResult{.mBroker = Broker::InteractiveBrokers,
                     .mStatement = {.mIncomePaymentRows = {
                                        {.mMetadata = {.mTaxDate = date("2024-03-15"),
                                                       .mSources = {ibkrSource(1, 5, "2001")}},
                                         .mKind = IncomePaymentKind::Dividend,
                                         .mIsin = "US0000000010",
                                         .mAmount = 120'000, // USD 12.00, the same as file 0
                                         .mCurrency = Currency::USD,
                                         .mBookedDate = date("2024-03-18"),
                                         .mPaymentGroupId = "G1"},
                                        {.mMetadata = {.mTaxDate = date("2024-03-15"),
                                                       .mSources = {ibkrSource(1, 6, "2002")}},
                                         .mKind = IncomePaymentKind::Tax,
                                         .mIsin = "US0000000010",
                                         .mAmount = 19'000, // USD 1.90, differs from file 0
                                         .mCurrency = Currency::USD,
                                         .mBookedDate = date("2024-03-18"),
                                         .mPaymentGroupId = "G1"}}}}});

    const auto merged = DeterministicStatementMerger{}.merge(std::move(inputs));
    const auto conflicts = mergeDiagnosticsOf(merged, MergeDiagnosticCode::ConflictingDuplicate);

    ASSERT_EQ(conflicts.size(), 1U);
    EXPECT_EQ(conflicts.front().mEventKinds,
              std::vector<StatementEventKind>{StatementEventKind::Dividend});

    ASSERT_EQ(dividendCount(merged), 1U);

    const auto* dividend = findDividend(merged, "US0000000010", date("2024-03-15"));

    ASSERT_NE(dividend, nullptr);
    EXPECT_EQ(dividend->mGrossAmount, 120'000); // USD 12.00 at 4 decimals
    EXPECT_FALSE(dividend->mTaxPaid.has_value());
}

// The same transaction ID in two files with one fact that differs is a conflict, whichever fact it
// is.

TEST(IncomePaymentMergeTest, AnotherKindOfTheSameTransactionIsAConflict) {
    const auto merged = mergeTwoCopies(
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 5, "2001")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 120'000, // USD 12.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1"},
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(1, 5, "2001")}},
         .mKind = IncomePaymentKind::Tax, // a tax row, not a dividend
         .mIsin = "US0000000010",
         .mAmount = 120'000,
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1"});

    EXPECT_EQ(mergeDiagnosticsOf(merged, MergeDiagnosticCode::ConflictingDuplicate).size(), 1U);
}

TEST(IncomePaymentMergeTest, AnotherCurrencyOfTheSameTransactionIsAConflict) {
    const auto merged = mergeTwoCopies(
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 5, "2001")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 120'000, // USD 12.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1"},
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(1, 5, "2001")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 120'000,
         .mCurrency = Currency::EUR, // EUR 12.00
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1"});

    EXPECT_EQ(mergeDiagnosticsOf(merged, MergeDiagnosticCode::ConflictingDuplicate).size(), 1U);
}

TEST(IncomePaymentMergeTest, AnotherCountryOfTheSameTransactionIsAConflict) {
    const auto merged = mergeTwoCopies(
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 5, "2001")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 120'000, // USD 12.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1",
         .mPayerCountry = "US"},
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(1, 5, "2001")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 120'000, // USD 12.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1",
         .mPayerCountry = std::nullopt});

    EXPECT_EQ(mergeDiagnosticsOf(merged, MergeDiagnosticCode::ConflictingDuplicate).size(), 1U);
}

TEST(IncomePaymentMergeTest, AnotherBookedDateOfTheSameTransactionIsAConflict) {
    const auto merged = mergeTwoCopies(
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 5, "2001")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 120'000, // USD 12.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1"},
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(1, 5, "2001")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 120'000,
         .mCurrency = Currency::USD,
         .mBookedDate = date("2025-01-20"), // booked later
         .mPaymentGroupId = "G1"});

    EXPECT_EQ(mergeDiagnosticsOf(merged, MergeDiagnosticCode::ConflictingDuplicate).size(), 1U);
}

TEST(IncomePaymentMergeTest, AnotherPaymentGroupOfTheSameTransactionIsAConflict) {
    const auto merged = mergeTwoCopies(
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 5, "2001")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 120'000, // USD 12.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1"},
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(1, 5, "2001")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 120'000,
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G2"});

    EXPECT_EQ(mergeDiagnosticsOf(merged, MergeDiagnosticCode::ConflictingDuplicate).size(), 1U);
}

TEST(IncomePaymentMergeTest, APaymentInLieuInOneCopyOnlyIsAConflict) {
    const auto merged = mergeTwoCopies(
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 5, "2001")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 120'000, // USD 12.00
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1",
         .mSubstitutePayment = false},
        {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(1, 5, "2001")}},
         .mKind = IncomePaymentKind::Dividend,
         .mIsin = "US0000000010",
         .mAmount = 120'000,
         .mCurrency = Currency::USD,
         .mBookedDate = date("2024-03-18"),
         .mPaymentGroupId = "G1",
         .mSubstitutePayment = true});

    EXPECT_EQ(mergeDiagnosticsOf(merged, MergeDiagnosticCode::ConflictingDuplicate).size(), 1U);
}

TEST(IncomePaymentMergeTest, TwoCopiesInOneFileAreOneRowWithOneWarning) {
    // The same row twice in file 0: merged into one row, with the duplicate-in-file warning.
    std::vector<StatementMergeInput> inputs;

    inputs.push_back(
        {0,
         ParseResult{.mBroker = Broker::InteractiveBrokers,
                     .mStatement = {.mIncomePaymentRows = {
                                        {.mMetadata = {.mTaxDate = date("2024-03-15"),
                                                       .mSources = {ibkrSource(0, 5, "2001")}},
                                         .mKind = IncomePaymentKind::Dividend,
                                         .mIsin = "US0000000010",
                                         .mAmount = 120'000, // USD 12.00
                                         .mCurrency = Currency::USD,
                                         .mBookedDate = date("2024-03-18"),
                                         .mPaymentGroupId = "G1"},
                                        {.mMetadata = {.mTaxDate = date("2024-03-15"),
                                                       .mSources = {ibkrSource(0, 9, "2001")}},
                                         .mKind = IncomePaymentKind::Dividend,
                                         .mIsin = "US0000000010",
                                         .mAmount = 120'000,
                                         .mCurrency = Currency::USD,
                                         .mBookedDate = date("2024-03-18"),
                                         .mPaymentGroupId = "G1"}}}}});

    const auto merged = DeterministicStatementMerger{}.merge(std::move(inputs));

    EXPECT_EQ(mergeDiagnosticsOf(merged, MergeDiagnosticCode::DuplicateTransactionInFile).size(),
              1U);
    ASSERT_EQ(dividendCount(merged), 1U);
    EXPECT_EQ(findDividend(merged, "US0000000010", date("2024-03-15"))->mGrossAmount, 120'000);
}

TEST(IncomePaymentMergeTest, ARowWithTheSourceOfAnotherFileIsRefused) {
    const auto merged = mergeOne(
        ParseResult{.mBroker = Broker::InteractiveBrokers,
                    .mStatement = {.mIncomePaymentRows = {
                                       {.mMetadata = {.mTaxDate = date("2024-03-15"),
                                                      .mSources = {ibkrSource(3, 5, "2001")}},
                                        .mKind = IncomePaymentKind::Dividend,
                                        .mIsin = "US0000000010",
                                        .mAmount = 120'000, // USD 12.00
                                        .mCurrency = Currency::USD,
                                        .mBookedDate = date("2024-03-18"),
                                        .mPaymentGroupId = "G1"}}}});

    // The input is file 0, but the row says file 3.
    EXPECT_EQ(mergeDiagnosticsOf(merged, MergeDiagnosticCode::InconsistentSourceIndex).size(), 1U);
    EXPECT_EQ(dividendCount(merged), 0U);
}

TEST(IncomePaymentMergeTest, ANettedDividendWithoutANameAddsNoNameConflictToAnotherBrokersName) {
    std::vector<StatementMergeInput> inputs;

    inputs.push_back(
        {0,
         ParseResult{
             .mBroker = Broker::TradeRepublic,
             .mStatement = {.mDividendInstruments = {{
                                .mName = "Synthetic A",
                                .mIsin = "US0000000010",
                                .mTransactions =
                                    {{.mMetadata = {.mTaxDate = date("2024-02-15"),
                                                    .mSources = {tradeRepublicSource(0, 5, "d-1")}},
                                      .mGrossAmount = 20'000, // USD 2.00
                                      .mCurrency = Currency::USD,
                                      .mTaxCurrency = Currency::USD}},
                            }}}}});
    inputs.push_back(
        {1,
         ParseResult{
             .mBroker = Broker::InteractiveBrokers,
             .mStatement = {.mIncomePaymentRows = {
                                {.mMetadata = {.mTaxDate = date("2024-03-15"),
                                               .mSources = {ibkrSource(1, 5, "2001")}},
                                 .mKind = IncomePaymentKind::Dividend,
                                 .mName = "",
                                 .mIsin = "US0000000010",
                                 .mAmount = 120'000, // USD 12.00
                                 .mCurrency = Currency::USD,
                                 .mBookedDate = date("2024-03-18"),
                                 .mPaymentGroupId = "G1",
                                 .mInstrumentEvidence = {{.mSource = ibkrSource(1, 5, "2001"),
                                                          .mName = "",
                                                          .mAssetClass = AssetClass::Stock,
                                                          .mAssetClassLabel = "STK"}}}}}}});

    const auto merged = DeterministicStatementMerger{}.merge(std::move(inputs));

    ASSERT_EQ(merged.mStatement.mPresentation.mDividendInstruments.size(), 1U);
    EXPECT_EQ(merged.mStatement.mPresentation.mDividendInstruments.front().mName, "Synthetic A");
    EXPECT_EQ(merged.mStatement.mPresentation.mDividendInstruments.front().mTransactions.size(),
              2U);
    EXPECT_TRUE(mergeDiagnosticsOf(merged, MergeDiagnosticCode::InstrumentNameConflict).empty());
}

TEST(IncomePaymentMergeTest, TwoBrokersPayingTheSameIsinOnTheSameDayAreTwoDividends) {
    // Both rows have the transaction ID "1" and the pay date 2024-03-15: the identity includes the
    // broker, so neither is a copy of the other.
    std::vector<StatementMergeInput> inputs;

    inputs.push_back(
        {0,
         ParseResult{
             .mBroker = Broker::TradeRepublic,
             .mStatement = {
                 .mDividendInstruments = {{
                     .mName = "Synthetic A",
                     .mIsin = "US0000000010",
                     .mTransactions = {{.mMetadata = {.mTaxDate = date("2024-03-15"),
                                                      .mSources = {tradeRepublicSource(0, 5, "1")}},
                                        .mGrossAmount = 20'000, // USD 2.00
                                        .mCurrency = Currency::USD,
                                        .mTaxCurrency = Currency::USD}},
                 }}}}});
    inputs.push_back(
        {1,
         ParseResult{.mBroker = Broker::InteractiveBrokers,
                     .mStatement = {
                         .mIncomePaymentRows = {{.mMetadata = {.mTaxDate = date("2024-03-15"),
                                                               .mSources = {ibkrSource(1, 5, "1")}},
                                                 .mKind = IncomePaymentKind::Dividend,
                                                 .mIsin = "US0000000010",
                                                 .mAmount = 120'000, // USD 12.00
                                                 .mCurrency = Currency::USD,
                                                 .mBookedDate = date("2024-03-15"),
                                                 .mPaymentGroupId = "G1"}}}}});

    const auto merged = DeterministicStatementMerger{}.merge(std::move(inputs));

    EXPECT_TRUE(mergeDiagnosticsOf(merged, MergeDiagnosticCode::ConflictingDuplicate).empty());
    EXPECT_TRUE(
        mergeDiagnosticsOf(merged, MergeDiagnosticCode::DuplicateTransactionInFile).empty());
    ASSERT_EQ(merged.mStatement.mPresentation.mDividendInstruments.size(), 1U);

    const auto& dividends =
        merged.mStatement.mPresentation.mDividendInstruments.front().mTransactions;

    ASSERT_EQ(dividends.size(), 2U);
    // Same day, no timestamps: the earlier file comes first.
    EXPECT_EQ(dividends.at(0).mGrossAmount, 20'000);  // Trade Republic, USD 2.00
    EXPECT_EQ(dividends.at(1).mGrossAmount, 120'000); // Interactive Brokers, USD 12.00
}

TEST(IncomePaymentMergeTest, TwoPaymentsOfOneBrokerOnTheSameDayAndIsinAreTwoDividends) {
    const auto merged = mergeOne(ParseResult{
        .mBroker = Broker::InteractiveBrokers,
        .mStatement = {
            .mIncomePaymentRows = {
                {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 5, "1")}},
                 .mKind = IncomePaymentKind::Dividend,
                 .mIsin = "US0000000010",
                 .mAmount = 120'000, // USD 12.00
                 .mCurrency = Currency::USD,
                 .mBookedDate = date("2024-03-15"),
                 .mPaymentGroupId = "G1"},
                {.mMetadata = {.mTaxDate = date("2024-03-15"), .mSources = {ibkrSource(0, 6, "2")}},
                 .mKind = IncomePaymentKind::Dividend,
                 .mIsin = "US0000000010",
                 .mAmount = 30'000, // USD 3.00, another payment (another group ID)
                 .mCurrency = Currency::USD,
                 .mBookedDate = date("2024-03-15"),
                 .mPaymentGroupId = "G2"}}}});

    EXPECT_EQ(dividendCount(merged), 2U);
    EXPECT_TRUE(mergeDiagnosticsOf(merged, MergeDiagnosticCode::ConflictingDuplicate).empty());
}

TEST(IncomePaymentMergeTest, ADividendOfTradeRepublicWithoutPaymentRowsIsUnchanged) {
    const auto merged = mergeOne(ParseResult{
        .mBroker = Broker::TradeRepublic,
        .mStatement = {
            .mDividendInstruments = {{
                .mName = "Synthetic A",
                .mIsin = "US0000000010",
                .mTransactions = {{.mMetadata = {.mTaxDate = date("2024-02-15"),
                                                 .mSources = {tradeRepublicSource(0, 5, "d-1")}},
                                   .mGrossAmount = 20'000, // USD 2.00
                                   .mCurrency = Currency::USD,
                                   .mTaxCurrency = Currency::USD}},
            }}}});

    EXPECT_TRUE(merged.mDiagnostics.empty());
    ASSERT_EQ(merged.mStatement.mPresentation.mDividendInstruments.size(), 1U);
    EXPECT_EQ(merged.mStatement.mPresentation.mDividendInstruments.front().mName, "Synthetic A");
    EXPECT_EQ(merged.mStatement.mPresentation.mDividendInstruments.front()
                  .mTransactions.front()
                  .mGrossAmount,
              20'000);
}

TEST(IncomePaymentMergeTest, ARowThatNamesTheInstrumentGivesTheNameToTheDividend) {
    const auto merged = mergeOne(
        ParseResult{.mBroker = Broker::InteractiveBrokers,
                    .mStatement = {.mIncomePaymentRows = {
                                       {.mMetadata = {.mTaxDate = date("2024-03-15"),
                                                      .mSources = {ibkrSource(0, 5, "2001")}},
                                        .mKind = IncomePaymentKind::Dividend,
                                        .mName = "Synthetic A",
                                        .mIsin = "US0000000010",
                                        .mAmount = 120'000, // USD 12.00
                                        .mCurrency = Currency::USD,
                                        .mBookedDate = date("2024-03-18"),
                                        .mPaymentGroupId = "G1"}}}});

    // A row that names the instrument and has no evidence gives the name to the dividend.
    ASSERT_EQ(merged.mStatement.mPresentation.mDividendInstruments.size(), 1U);
    EXPECT_EQ(merged.mStatement.mPresentation.mDividendInstruments.front().mName, "Synthetic A");
}

} // namespace
