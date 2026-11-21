#include "parsers/ibkr_parser.hpp"

#include "../support/large_allocation_hook.hpp"
#include "processors/statement_merger.hpp"
#include "taxbroker/diagnostic_messages.hpp"
#include "taxbroker/types.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// The expected values of the fixture tests are the tables in tests/test_data/ibkr/README.md. The
// rows written in a test are synthetic: made-up names, ISINs with valid check digits, no real
// value.

namespace {

using namespace std::chrono;
using taxbroker::AssetClass;
using taxbroker::BondRedemptionEvent;
using taxbroker::BondRedemptionKind;
using taxbroker::Broker;
using taxbroker::CashHolder;
using taxbroker::CorporateAction;
using taxbroker::CorporateActionType;
using taxbroker::Currency;
using taxbroker::DiagnosticCode;
using taxbroker::DiagnosticSeverity;
using taxbroker::IncomePaymentKind;
using taxbroker::IncomePaymentRow;
using taxbroker::InterestTransaction;
using taxbroker::LeftOutReason;
using taxbroker::MessageKey;
using taxbroker::ParseDiagnostic;
using taxbroker::ParsedRowKind;
using taxbroker::ParseResult;
using taxbroker::TradeSide;
using taxbroker::TradeTransaction;
using taxbroker::ibkr::IbkrParser;

std::filesystem::path fixtureFolder() {
    return std::filesystem::path{__FILE__}.parent_path().parent_path() / "test_data" / "ibkr";
}

/// Parses one fixture file with the real parser, as the application does.
ParseResult parseFixture(const std::string& aRelativePath) {
    IbkrParser parser;

    return parser.parseFile(fixtureFolder() / aRelativePath, 0);
}

/// Parses XML text written in a test.
ParseResult parseText(const std::string_view aXml) {
    IbkrParser parser;

    return parser.parse(aXml, "inline.xml", 0);
}

/// The file around the rows of a test: one statement of the account `U0000000`, entity `IB-IE`.
/// The first row of the trades section is on line 6.
std::string statementWithRows(const std::string_view aTrades,
                              const std::string_view aActions,
                              const std::string_view aCash) {
    std::string xml = "<FlexQueryResponse queryName=\"synthetic\" type=\"AF\">\n"
                      "  <FlexStatements count=\"1\">\n"
                      "    <FlexStatement accountId=\"U0000000\">\n"
                      "      <AccountInformation accountId=\"U0000000\" ibEntity=\"IB-IE\"/>\n"
                      "      <Trades>\n";

    xml += aTrades;
    xml += "\n      </Trades>\n      <CorporateActions>\n";
    xml += aActions;
    xml += "\n      </CorporateActions>\n      <CashTransactions>\n";
    xml += aCash;
    xml += "\n      </CashTransactions>\n    </FlexStatement>\n  </FlexStatements>\n"
           "</FlexQueryResponse>\n";

    return xml;
}

std::string transactionIdOf(const taxbroker::EventMetadata& aMetadata) {
    return aMetadata.mSources.front().mTransactionId.value_or("");
}

const TradeTransaction* findTrade(const ParseResult& aResult, const std::string_view aId) {
    for (const auto& instrument : aResult.mStatement.mTradeInstruments)
    {
        const auto found = std::find_if(instrument.mTransactions.begin(),
                                        instrument.mTransactions.end(),
                                        [aId](const TradeTransaction& aTrade) {
                                            return transactionIdOf(aTrade.mMetadata) == aId;
                                        });

        if (found != instrument.mTransactions.end())
        {
            return &*found;
        }
    }

    return nullptr;
}

const CorporateAction* findAction(const ParseResult& aResult, const std::string_view aId) {
    for (const auto& instrument : aResult.mStatement.mTradeInstruments)
    {
        const auto found = std::find_if(instrument.mCorporateActions.begin(),
                                        instrument.mCorporateActions.end(),
                                        [aId](const CorporateAction& aAction) {
                                            return transactionIdOf(aAction.mMetadata) == aId;
                                        });

        if (found != instrument.mCorporateActions.end())
        {
            return &*found;
        }
    }

    return nullptr;
}

const IncomePaymentRow* findPayment(const ParseResult& aResult, const std::string_view aId) {
    const auto& rows = aResult.mStatement.mIncomePaymentRows;
    const auto found = std::find_if(rows.begin(), rows.end(), [aId](const IncomePaymentRow& aRow) {
        return transactionIdOf(aRow.mMetadata) == aId;
    });

    return found == rows.end() ? nullptr : &*found;
}

const InterestTransaction* findInterest(const ParseResult& aResult, const std::string_view aId) {
    for (const auto& instrument : aResult.mStatement.mInterestInstruments)
    {
        const auto found = std::find_if(instrument.mTransactions.begin(),
                                        instrument.mTransactions.end(),
                                        [aId](const InterestTransaction& aInterest) {
                                            return transactionIdOf(aInterest.mMetadata) == aId;
                                        });

        if (found != instrument.mTransactions.end())
        {
            return &*found;
        }
    }

    return nullptr;
}

/// The diagnostics of one transaction ID, in order.
std::vector<const ParseDiagnostic*> diagnosticsOf(const ParseResult& aResult,
                                                  const std::string_view aId) {
    std::vector<const ParseDiagnostic*> found;

    for (const auto& diagnostic : aResult.mDiagnostics)
    {
        if (diagnostic.mTransactionId == aId)
        {
            found.push_back(&diagnostic);
        }
    }

    return found;
}

/// The diagnostics with one message key, in order (for the file-level notices).
std::vector<const ParseDiagnostic*> noticesOf(const ParseResult& aResult, const MessageKey aKey) {
    std::vector<const ParseDiagnostic*> found;

    for (const auto& diagnostic : aResult.mDiagnostics)
    {
        if (diagnostic.mMessageKey == aKey)
        {
            found.push_back(&diagnostic);
        }
    }

    return found;
}

/// The number of events and rows a file gave, of every kind.
std::size_t eventCount(const ParseResult& aResult) {
    const auto& statement = aResult.mStatement;
    const auto trades = std::accumulate(
        statement.mTradeInstruments.begin(),
        statement.mTradeInstruments.end(),
        std::size_t{0},
        [](const std::size_t aSum, const taxbroker::TradeInstrument& aInstrument) {
            return aSum + aInstrument.mTransactions.size() + aInstrument.mCorporateActions.size();
        });
    const auto dividends = std::accumulate(
        statement.mDividendInstruments.begin(),
        statement.mDividendInstruments.end(),
        std::size_t{0},
        [](const std::size_t aSum, const taxbroker::DividendInstrument& aInstrument) {
            return aSum + aInstrument.mTransactions.size();
        });
    const auto interest = std::accumulate(
        statement.mInterestInstruments.begin(),
        statement.mInterestInstruments.end(),
        std::size_t{0},
        [](const std::size_t aSum, const taxbroker::InterestInstrument& aInstrument) {
            return aSum + aInstrument.mTransactions.size();
        });

    return trades + dividends + interest + statement.mBondRedemptions.size() +
           statement.mBenefitEvents.size() + statement.mPrivateMarketEvents.size() +
           statement.mIncomePaymentRows.size() + statement.mLeftOutRows.size();
}

taxbroker::Date date(const int aYear, const unsigned aMonth, const unsigned aDay) {
    return taxbroker::Date{sys_days{year{aYear} / month{aMonth} / day{aDay}}.time_since_epoch()};
}

/// The UTC instant with the given calendar fields, to write the expected values as dates.
taxbroker::SourceTimestamp utc(const int aYear,
                               const unsigned aMonth,
                               const unsigned aDay,
                               const int aHour,
                               const int aMinute,
                               const int aSecond) {
    const sys_seconds seconds = sys_days{year{aYear} / month{aMonth} / day{aDay}} + hours{aHour} +
                                minutes{aMinute} + std::chrono::seconds{aSecond};

    return taxbroker::SourceTimestamp{duration_cast<milliseconds>(seconds.time_since_epoch())};
}

const ParseResult& supported() {
    static const ParseResult result = parseFixture("supported.xml");

    return result;
}

const ParseResult& ignored() {
    static const ParseResult result = parseFixture("ignored.xml");

    return result;
}

const ParseResult& rejectedRows() {
    static const ParseResult result = parseFixture("rejected_rows.xml");

    return result;
}

const ParseResult& timestamps() {
    static const ParseResult result = parseFixture("timestamps.xml");

    return result;
}

const ParseResult& quantities() {
    static const ParseResult result = parseFixture("quantities.xml");

    return result;
}

} // namespace

// supported.xml: trades

TEST(IbkrParserSupportedTest, TheFileGivesNoDiagnosticAtAll) {
    EXPECT_TRUE(supported().mDiagnostics.empty());
    EXPECT_EQ(supported().mBroker, Broker::InteractiveBrokers);
}

TEST(IbkrParserSupportedTest, TheFileGivesTheExpectedNumberOfEvents) {
    const auto& statement = supported().mStatement;

    // 7 trades, 3 payment rows (2001, 2002, 2006), 1 broker interest, 1 coupon, 3 splits, 1 bond
    // maturity. Row 2010 (accrued interest paid) gives no event.
    EXPECT_EQ(statement.mIncomePaymentRows.size(), 3U);
    EXPECT_EQ(statement.mBondRedemptions.size(), 1U);
    EXPECT_TRUE(statement.mLeftOutRows.empty());
    EXPECT_TRUE(statement.mDividendInstruments.empty());
    EXPECT_EQ(statement.mInterestInstruments.size(), 2U);
    EXPECT_EQ(statement.mTradeInstruments.size(), 5U);
    EXPECT_EQ(eventCount(supported()), 7U + 3U + 1U + 1U + 1U + 3U);
}

TEST(IbkrParserSupportedTest, BuyOfAShare1001) {
    const auto* trade = findTrade(supported(), "1001");

    ASSERT_NE(trade, nullptr);

    EXPECT_EQ(trade->mTradeSide, TradeSide::Buy);
    EXPECT_EQ(trade->mUnits, 1'000'000'000);      // 10 units at 8 decimals
    EXPECT_EQ(trade->mUnitPrice, 15'025'000'000); // USD 150.25 at 8 decimals
    EXPECT_EQ(trade->mAmount, 15'025'000);        // |proceeds| USD 1502.50 at 4 decimals
    EXPECT_EQ(trade->mFeePaid, 10'000);           // |ibCommission| USD 1.00
    EXPECT_EQ(trade->mTransactionTaxPaid, 0);     // taxes="0": a known zero
    EXPECT_EQ(trade->mCurrency, Currency::USD);
    EXPECT_EQ(trade->mFeeCurrency, Currency::USD);
    EXPECT_FALSE(trade->mExchangeRate.has_value()); // fxRateToBase is not read
    EXPECT_EQ(trade->mMetadata.mTaxDate, date(2024, 3, 5));
    EXPECT_EQ(trade->mMetadata.mSourceTimestamp, utc(2024, 3, 5, 20, 42, 10)); // 15:42:10 EST
    EXPECT_FALSE(trade->mMetadata.mOrderingTimestamp.has_value());
}

TEST(IbkrParserSupportedTest, BuyOfAShareKeepsItsSourceAndEvidence) {
    const auto* trade = findTrade(supported(), "1001");

    ASSERT_NE(trade, nullptr);
    ASSERT_EQ(trade->mMetadata.mSources.size(), 1U);

    const auto& source = trade->mMetadata.mSources.front();

    EXPECT_EQ(source.mBroker, Broker::InteractiveBrokers);
    EXPECT_EQ(source.mFilename.value(), "supported.xml");
    EXPECT_EQ(source.mSourceRow, 6U); // the line of the element in the file
    EXPECT_EQ(source.mTransactionId, "1001");
    EXPECT_EQ(source.mInputSequence.mSourceIndex, 0U);
    EXPECT_EQ(source.mInputSequence.mEventIndex, 0U); // the first row element of the file

    ASSERT_EQ(trade->mUnitEvidence.size(), 1U);
    EXPECT_EQ(trade->mUnitEvidence.front().mSourceText, "10");
    EXPECT_EQ(trade->mUnitEvidence.front().mCanonicalValue, "10");
    EXPECT_EQ(trade->mUnitEvidence.front().mDiscardedDigits, "");
    EXPECT_FALSE(trade->mUnitEvidence.front().mRoundedAwayFromZero);

    ASSERT_EQ(trade->mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(trade->mInstrumentEvidence.front().mName, "SYNTHETIC EXAMPLE A INC");
    EXPECT_EQ(trade->mInstrumentEvidence.front().mAssetClass, AssetClass::Stock);
    EXPECT_EQ(trade->mInstrumentEvidence.front().mAssetClassLabel, "STK");
}

TEST(IbkrParserSupportedTest, FractionalSell1002) {
    const auto* trade = findTrade(supported(), "1002");

    ASSERT_NE(trade, nullptr);

    EXPECT_EQ(trade->mTradeSide, TradeSide::Sell);
    EXPECT_EQ(trade->mUnits, 50'000'000);         // 0.5 units; quantity -0.5 turned around
    EXPECT_EQ(trade->mUnitPrice, 20'000'000'000); // USD 200
    EXPECT_EQ(trade->mAmount, 1'000'000);         // USD 100.00
    EXPECT_EQ(trade->mCurrency, Currency::USD);
    EXPECT_EQ(trade->mFeePaid, 3'500); // EUR 0.35: the fee is in another currency
    EXPECT_EQ(trade->mFeeCurrency, Currency::EUR);
    EXPECT_EQ(trade->mMetadata.mTaxDate, date(2024, 6, 10));
    EXPECT_EQ(trade->mMetadata.mSourceTimestamp, utc(2024, 6, 10, 14, 15, 0)); // 10:15 EDT
    EXPECT_EQ(trade->mMetadata.mSources.front().mSourceRow, 7U);
    EXPECT_EQ(trade->mUnitEvidence.front().mSourceText, "-0.5"); // the source sign is kept
}

TEST(IbkrParserSupportedTest, BuyOfAnEtfIsAFund1003) {
    const auto* trade = findTrade(supported(), "1003");

    ASSERT_NE(trade, nullptr);

    EXPECT_EQ(trade->mTradeSide, TradeSide::Buy);
    EXPECT_EQ(trade->mUnits, 400'000'000);       // 4 units
    EXPECT_EQ(trade->mUnitPrice, 5'000'000'000); // EUR 50
    EXPECT_EQ(trade->mAmount, 2'000'000);        // EUR 200.00
    EXPECT_EQ(trade->mFeePaid, 12'500);          // EUR 1.25
    EXPECT_EQ(trade->mCurrency, Currency::EUR);
    EXPECT_EQ(trade->mMetadata.mTaxDate, date(2024, 4, 2));
    EXPECT_EQ(trade->mMetadata.mSourceTimestamp, utc(2024, 4, 2, 8, 0, 0)); // 04:00 EDT
    EXPECT_EQ(trade->mInstrumentEvidence.front().mAssetClass, AssetClass::Fund);
    EXPECT_EQ(trade->mInstrumentEvidence.front().mAssetClassLabel, "STK");
}

TEST(IbkrParserSupportedTest, BuyOfABondStoresNominalUnitsAndAFractionPrice1008) {
    const auto* trade = findTrade(supported(), "1008");

    ASSERT_NE(trade, nullptr);

    EXPECT_EQ(trade->mTradeSide, TradeSide::Buy);
    EXPECT_EQ(trade->mUnits, 100'000'000'000); // 1000 units of nominal
    // tradePrice 99.5 x multiplier 0.01 = 0.995 per unit, at 8 decimals.
    EXPECT_EQ(trade->mUnitPrice, 99'500'000);
    EXPECT_EQ(trade->mAmount, 9'950'000); // EUR 995.00 = 1000 x 0.995
    EXPECT_EQ(trade->mFeePaid, 20'000);   // EUR 2.00
    EXPECT_EQ(trade->mCurrency, Currency::EUR);
    EXPECT_EQ(trade->mInstrumentEvidence.front().mAssetClass, AssetClass::Bond);
}

TEST(IbkrParserSupportedTest, BuyOfAFundWithoutFee1009) {
    const auto* trade = findTrade(supported(), "1009");

    ASSERT_NE(trade, nullptr);

    EXPECT_EQ(trade->mUnits, 300'000'000);        // 3 units
    EXPECT_EQ(trade->mUnitPrice, 10'000'000'000); // EUR 100
    EXPECT_EQ(trade->mAmount, 3'000'000);         // EUR 300.00
    EXPECT_EQ(trade->mFeePaid, 0);                // ibCommission="0": a known zero fee
    EXPECT_EQ(trade->mFeeCurrency, Currency::EUR);
    EXPECT_EQ(trade->mInstrumentEvidence.front().mAssetClass, AssetClass::Fund);
    EXPECT_EQ(trade->mInstrumentEvidence.front().mAssetClassLabel, "FUND");
}

TEST(IbkrParserSupportedTest, BuyWithATransactionTaxHasASecondFee1010) {
    const auto* trade = findTrade(supported(), "1010");

    ASSERT_NE(trade, nullptr);

    EXPECT_EQ(trade->mFeePaid, 12'500); // ibCommission -1.25: EUR 1.25
    EXPECT_EQ(trade->mFeeCurrency, Currency::EUR);
    EXPECT_EQ(trade->mTransactionTaxPaid, 8'000); // taxes -0.8: EUR 0.80 in the trade currency
}

TEST(IbkrParserSupportedTest, BuyInTheSecondAccountJoinsTheSameInstrument5001) {
    const auto* trade = findTrade(supported(), "5001");

    ASSERT_NE(trade, nullptr);

    EXPECT_EQ(trade->mUnits, 200'000'000);        // 2 units
    EXPECT_EQ(trade->mUnitPrice, 16'000'000'000); // USD 160
    EXPECT_EQ(trade->mAmount, 3'200'000);         // USD 320.00
    EXPECT_EQ(trade->mFeePaid, 10'000);           // USD 1.00
    EXPECT_EQ(trade->mMetadata.mTaxDate, date(2024, 7, 1));
    EXPECT_EQ(trade->mMetadata.mSourceTimestamp, utc(2024, 7, 1, 15, 30, 0)); // 11:30 EDT
    // 1001, 1002 and 5001 are in one instrument; the account is never in the event.
    EXPECT_EQ(trade->mMetadata.mSources.front().mSourceRow, 30U);
    EXPECT_EQ(trade->mMetadata.mSources.front().mInputSequence.mEventIndex, 15U);

    const auto& instruments = supported().mStatement.mTradeInstruments;
    const auto shared = std::find_if(instruments.begin(),
                                     instruments.end(),
                                     [](const taxbroker::TradeInstrument& aInstrument) {
                                         return aInstrument.mIsin == "US0000000010";
                                     });

    ASSERT_NE(shared, instruments.end());

    EXPECT_EQ(shared->mTransactions.size(), 3U);
    EXPECT_EQ(shared->mCorporateActions.size(), 2U);
    EXPECT_EQ(shared->mAssetClass, AssetClass::Stock);
    EXPECT_EQ(shared->mName, "SYNTHETIC EXAMPLE A INC");
}

// supported.xml: cash rows

TEST(IbkrParserSupportedTest, DividendRow2001) {
    const auto* row = findPayment(supported(), "2001");

    ASSERT_NE(row, nullptr);

    EXPECT_EQ(row->mKind, IncomePaymentKind::Dividend);
    EXPECT_EQ(row->mIsin, "US0000000010");
    EXPECT_EQ(row->mAmount, 120'000); // USD 12.00 at 4 decimals
    EXPECT_EQ(row->mCurrency, Currency::USD);
    EXPECT_EQ(row->mPaymentGroupId, "A9001");
    EXPECT_FALSE(row->mSubstitutePayment);
    EXPECT_EQ(row->mMetadata.mTaxDate, date(2024, 3, 15));     // dateTime, not reportDate
    EXPECT_EQ(row->mBookedDate, date(2024, 3, 18));            // reportDate
    EXPECT_FALSE(row->mMetadata.mSourceTimestamp.has_value()); // the cash row has a date only
    EXPECT_EQ(row->mMetadata.mSources.front().mSourceRow, 19U);
}

TEST(IbkrParserSupportedTest, TaxRowOfTheDividendIsStoredPositive2002) {
    const auto* row = findPayment(supported(), "2002");

    ASSERT_NE(row, nullptr);

    EXPECT_EQ(row->mKind, IncomePaymentKind::Tax);
    EXPECT_EQ(row->mIsin, "US0000000010");
    EXPECT_EQ(row->mAmount, 18'000); // amount -1.8 turned around: USD 1.80
    EXPECT_EQ(row->mCurrency, Currency::USD);
    EXPECT_EQ(row->mPaymentGroupId, "A9001"); // the same payment as 2001
    EXPECT_EQ(row->mMetadata.mTaxDate, date(2024, 3, 15));
}

TEST(IbkrParserSupportedTest, PaymentInLieuOfADividendIsFlagged2006) {
    const auto* row = findPayment(supported(), "2006");

    ASSERT_NE(row, nullptr);

    EXPECT_EQ(row->mKind, IncomePaymentKind::Dividend);
    EXPECT_EQ(row->mAmount, 30'000); // USD 3.00
    EXPECT_EQ(row->mPaymentGroupId, "A9003");
    EXPECT_TRUE(row->mSubstitutePayment);
    EXPECT_EQ(row->mMetadata.mTaxDate, date(2024, 6, 14));
}

TEST(IbkrParserSupportedTest, BrokerInterestNamesThePayerEntity2007) {
    const auto* interest = findInterest(supported(), "2007");

    ASSERT_NE(interest, nullptr);

    EXPECT_EQ(interest->mGrossAmount, 32'100); // EUR 3.21
    EXPECT_EQ(interest->mCurrency, Currency::EUR);
    EXPECT_EQ(interest->mTaxCurrency, Currency::EUR);
    EXPECT_FALSE(interest->mTaxPaid.has_value()); // no tax row: blank, not zero
    EXPECT_FALSE(interest->mExchangeRate.has_value());
    EXPECT_EQ(interest->mCashHolder, CashHolder::Broker);
    EXPECT_EQ(interest->mPayerCode, "IB-IE"); // ibEntity of the account `U0000000`
    EXPECT_EQ(interest->mMetadata.mTaxDate, date(2024, 4, 3));
    EXPECT_FALSE(interest->mMetadata.mSourceTimestamp.has_value());
    EXPECT_TRUE(interest->mInstrumentEvidence.empty());
}

TEST(IbkrParserSupportedTest, BrokerInterestHasNoInstrument) {
    const auto& instruments = supported().mStatement.mInterestInstruments;
    const auto cash = std::find_if(instruments.begin(),
                                   instruments.end(),
                                   [](const taxbroker::InterestInstrument& aInstrument) {
                                       return aInstrument.mInterestType ==
                                              taxbroker::InterestType::BrokerInterest;
                                   });

    ASSERT_NE(cash, instruments.end());

    EXPECT_FALSE(cash->mIsin.has_value());
    EXPECT_EQ(cash->mName, "");
    EXPECT_EQ(cash->mTransactions.size(), 1U);
}

TEST(IbkrParserSupportedTest, BondCouponIsBondInterestWithoutCashHolder2009) {
    const auto* interest = findInterest(supported(), "2009");

    ASSERT_NE(interest, nullptr);

    EXPECT_EQ(interest->mGrossAmount, 450'000); // EUR 45.00
    EXPECT_EQ(interest->mCurrency, Currency::EUR);
    EXPECT_FALSE(interest->mCashHolder.has_value());
    EXPECT_FALSE(interest->mPayerCode.has_value()); // the issuer is found by ISIN
    EXPECT_FALSE(interest->mTaxPaid.has_value());
    EXPECT_EQ(interest->mMetadata.mTaxDate, date(2024, 6, 17));
    ASSERT_EQ(interest->mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(interest->mInstrumentEvidence.front().mAssetClass, AssetClass::Bond);

    const auto& instruments = supported().mStatement.mInterestInstruments;
    const auto bond =
        std::find_if(instruments.begin(),
                     instruments.end(),
                     [](const taxbroker::InterestInstrument& aInstrument) {
                         return aInstrument.mInterestType == taxbroker::InterestType::BondInterest;
                     });

    ASSERT_NE(bond, instruments.end());

    EXPECT_EQ(bond->mIsin, "DE0000000041");
}

TEST(IbkrParserSupportedTest, AccruedInterestPaidGivesNoEvent2010) {
    EXPECT_EQ(findPayment(supported(), "2010"), nullptr);
    EXPECT_EQ(findInterest(supported(), "2010"), nullptr);
    EXPECT_TRUE(diagnosticsOf(supported(), "2010").empty());
}

// supported.xml: corporate actions

TEST(IbkrParserSupportedTest, ForwardSplitIsAnUnresolvedSplit3001And5002) {
    const auto* first = findAction(supported(), "3001");
    const auto* second = findAction(supported(), "5002");

    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    EXPECT_EQ(first->mType, CorporateActionType::UnresolvedSplit);
    EXPECT_FALSE(first->mRatio.has_value());      // "SPLIT 4 FOR 1" is evidence, never the ratio
    EXPECT_EQ(first->mUnitsDelta, 2'850'000'000); // quantity 28.5
    EXPECT_EQ(first->mMetadata.mTaxDate, date(2024, 8, 1));
    EXPECT_EQ(first->mMetadata.mSourceTimestamp, utc(2024, 8, 2, 0, 25, 0)); // 20:25 EDT
    ASSERT_EQ(first->mUnitEvidence.size(), 1U);
    EXPECT_EQ(first->mUnitEvidence.front().mSourceText, "28.5");
    EXPECT_TRUE(first->mInstrumentEvidence.empty()); // a corporate action row gives no class

    EXPECT_EQ(second->mType, CorporateActionType::UnresolvedSplit);
    EXPECT_EQ(second->mUnitsDelta, 600'000'000); // quantity 6
    EXPECT_EQ(second->mMetadata.mTaxDate, date(2024, 8, 1));
    EXPECT_EQ(second->mMetadata.mSourceTimestamp, utc(2024, 8, 2, 0, 25, 0));
    EXPECT_EQ(second->mUnitEvidence.front().mSourceText, "6");
}

TEST(IbkrParserSupportedTest, ReverseSplitIsAnUnresolvedSplit3004) {
    const auto* action = findAction(supported(), "3004");

    ASSERT_NE(action, nullptr);

    EXPECT_EQ(action->mType, CorporateActionType::UnresolvedSplit);
    EXPECT_FALSE(action->mRatio.has_value());
    EXPECT_EQ(action->mUnitsDelta, -300'000'000); // quantity -3: evidence, not a direction
    EXPECT_EQ(action->mMetadata.mTaxDate, date(2024, 10, 14));
    EXPECT_EQ(action->mMetadata.mSourceTimestamp, utc(2024, 10, 15, 0, 25, 0)); // 20:25 EDT
    EXPECT_EQ(action->mUnitEvidence.front().mSourceText, "-3");
}

TEST(IbkrParserSupportedTest, SplitOfAnEtfKeepsTheFundClassOfItsTrades) {
    const auto& instruments = supported().mStatement.mTradeInstruments;
    const auto etf = std::find_if(instruments.begin(),
                                  instruments.end(),
                                  [](const taxbroker::TradeInstrument& aInstrument) {
                                      return aInstrument.mIsin == "IE0000000020";
                                  });

    ASSERT_NE(etf, instruments.end());

    // The split row says `STK` and has no `subCategory`; it must not turn the fund into a share.
    EXPECT_EQ(etf->mAssetClass, AssetClass::Fund);
    EXPECT_EQ(etf->mTransactions.size(), 1U);
    EXPECT_EQ(etf->mCorporateActions.size(), 1U);
}

TEST(IbkrParserSupportedTest, BondMaturityIsARedemptionEvent3003) {
    const auto& redemptions = supported().mStatement.mBondRedemptions;

    ASSERT_EQ(redemptions.size(), 1U);

    const auto& redemption = redemptions.front();

    EXPECT_EQ(redemption.mKind, BondRedemptionKind::Maturity); // quantity -1000: units leave
    EXPECT_EQ(redemption.mIsin, "DE0000000041");
    EXPECT_EQ(redemption.mName, "SYNBOND 4.5% 2024");
    EXPECT_EQ(redemption.mUnits, 100'000'000'000); // 1000 units
    EXPECT_EQ(redemption.mAmount, 10'000'000);     // EUR 1000.00
    EXPECT_EQ(redemption.mFeePaid, 0);
    EXPECT_EQ(redemption.mCurrency, Currency::EUR);
    EXPECT_EQ(redemption.mFeeCurrency, Currency::EUR);
    EXPECT_FALSE(redemption.mTaxPaid.has_value());
    EXPECT_FALSE(redemption.mRedemptionPrice.has_value());
    EXPECT_EQ(redemption.mMetadata.mTaxDate, date(2024, 12, 16));
    EXPECT_EQ(redemption.mMetadata.mSourceTimestamp, utc(2024, 12, 17, 1, 25, 0)); // 20:25 EST
    EXPECT_EQ(transactionIdOf(redemption.mMetadata), "3003");
}

// ignored.xml

TEST(IbkrParserIgnoredTest, TheFileGivesNoEvent) {
    const auto& statement = ignored().mStatement;

    EXPECT_TRUE(statement.mTradeInstruments.empty());
    EXPECT_TRUE(statement.mDividendInstruments.empty());
    EXPECT_TRUE(statement.mInterestInstruments.empty());
    EXPECT_TRUE(statement.mBondRedemptions.empty());
    EXPECT_TRUE(statement.mIncomePaymentRows.empty());
    EXPECT_EQ(eventCount(ignored()), statement.mLeftOutRows.size());
}

TEST(IbkrParserIgnoredTest, OptionFutureCfdAndMarginInterestAreLeftOut) {
    const auto& rows = ignored().mStatement.mLeftOutRows;

    ASSERT_EQ(rows.size(), 4U);

    // The rows 1004 (option), 1022 (future), 1023 (CFD) and 2008 (broker interest paid).
    EXPECT_EQ(transactionIdOf(rows[0].mMetadata), "1004");
    EXPECT_EQ(transactionIdOf(rows[1].mMetadata), "1022");
    EXPECT_EQ(transactionIdOf(rows[2].mMetadata), "1023");
    EXPECT_EQ(transactionIdOf(rows[3].mMetadata), "2008");
    EXPECT_EQ(rows[0].mReason, LeftOutReason::MarginOrLeverage);
    EXPECT_EQ(rows[3].mReason, LeftOutReason::MarginOrLeverage);
    EXPECT_EQ(rows[0].mMetadata.mSources.front().mSourceRow, 9U);
}

TEST(IbkrParserIgnoredTest, LeftOutRowsHaveNoDiagnostic) {
    EXPECT_TRUE(diagnosticsOf(ignored(), "1004").empty());
    EXPECT_TRUE(diagnosticsOf(ignored(), "1022").empty());
    EXPECT_TRUE(diagnosticsOf(ignored(), "1023").empty());
    EXPECT_TRUE(diagnosticsOf(ignored(), "2008").empty());
}

TEST(IbkrParserIgnoredTest, TheLotAndTheOrderAreOneFileLevelNotice) {
    const auto& diagnostics = ignored().mDiagnostics;
    std::vector<const ParseDiagnostic*> notices;

    for (const auto& diagnostic : diagnostics)
    {
        if (diagnostic.mMessageKey == MessageKey::UnsupportedRowTypeNotExecution)
        {
            notices.push_back(&diagnostic);
        }
    }

    ASSERT_EQ(notices.size(), 1U);

    EXPECT_EQ(notices.front()->mSeverity, DiagnosticSeverity::Info);
    EXPECT_EQ(notices.front()->mCode, DiagnosticCode::UnsupportedRowType);
    EXPECT_EQ(notices.front()->mSourceFile, "ignored.xml");
    EXPECT_FALSE(notices.front()->mRowIndex.has_value());
    EXPECT_FALSE(notices.front()->mRowKind.has_value()); // it affects no form
    ASSERT_EQ(notices.front()->mMessageParams.size(), 1U);
    EXPECT_EQ(notices.front()->mMessageParams.front(), taxbroker::numberParam("count", 2));
}

TEST(IbkrParserIgnoredTest, TheCurrencyConversionIsOneFileLevelNoticeWithoutForm1006) {
    const auto notices = noticesOf(ignored(), MessageKey::UnsupportedRowTypeCurrencyConversion);

    ASSERT_EQ(notices.size(), 1U);

    EXPECT_EQ(notices.front()->mSeverity, DiagnosticSeverity::Info);
    EXPECT_EQ(notices.front()->mCode, DiagnosticCode::UnsupportedRowType);
    EXPECT_EQ(notices.front()->mSourceFile, "ignored.xml");
    EXPECT_FALSE(notices.front()->mRowIndex.has_value());
    EXPECT_FALSE(notices.front()->mRowKind.has_value()); // it affects no form
    ASSERT_EQ(notices.front()->mMessageParams.size(), 1U);
    EXPECT_EQ(notices.front()->mMessageParams.front(), taxbroker::numberParam("count", 1));
}

TEST(IbkrParserIgnoredTest, TwoCurrencyConversionsAreOneNoticeWithTheCountTwo) {
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T40" levelOfDetail="EXECUTION" assetCategory="CASH" description="EUR.USD"/>
        <Trade transactionID="T41" levelOfDetail="EXECUTION" assetCategory="CASH" description="USD.EUR"/>)",
        "",
        ""));

    EXPECT_EQ(eventCount(result), 0U);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mSeverity, DiagnosticSeverity::Info);
    EXPECT_EQ(result.mDiagnostics.front().mMessageKey,
              MessageKey::UnsupportedRowTypeCurrencyConversion);
    EXPECT_FALSE(result.mDiagnostics.front().mRowIndex.has_value());
    ASSERT_EQ(result.mDiagnostics.front().mMessageParams.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mMessageParams.front(),
              taxbroker::numberParam("count", 2));
}

TEST(IbkrParserIgnoredTest, ThereIsNoOtherDiagnostic) {
    // The two file-level notices above, and nothing for the other cash rows and the other sections.
    EXPECT_EQ(ignored().mDiagnostics.size(), 2U);
}

// rejected_rows.xml

TEST(IbkrParserRejectedRowsTest, OnlyTheValidBuyBecomesAnEvent8001) {
    EXPECT_EQ(eventCount(rejectedRows()), 1U);

    const auto* trade = findTrade(rejectedRows(), "8001");

    ASSERT_NE(trade, nullptr);

    EXPECT_EQ(trade->mUnits, 500'000'000);       // 5 units
    EXPECT_EQ(trade->mUnitPrice, 2'000'000'000); // USD 20
    EXPECT_EQ(trade->mAmount, 1'000'000);        // USD 100.00
    EXPECT_EQ(trade->mFeePaid, 10'000);          // USD 1.00
    EXPECT_EQ(trade->mMetadata.mTaxDate, date(2024, 7, 1));
    EXPECT_EQ(trade->mMetadata.mSourceTimestamp, utc(2024, 7, 1, 18, 0, 0)); // 14:00 EDT
    EXPECT_TRUE(diagnosticsOf(rejectedRows(), "8001").empty());
}

TEST(IbkrParserRejectedRowsTest, EveryOtherRowHasExactlyOneError) {
    EXPECT_EQ(rejectedRows().mDiagnostics.size(), 10U);

    for (const auto id :
         {"8002", "8003", "8004", "8005", "8006", "8007", "8008", "8009", "8010", "8011"})
    {
        const auto diagnostics = diagnosticsOf(rejectedRows(), id);

        ASSERT_EQ(diagnostics.size(), 1U) << id;
        EXPECT_EQ(diagnostics.front()->mSeverity, DiagnosticSeverity::Error) << id;
        EXPECT_EQ(diagnostics.front()->mRowKind, ParsedRowKind::Trade) << id;
    }
}

TEST(IbkrParserRejectedRowsTest, CancelledTradeByBuySell8002) {
    // buySell "BUY (Ca.)", transactionType TradeCancel and notes Ca: the first check that fails is
    // the cancellation, so the wrong sign and the positive fee of this row are not reported.
    const auto diagnostics = diagnosticsOf(rejectedRows(), "8002");

    ASSERT_EQ(diagnostics.size(), 1U);

    EXPECT_EQ(diagnostics.front()->mCode, DiagnosticCode::UnsupportedRowType);
    EXPECT_EQ(diagnostics.front()->mMessageKey, MessageKey::UnsupportedRowTypeCancelledTrade);
    EXPECT_EQ(diagnostics.front()->mField, "buySell");
    EXPECT_EQ(diagnostics.front()->mRowIndex, 7U);
}

TEST(IbkrParserRejectedRowsTest, CorrectedTradeByTransactionType8003) {
    const auto diagnostics = diagnosticsOf(rejectedRows(), "8003");

    ASSERT_EQ(diagnostics.size(), 1U);

    EXPECT_EQ(diagnostics.front()->mCode, DiagnosticCode::UnsupportedRowType);
    EXPECT_EQ(diagnostics.front()->mMessageKey, MessageKey::UnsupportedRowTypeCancelledTrade);
    EXPECT_EQ(diagnostics.front()->mField, "transactionType");
}

TEST(IbkrParserRejectedRowsTest, CancelCodeAmongOtherNotes8004) {
    // notes "Ca;P": the code Ca is one of several joined by a semicolon.
    const auto diagnostics = diagnosticsOf(rejectedRows(), "8004");

    ASSERT_EQ(diagnostics.size(), 1U);

    EXPECT_EQ(diagnostics.front()->mCode, DiagnosticCode::UnsupportedRowType);
    EXPECT_EQ(diagnostics.front()->mMessageKey, MessageKey::UnsupportedRowTypeCancelledTrade);
    EXPECT_EQ(diagnostics.front()->mField, "notes");
}

TEST(IbkrParserRejectedRowsTest, BuyWithANegativeQuantity8005) {
    const auto diagnostics = diagnosticsOf(rejectedRows(), "8005");

    ASSERT_EQ(diagnostics.size(), 1U);

    // quantity -5 and proceeds 100 both disagree with BUY; only the first is reported.
    EXPECT_EQ(diagnostics.front()->mCode, DiagnosticCode::InconsistentValue);
    EXPECT_EQ(diagnostics.front()->mMessageKey, MessageKey::InconsistentValueTradeSide);
    EXPECT_EQ(diagnostics.front()->mField, "quantity");
}

TEST(IbkrParserRejectedRowsTest, StockRowWithoutIsinAttribute8006) {
    const auto diagnostics = diagnosticsOf(rejectedRows(), "8006");

    ASSERT_EQ(diagnostics.size(), 1U);

    EXPECT_EQ(diagnostics.front()->mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(diagnostics.front()->mMessageKey, MessageKey::InvalidHeaderMissingAttribute);
    EXPECT_EQ(diagnostics.front()->mField, "isin");
    ASSERT_EQ(diagnostics.front()->mMessageParams.size(), 2U);
    EXPECT_EQ(diagnostics.front()->mMessageParams[0], taxbroker::textParam("field", "isin"));
    EXPECT_EQ(diagnostics.front()->mMessageParams[1],
              taxbroker::termParam("rowKind", taxbroker::term::kRowKindTrade));
}

TEST(IbkrParserRejectedRowsTest, TradeDateWithDashes8007) {
    const auto diagnostics = diagnosticsOf(rejectedRows(), "8007");

    ASSERT_EQ(diagnostics.size(), 1U);

    EXPECT_EQ(diagnostics.front()->mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(diagnostics.front()->mMessageKey, MessageKey::InvalidValueField);
    EXPECT_EQ(diagnostics.front()->mField, "tradeDate");
}

TEST(IbkrParserRejectedRowsTest, DateTimeWithASpaceInsteadOfASemicolon8008) {
    const auto diagnostics = diagnosticsOf(rejectedRows(), "8008");

    ASSERT_EQ(diagnostics.size(), 1U);

    EXPECT_EQ(diagnostics.front()->mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(diagnostics.front()->mField, "dateTime");
}

TEST(IbkrParserRejectedRowsTest, UnknownCurrencyCode8009) {
    // currency and ibCommissionCurrency are both XXX; the first one is reported.
    const auto diagnostics = diagnosticsOf(rejectedRows(), "8009");

    ASSERT_EQ(diagnostics.size(), 1U);

    EXPECT_EQ(diagnostics.front()->mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(diagnostics.front()->mField, "currency");
}

TEST(IbkrParserRejectedRowsTest, QuantityWithADecimalComma8010) {
    const auto diagnostics = diagnosticsOf(rejectedRows(), "8010");

    ASSERT_EQ(diagnostics.size(), 1U);

    EXPECT_EQ(diagnostics.front()->mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(diagnostics.front()->mField, "quantity");
}

TEST(IbkrParserRejectedRowsTest, EmptyProceeds8011) {
    const auto diagnostics = diagnosticsOf(rejectedRows(), "8011");

    ASSERT_EQ(diagnostics.size(), 1U);

    EXPECT_EQ(diagnostics.front()->mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(diagnostics.front()->mMessageKey, MessageKey::MissingFieldRequiredField);
    EXPECT_EQ(diagnostics.front()->mField, "proceeds");
}

// timestamps.xml

TEST(IbkrParserTimestampsTest, TheSixTradesHaveTheTimesOfTheContract) {
    const auto& result = timestamps();

    ASSERT_TRUE(result.mDiagnostics.empty());

    const auto* winter = findTrade(result, "9001");
    const auto* beforeSwitch = findTrade(result, "9002");
    const auto* summer = findTrade(result, "9003");
    const auto* overnight = findTrade(result, "9004");
    const auto* lastSummerDay = findTrade(result, "9005");
    const auto* firstWinterDay = findTrade(result, "9006");

    ASSERT_NE(winter, nullptr);
    ASSERT_NE(beforeSwitch, nullptr);
    ASSERT_NE(summer, nullptr);
    ASSERT_NE(overnight, nullptr);
    ASSERT_NE(lastSummerDay, nullptr);
    ASSERT_NE(firstWinterDay, nullptr);

    EXPECT_EQ(winter->mMetadata.mTaxDate, date(2024, 3, 5));
    EXPECT_EQ(winter->mMetadata.mSourceTimestamp, utc(2024, 3, 5, 14, 42, 10)); // EST
    EXPECT_EQ(beforeSwitch->mMetadata.mTaxDate, date(2024, 3, 8));
    EXPECT_EQ(beforeSwitch->mMetadata.mSourceTimestamp, utc(2024, 3, 8, 20, 59, 59)); // EST
    EXPECT_EQ(summer->mMetadata.mTaxDate, date(2024, 3, 11));
    EXPECT_EQ(summer->mMetadata.mSourceTimestamp, utc(2024, 3, 11, 13, 30, 0)); // EDT
    EXPECT_EQ(lastSummerDay->mMetadata.mTaxDate, date(2024, 11, 1));
    EXPECT_EQ(lastSummerDay->mMetadata.mSourceTimestamp, utc(2024, 11, 1, 20, 0, 0)); // EDT
    EXPECT_EQ(firstWinterDay->mMetadata.mTaxDate, date(2024, 11, 4));
    EXPECT_EQ(firstWinterDay->mMetadata.mSourceTimestamp, utc(2024, 11, 4, 14, 30, 0)); // EST
}

TEST(IbkrParserTimestampsTest, AnOvernightTradeKeepsTheTradeDateAsTaxDate9004) {
    const auto* trade = findTrade(timestamps(), "9004");

    ASSERT_NE(trade, nullptr);

    // tradeDate 20240306, dateTime 20240305;203000 (20:30 EST = 01:30 UTC on the 6th).
    EXPECT_EQ(trade->mMetadata.mTaxDate, date(2024, 3, 6));
    EXPECT_EQ(trade->mMetadata.mSourceTimestamp, utc(2024, 3, 6, 1, 30, 0));
    EXPECT_FALSE(trade->mMetadata.mOrderingTimestamp.has_value());
    EXPECT_TRUE(diagnosticsOf(timestamps(), "9004").empty()); // no inconsistent_value warning
}

// quantities.xml

TEST(IbkrParserQuantitiesTest, NineDecimalsAreRoundedAndTheOriginalTextIsKept9101) {
    ASSERT_TRUE(quantities().mDiagnostics.empty());

    const auto* trade = findTrade(quantities(), "9101");

    ASSERT_NE(trade, nullptr);

    EXPECT_EQ(trade->mUnits, 33'333'333); // 0.333333334 rounds to 0.33333333 at 8 decimals
    ASSERT_EQ(trade->mUnitEvidence.size(), 1U);
    EXPECT_EQ(trade->mUnitEvidence.front().mSourceText, "0.333333334");
    EXPECT_EQ(trade->mUnitEvidence.front().mCanonicalValue, "0.333333334");
    EXPECT_EQ(trade->mUnitEvidence.front().mDiscardedDigits, "4"); // the 9th decimal
    EXPECT_FALSE(trade->mUnitEvidence.front().mRoundedAwayFromZero);

    EXPECT_EQ(findTrade(quantities(), "9102")->mUnits, 33'333'333);
    EXPECT_EQ(findTrade(quantities(), "9103")->mUnits, 33'333'333);
}

TEST(IbkrParserQuantitiesTest, TheSaleOfOneWholeUnitHasNoDiscardedDigits9104) {
    const auto* trade = findTrade(quantities(), "9104");

    ASSERT_NE(trade, nullptr);

    EXPECT_EQ(trade->mTradeSide, TradeSide::Sell);
    EXPECT_EQ(trade->mUnits, 100'000'000); // 1 unit
    EXPECT_EQ(trade->mUnitEvidence.front().mSourceText, "-1");
    EXPECT_EQ(trade->mUnitEvidence.front().mCanonicalValue, "-1");
    EXPECT_EQ(trade->mUnitEvidence.front().mDiscardedDigits, "");
}

TEST(IbkrParserQuantitiesTest, ThirtyOneDecimalsKeepTheWholeTextAsEvidence9105) {
    const auto* trade = findTrade(quantities(), "9105");

    ASSERT_NE(trade, nullptr);

    // 0.12345678|90123... rounds up: the 9th decimal is 9.
    EXPECT_EQ(trade->mUnits, 12'345'679);

    const auto& evidence = trade->mUnitEvidence.front();

    EXPECT_EQ(evidence.mSourceText, "0.1234567890123456789012345678901");
    EXPECT_EQ(evidence.mCanonicalValue, "0.1234567890123456789012345678901");
    EXPECT_EQ(evidence.mDiscardedDigits, "90123456789012345678901"); // 23 digits after the 8th
    EXPECT_TRUE(evidence.mRoundedAwayFromZero);
}

// file_level/

TEST(IbkrParserFileLevelTest, FlexQueryAsCsvIsNotRead) {
    const auto result = parseFixture("file_level/flex_query.csv");

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics.front().mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(result.mDiagnostics.front().mMessageKey,
              MessageKey::InvalidHeaderFlexQueryXmlRequired);
    EXPECT_EQ(result.mDiagnostics.front().mSourceFile, "flex_query.csv");
    EXPECT_FALSE(result.mDiagnostics.front().mRowIndex.has_value());
    EXPECT_FALSE(result.mDiagnostics.front().mRowKind.has_value());
    EXPECT_EQ(eventCount(result), 0U);
}

TEST(IbkrParserFileLevelTest, ActivityStatementAsCsvIsNotRead) {
    const auto result = parseFixture("file_level/activity_statement.csv");

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(result.mDiagnostics.front().mMessageKey,
              MessageKey::InvalidHeaderFlexQueryXmlRequired);
    EXPECT_EQ(eventCount(result), 0U);
}

TEST(IbkrParserFileLevelTest, WebServiceReplyWithAnotherRootIsNotRead) {
    const auto result = parseFixture("file_level/wrong_root.xml");

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(result.mDiagnostics.front().mMessageKey,
              MessageKey::InvalidHeaderFlexQueryXmlRequired);
    EXPECT_EQ(eventCount(result), 0U);
}

TEST(IbkrParserFileLevelTest, TruncatedFileIsNotRead) {
    // The file is cut off inside the trades section: no row of it is used.
    const auto result = parseFixture("file_level/truncated.xml");

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(result.mDiagnostics.front().mMessageKey,
              MessageKey::InvalidHeaderFlexQueryXmlRequired);
    EXPECT_EQ(eventCount(result), 0U);
}

TEST(IbkrParserFileLevelTest, DocumentTypeWithAnExternalEntityIsRejectedUnread) {
    const auto result = parseFixture("file_level/doctype_entity.xml");

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics.front().mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(result.mDiagnostics.front().mMessageKey, MessageKey::InvalidHeaderDoctypeNotAllowed);
    EXPECT_EQ(eventCount(result), 0U);
}

TEST(IbkrParserFileLevelTest, FileWithoutStatementsGivesNothing) {
    const auto result = parseFixture("file_level/empty_statements.xml");

    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_EQ(eventCount(result), 0U);
}

TEST(IbkrParserFileLevelTest, ByteOrderMarkIsAccepted) {
    const auto result = parseFixture("file_level/bom.xml");

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(eventCount(result), 1U);

    const auto& trade = result.mStatement.mTradeInstruments.front().mTransactions.front();

    EXPECT_EQ(result.mStatement.mTradeInstruments.front().mIsin, "US0000000010");
    EXPECT_EQ(trade.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(trade.mUnits, 1'000'000'000);      // 10 units
    EXPECT_EQ(trade.mUnitPrice, 15'025'000'000); // USD 150.25
    EXPECT_EQ(trade.mFeePaid, 10'000);           // USD 1.00
    EXPECT_EQ(trade.mMetadata.mSources.front().mSourceRow, 6U);
}

TEST(IbkrParserFileLevelTest, MalformedDocumentTypeIsRejectedAsADocumentType) {
    const auto result = parseText("<!DOCTYPE FlexQueryResponse [ <!ENTITY a \"b\"> "
                                  "<FlexQueryResponse/>");

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics.front().mMessageKey, MessageKey::InvalidHeaderDoctypeNotAllowed);
}

TEST(IbkrParserFileLevelTest, EmptyTextIsNotAFlexQuery) {
    const auto result = parseText("");

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics.front().mMessageKey,
              MessageKey::InvalidHeaderFlexQueryXmlRequired);
}

TEST(IbkrParserFileLevelTest, FileWithoutAFileNameIsNotRead) {
    IbkrParser parser;
    const auto result = parser.parse("<FlexQueryResponse/>", "", 0);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::ParseError);
    EXPECT_EQ(result.mDiagnostics.front().mMessageKey, MessageKey::ParseErrorInvalidFilename);
}

TEST(IbkrParserFileLevelTest, FileOverTheSizeLimitIsNotRead) {
    IbkrParser parser{10}; // a limit of 10 bytes
    const auto result = parser.parse("<FlexQueryResponse/>", "big.xml", 0);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InputFileTooLarge);
    EXPECT_EQ(result.mDiagnostics.front().mSourceFile, "big.xml");
}

TEST(IbkrParserFileLevelTest, MissingFileGivesAnUnreadableError) {
    IbkrParser parser;
    const auto result = parser.parseFile(fixtureFolder() / "no_such_file.xml", 0);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::ParseError);
    EXPECT_EQ(result.mBroker, Broker::InteractiveBrokers);
}

TEST(IbkrParserFileLevelTest, SourceIndexIsKeptInTheInputSequence) {
    IbkrParser parser;
    const auto result = parser.parseFile(fixtureFolder() / "file_level/bom.xml", 3);
    const auto& trade = result.mStatement.mTradeInstruments.front().mTransactions.front();

    EXPECT_EQ(trade.mMetadata.mSources.front().mInputSequence.mSourceIndex, 3U);
}

// unknown_entity.xml, contract_example.xml

TEST(IbkrParserEntityTest, UnknownEntityIsKeptAsThePayerCode2301) {
    const auto result = parseFixture("unknown_entity.xml");

    // The parser does not know the payer list: the code is kept and a later stage warns.
    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(eventCount(result), 1U);

    const auto* interest = findInterest(result, "2301");

    ASSERT_NE(interest, nullptr);

    EXPECT_EQ(interest->mGrossAmount, 17'500); // EUR 1.75
    EXPECT_EQ(interest->mCashHolder, CashHolder::Broker);
    EXPECT_EQ(interest->mPayerCode, "IB-XX");
    EXPECT_EQ(interest->mMetadata.mTaxDate, date(2024, 5, 3));
}

TEST(IbkrParserEntityTest, EachStatementUsesItsOwnEntity) {
    const std::string xml =
        R"(<FlexQueryResponse queryName="synthetic" type="AF">
  <FlexStatements count="4">
    <FlexStatement accountId="U0000000">
      <AccountInformation accountId="U0000000" ibEntity="IB-IE"/>
      <CashTransactions>
        <CashTransaction transactionID="C1" levelOfDetail="DETAIL" actionID="" type="Broker Interest Received" assetCategory="CASH" description="EUR CREDIT INT" isin="" currency="EUR" dateTime="20240403" reportDate="20240403" amount="1"/>
      </CashTransactions>
    </FlexStatement>
    <FlexStatement accountId="U0000001">
      <AccountInformation accountId="U0000001" ibEntity="IB-CE"/>
      <CashTransactions>
        <CashTransaction transactionID="C2" levelOfDetail="DETAIL" actionID="" type="Broker Interest Received" assetCategory="CASH" description="EUR CREDIT INT" isin="" currency="EUR" dateTime="20240403" reportDate="20240403" amount="2"/>
      </CashTransactions>
    </FlexStatement>
    <FlexStatement accountId="U0000002">
      <CashTransactions>
        <CashTransaction transactionID="C3" levelOfDetail="DETAIL" actionID="" type="Broker Interest Received" assetCategory="CASH" description="EUR CREDIT INT" isin="" currency="EUR" dateTime="20240403" reportDate="20240403" amount="3"/>
      </CashTransactions>
    </FlexStatement>
    <FlexStatement accountId="U0000003">
      <AccountInformation accountId="U0000003" ibEntity=""/>
      <CashTransactions>
        <CashTransaction transactionID="C4" levelOfDetail="DETAIL" actionID="" type="Broker Interest Received" assetCategory="CASH" description="EUR CREDIT INT" isin="" currency="EUR" dateTime="20240403" reportDate="20240403" amount="4"/>
      </CashTransactions>
    </FlexStatement>
  </FlexStatements>
</FlexQueryResponse>
)";
    const auto result = parseText(xml);

    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_EQ(findInterest(result, "C1")->mPayerCode, "IB-IE");
    EXPECT_EQ(findInterest(result, "C2")->mPayerCode, "IB-CE");
    EXPECT_FALSE(findInterest(result, "C3")->mPayerCode.has_value()); // no AccountInformation
    EXPECT_FALSE(findInterest(result, "C4")->mPayerCode.has_value()); // an empty ibEntity
    EXPECT_EQ(result.mStatement.mInterestInstruments.size(), 1U);     // one cash interest list
}

TEST(IbkrParserContractExampleTest, EveryKindOfRowGivesItsEvent) {
    const auto result = parseFixture("contract_example.xml");
    const auto& statement = result.mStatement;
    std::size_t trades = 0;
    std::size_t actions = 0;

    for (const auto& instrument : statement.mTradeInstruments)
    {
        trades += instrument.mTransactions.size();
        actions += instrument.mCorporateActions.size();
    }

    EXPECT_EQ(trades, 7U);                            // 1001, 1002, 1003, 1008, 1009, 1010, 5001
    EXPECT_EQ(actions, 4U);                           // splits 3001, 5002, 3004 and the merger 3002
    EXPECT_EQ(statement.mBondRedemptions.size(), 1U); // 3003
    EXPECT_EQ(statement.mIncomePaymentRows.size(), 7U); // 2001-2006 and 2013
    EXPECT_EQ(statement.mLeftOutRows.size(), 2U); // the option 1004 and the margin interest 2008
    EXPECT_EQ(statement.mInterestInstruments.size(), 2U);

    const auto* merger = findAction(result, "3002");

    ASSERT_NE(merger, nullptr);

    EXPECT_EQ(merger->mType, CorporateActionType::Merger); // the type the processor blocks
}

TEST(IbkrParserContractExampleTest, TheDiagnosticsAreTheFourOfTheReadme) {
    const auto result = parseFixture("contract_example.xml");

    // Two cancelled or corrected trades (1007, 1011), the currency conversion (1006) and the one
    // notice for the closed lot.
    ASSERT_EQ(result.mDiagnostics.size(), 4U);

    EXPECT_EQ(diagnosticsOf(result, "1007").size(), 1U);
    EXPECT_EQ(diagnosticsOf(result, "1011").size(), 1U);
    EXPECT_EQ(noticesOf(result, MessageKey::UnsupportedRowTypeCurrencyConversion).size(), 1U);
    EXPECT_EQ(diagnosticsOf(result, "1007").front()->mMessageKey,
              MessageKey::UnsupportedRowTypeCancelledTrade);
    EXPECT_EQ(diagnosticsOf(result, "1011").front()->mMessageKey,
              MessageKey::UnsupportedRowTypeCancelledTrade);
}

TEST(IbkrParserContractExampleTest, WithholdingRefundAndCorrectionsAreStoredPositive) {
    const auto result = parseFixture("contract_example.xml");
    const auto* refund = findPayment(result, "2013");
    const auto* reversal = findPayment(result, "2004");

    ASSERT_NE(refund, nullptr);
    ASSERT_NE(reversal, nullptr);

    EXPECT_EQ(refund->mKind, IncomePaymentKind::TaxRefund); // a positive tax row
    EXPECT_EQ(refund->mAmount, 3'000);                      // USD 0.30
    EXPECT_EQ(refund->mBookedDate, date(2025, 2, 10));      // booked later than it was paid
    EXPECT_EQ(refund->mMetadata.mTaxDate, date(2024, 3, 15));

    EXPECT_EQ(reversal->mKind, IncomePaymentKind::DividendReversal);
    EXPECT_EQ(reversal->mAmount, 50'000); // USD 5.00
    EXPECT_EQ(reversal->mPaymentGroupId, "A9002");
}

// conflicts

TEST(IbkrParserConflictTest, SameTradeIdWithDifferentFactsInTwoFilesIsOneConflict) {
    IbkrParser parser;
    std::vector<taxbroker::StatementMergeInput> inputs;

    inputs.push_back({0, parser.parseFile(fixtureFolder() / "conflicts_2024.xml", 0)});
    inputs.push_back({1, parser.parseFile(fixtureFolder() / "conflicts_2025.xml", 1)});

    const auto merged = taxbroker::DeterministicStatementMerger{}.merge(std::move(inputs));
    std::size_t conflicts = 0;

    for (const auto& diagnostic : merged.mDiagnostics)
    {
        if (const auto* merge = std::get_if<taxbroker::MergeDiagnostic>(&diagnostic))
        {
            ++conflicts;

            EXPECT_EQ(merge->mCode, taxbroker::MergeDiagnosticCode::ConflictingDuplicate);
            EXPECT_EQ(merge->mSources.size(), 2U); // both versions of trade 6101
        }
    }

    EXPECT_EQ(conflicts, 1U);
    EXPECT_TRUE(merged.mStatement.mPresentation.mTradeInstruments.empty());
}

TEST(IbkrParserConflictTest, TheSameTradeWithTheSameFactsInTwoFilesIsOneTrade) {
    // The 2024 and 2025 correction files both hold the buy 6001 with the same facts.
    IbkrParser parser;
    auto first = parser.parseFile(fixtureFolder() / "corrections_2024.xml", 0);
    auto second = parser.parseFile(fixtureFolder() / "corrections_2025.xml", 1);

    // The payment rows are added up by a later step, so only the buy is merged here.
    first.mStatement.mIncomePaymentRows.clear();
    second.mStatement.mIncomePaymentRows.clear();

    std::vector<taxbroker::StatementMergeInput> inputs;

    inputs.push_back({0, std::move(first)});
    inputs.push_back({1, std::move(second)});

    const auto merged = taxbroker::DeterministicStatementMerger{}.merge(std::move(inputs));

    ASSERT_EQ(merged.mStatement.mPresentation.mTradeInstruments.size(), 1U);
    EXPECT_EQ(merged.mStatement.mPresentation.mTradeInstruments.front().mTransactions.size(), 1U);
    EXPECT_EQ(merged.mStatement.mPresentation.mTradeInstruments.front()
                  .mTransactions.front()
                  .mMetadata.mSources.size(),
              2U); // the two files
}

TEST(IbkrParserConflictTest, TheMergerRefusesRowsItCannotMergeYet) {
    IbkrParser parser;
    std::vector<taxbroker::StatementMergeInput> inputs;

    inputs.push_back({0, parser.parseFile(fixtureFolder() / "supported.xml", 0)});

    EXPECT_THROW((void)taxbroker::DeterministicStatementMerger{}.merge(std::move(inputs)),
                 std::logic_error);
}

TEST(IbkrParserConflictTest, TheMergerRefusesLeftOutRowsItCannotCountYet) {
    IbkrParser parser;
    std::vector<taxbroker::StatementMergeInput> inputs;

    inputs.push_back({0, parser.parseFile(fixtureFolder() / "ignored.xml", 0)});

    EXPECT_THROW((void)taxbroker::DeterministicStatementMerger{}.merge(std::move(inputs)),
                 std::logic_error);
}

// Rows written in the test: trades

namespace {

/// A valid buy of 5 units at USD 20 with a fee of USD 1.00; the tests below each break it.
constexpr std::string_view kGoodTradeRow =
    R"(<Trade transactionID="T1" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="5" tradePrice="20" proceeds="-100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>)";

} // namespace

TEST(IbkrParserTradeRowTest, AGoodRowGivesATradeAndNoDiagnostic) {
    const auto result = parseText(statementWithRows(kGoodTradeRow, "", ""));

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_NE(findTrade(result, "T1"), nullptr);
    EXPECT_EQ(findTrade(result, "T1")->mMetadata.mSources.front().mSourceRow, 6U);
}

TEST(IbkrParserTradeRowTest, SummaryRowIsNotAnExecution) {
    // levelOfDetail SUMMARY: counted in the one notice, never read.
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T2" levelOfDetail="SUMMARY" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="5" tradePrice="20" proceeds="-100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>)",
        "",
        ""));

    EXPECT_EQ(eventCount(result), 0U);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mSeverity, DiagnosticSeverity::Info);
    EXPECT_EQ(result.mDiagnostics.front().mMessageKey, MessageKey::UnsupportedRowTypeNotExecution);
    EXPECT_EQ(result.mDiagnostics.front().mMessageParams.front(),
              taxbroker::numberParam("count", 1));
}

TEST(IbkrParserTradeRowTest, TradeWithAnEmptyLevelOfDetailIsAMissingField) {
    // An empty value is not a summary row: the buy would be lost behind a notice.
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T2" levelOfDetail="" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="5" tradePrice="20" proceeds="-100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>)",
        "",
        ""));

    EXPECT_EQ(eventCount(result), 0U);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(result.mDiagnostics.front().mField, "levelOfDetail");
    EXPECT_EQ(result.mDiagnostics.front().mRowKind, ParsedRowKind::Trade);
    EXPECT_EQ(result.mDiagnostics.front().mTransactionId, "T2");
}

TEST(IbkrParserTradeRowTest, RowWithoutLevelOfDetailIsRejected) {
    // The attribute is not there at all (not just empty).
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T3" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="5" tradePrice="20" proceeds="-100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>)",
        "",
        ""));

    EXPECT_EQ(eventCount(result), 0U);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(result.mDiagnostics.front().mField, "levelOfDetail");
    EXPECT_EQ(result.mDiagnostics.front().mTransactionId, "T3");
}

TEST(IbkrParserTradeRowTest, RowWithoutAssetCategoryIsRejected) {
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T4" levelOfDetail="EXECUTION" transactionType="ExchTrade" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="5" tradePrice="20" proceeds="-100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>)",
        "",
        ""));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mField, "assetCategory");
    EXPECT_EQ(eventCount(result), 0U);
}

TEST(IbkrParserTradeRowTest, RowWithoutTheMultiplierAttributeIsRejected) {
    // A missing multiplier is not read as 1: the price of a bond would be 100 times too high.
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T5" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="BOND" subCategory="CORP" description="SYNBOND" isin="DE0000000041" currency="EUR" tradeDate="20240320" dateTime="20240320;050000" buySell="BUY" quantity="1000" tradePrice="99.5" proceeds="-995" ibCommission="-2" ibCommissionCurrency="EUR" taxes="0" notes=""/>)",
        "",
        ""));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(result.mDiagnostics.front().mField, "multiplier");
    EXPECT_EQ(eventCount(result), 0U);
}

TEST(IbkrParserTradeRowTest, OptionWithoutAnIsinIsLeftOutAndNeedsNoOtherAttribute) {
    // A derivative needs only its class, its ID and a date for the identity of the row.
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T6" levelOfDetail="EXECUTION" assetCategory="OPT" tradeDate="20240701"/>)",
        "",
        ""));

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mLeftOutRows.size(), 1U);
    EXPECT_EQ(result.mStatement.mLeftOutRows.front().mMetadata.mTaxDate, date(2024, 7, 1));
    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
}

TEST(IbkrParserTradeRowTest, OptionWithABadDateIsStillLeftOut) {
    // Nothing is read from a left-out row, so its date is not an error.
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T7" levelOfDetail="EXECUTION" assetCategory="FOP" tradeDate="soon"/>)",
        "",
        ""));

    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_EQ(result.mStatement.mLeftOutRows.size(), 1U);
}

TEST(IbkrParserTradeRowTest, CryptoTradeWithoutAnIsinIsOneNotice) {
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T8" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="CRYPTO" subCategory="" description="BITCOIN" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="0.1" tradePrice="60000" proceeds="-6000" ibCommission="-6" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>)",
        "",
        ""));

    EXPECT_EQ(eventCount(result), 0U);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mSeverity, DiagnosticSeverity::Info);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::UnsupportedRowType);
    EXPECT_EQ(result.mDiagnostics.front().mMessageKey,
              MessageKey::UnsupportedRowTypeCryptoWithoutIsin);
    EXPECT_FALSE(result.mDiagnostics.front().mRowKind.has_value());
}

TEST(IbkrParserTradeRowTest, CryptoTradeWithAnIsinIsATradeOfClassCrypto) {
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T9" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="CRYPTO" subCategory="" description="SYNTHETIC COIN" isin="XS0000000011" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="2" tradePrice="10" proceeds="-20" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>)",
        "",
        ""));

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_NE(findTrade(result, "T9"), nullptr);
    EXPECT_EQ(result.mStatement.mTradeInstruments.front().mAssetClass, AssetClass::Crypto);
}

TEST(IbkrParserTradeRowTest, CommodityWithoutAnIsinIsAMissingField) {
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T10" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="CMDTY" subCategory="" description="SYNTHETIC GOLD" isin="" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="1" tradePrice="2000" proceeds="-2000" ibCommission="-2" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>)",
        "",
        ""));

    EXPECT_EQ(eventCount(result), 0U);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(result.mDiagnostics.front().mField, "isin");
    EXPECT_EQ(result.mDiagnostics.front().mRowKind, ParsedRowKind::Trade);
}

TEST(IbkrParserTradeRowTest, CommodityWithAnIsinIsAnUnknownClassForTheClassificationRequest) {
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T11" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="CMDTY" subCategory="" description="SYNTHETIC ETC" isin="JE0000000015" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="1" tradePrice="2000" proceeds="-2000" ibCommission="-2" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>)",
        "",
        ""));

    // The trade is kept with an unknown class, and the error stays until the user classifies it.
    ASSERT_NE(findTrade(result, "T11"), nullptr);
    EXPECT_EQ(result.mStatement.mTradeInstruments.front().mAssetClass, AssetClass::Unknown);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::UnsupportedAssetClass);
    EXPECT_EQ(result.mDiagnostics.front().mMessageKey,
              MessageKey::UnsupportedAssetClassUnrecognized);
    EXPECT_EQ(findTrade(result, "T11")->mInstrumentEvidence.front().mAssetClassLabel, "CMDTY");
}

TEST(IbkrParserTradeRowTest, EmptyAssetCategoryIsAMissingAssetClass) {
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T12" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="" subCategory="" description="SYNTHETIC X" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="5" tradePrice="20" proceeds="-100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>)",
        "",
        ""));

    ASSERT_NE(findTrade(result, "T12"), nullptr);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(result.mDiagnostics.front().mMessageKey, MessageKey::MissingFieldAssetClass);
}

TEST(IbkrParserTradeRowTest, TheSameIsinAsShareAndFundIsAConflictingClass) {
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T13" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="5" tradePrice="20" proceeds="-100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>
        <Trade transactionID="T14" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="FUND" subCategory="" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240702" dateTime="20240702;140000" buySell="BUY" quantity="5" tradePrice="20" proceeds="-100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>)",
        "",
        ""));

    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);
    EXPECT_EQ(result.mStatement.mTradeInstruments.front().mAssetClass, AssetClass::Unknown);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InconsistentValue);
    EXPECT_EQ(result.mDiagnostics.front().mMessageKey, MessageKey::InconsistentValueAssetClass);
    EXPECT_EQ(result.mDiagnostics.front().mTransactionId, "T14");
}

TEST(IbkrParserTradeRowTest, BlankDescriptionIsAWarningAndTheTradeIsKept) {
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T15" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="5" tradePrice="20" proceeds="-100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>)",
        "",
        ""));

    ASSERT_NE(findTrade(result, "T15"), nullptr);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(result.mDiagnostics.front().mMessageKey, MessageKey::MissingFieldName);
    EXPECT_EQ(result.mDiagnostics.front().mField, "description");
}

TEST(IbkrParserTradeRowTest, BillIsABondLikeABond) {
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T16" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="BILL" subCategory="" description="SYNTHETIC T-BILL" isin="US0000000051" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="1000" tradePrice="98" proceeds="-980" ibCommission="0" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="0.01"/>)",
        "",
        ""));

    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_EQ(result.mStatement.mTradeInstruments.front().mAssetClass, AssetClass::Bond);
    EXPECT_EQ(findTrade(result, "T16")->mUnitPrice, 98'000'000); // 98 x 0.01 = 0.98
}

TEST(IbkrParserTradeRowTest, PriceTimesMultiplierIsRoundedOnceToEightDecimals) {
    // 12.345678495 x 0.01 = 0.12345678495, which is 0.12345678 at 8 decimals (the 9th decimal is
    // 4). Rounding the price to 8 decimals first (12.34567850) would give 0.1234567850 and then
    // 0.12345679, so this row shows that the product is rounded only once. Amount check:
    // 1000 x 0.12345678 = 123.45678, close to the proceeds 123.46.
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T17" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="BOND" subCategory="CORP" description="SYNBOND" isin="DE0000000041" currency="EUR" tradeDate="20240320" dateTime="20240320;050000" buySell="BUY" quantity="1000" tradePrice="12.345678495" proceeds="-123.46" ibCommission="-2" ibCommissionCurrency="EUR" taxes="0" notes="" multiplier="0.01"/>)",
        "",
        ""));

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_NE(findTrade(result, "T17"), nullptr);
    EXPECT_EQ(findTrade(result, "T17")->mUnitPrice, 12'345'678);
}

TEST(IbkrParserTradeRowTest, EmptyCommissionAndTaxesAreZero) {
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T18" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="5" tradePrice="20" proceeds="-100" ibCommission="" ibCommissionCurrency="USD" taxes="" notes="" multiplier="1"/>)",
        "",
        ""));

    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_EQ(findTrade(result, "T18")->mFeePaid, 0);
    EXPECT_EQ(findTrade(result, "T18")->mTransactionTaxPaid, 0);
}

TEST(IbkrParserTradeRowTest, OtherNotesCodesAreNotACancellation) {
    // IA, P and ST are normal codes; only Ca means cancelled.
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T19" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="5" tradePrice="20" proceeds="-100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="IA;P;ST" multiplier="1"/>)",
        "",
        ""));

    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_NE(findTrade(result, "T19"), nullptr);
}

TEST(IbkrParserTradeRowTest, TradeAmountThatDoesNotMatchPriceTimesUnitsIsAWarning) {
    // 5 x 20 = 100, but proceeds say 105: more than the EUR 0.01 rounding.
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T20" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="5" tradePrice="20" proceeds="-105" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>)",
        "",
        ""));

    ASSERT_NE(findTrade(result, "T20"), nullptr); // the row is kept
    EXPECT_EQ(findTrade(result, "T20")->mAmount, 1'050'000);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::TradeAmountMismatch);
    EXPECT_EQ(result.mDiagnostics.front().mField, "proceeds");
}

TEST(IbkrParserTradeRowTest, AFieldMissingAsAttributeIsReportedBeforeAFieldWithABadValue) {
    // `buySell` is not there at all, `currency` is bad: the missing attribute wins.
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T21" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="XXX" tradeDate="20240701" dateTime="20240701;140000" quantity="5" tradePrice="20" proceeds="-100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>)",
        "",
        ""));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(result.mDiagnostics.front().mField, "buySell");
}

TEST(IbkrParserTradeRowTest, BadValuesAreReportedOneFieldAtATimeInTheOrderOfTheChecks) {
    // The first of these rows has a bad dateTime and a bad quantity: only dateTime is reported.
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T22" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240310;023000" buySell="BUY" quantity="five" tradePrice="20" proceeds="-100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>)",
        "",
        ""));

    // 20240310;023000 is the hour that does not exist in US Eastern time.
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(result.mDiagnostics.front().mField, "dateTime");
}

TEST(IbkrParserTradeRowTest, DateTimeWithoutATimeIsRejectedOnATrade) {
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T23" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701" buySell="BUY" quantity="5" tradePrice="20" proceeds="-100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>)",
        "",
        ""));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mField, "dateTime");
    EXPECT_EQ(eventCount(result), 0U);
}

TEST(IbkrParserTradeRowTest, UnknownFeeCurrencyIsRejected) {
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T24" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="5" tradePrice="20" proceeds="-100" ibCommission="-1" ibCommissionCurrency="XXX" taxes="0" notes="" multiplier="1"/>)",
        "",
        ""));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mField, "ibCommissionCurrency");
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidValue);
}

TEST(IbkrParserTradeRowTest, BuySellOfAnotherWordIsRejected) {
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T25" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="HOLD" quantity="5" tradePrice="20" proceeds="-100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>)",
        "",
        ""));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mField, "buySell");
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidValue);
}

TEST(IbkrParserTradeRowTest, ZeroPriceIsRejected) {
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T26" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="5" tradePrice="0" proceeds="-100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>)",
        "",
        ""));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mField, "tradePrice");
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidValue);
}

TEST(IbkrParserTradeRowTest, ZeroMultiplierIsRejected) {
    // A bad multiplier is reported on the multiplier, not on the valid price of 20.
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T27" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="5" tradePrice="20" proceeds="-100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="0"/>)",
        R"()",
        R"()"));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(result.mDiagnostics.front().mField, "multiplier");
    EXPECT_EQ(eventCount(result), 0U);
}

TEST(IbkrParserTradeRowTest, NegativeMultiplierIsRejected) {
    // A bad multiplier is reported on the multiplier, not on the valid price of 20.
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T27" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="5" tradePrice="20" proceeds="-100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="-0.01"/>)",
        R"()",
        R"()"));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(result.mDiagnostics.front().mField, "multiplier");
    EXPECT_EQ(eventCount(result), 0U);
}

TEST(IbkrParserTradeRowTest, MultiplierThatIsNotANumberIsRejected) {
    // A bad multiplier is reported on the multiplier, not on the valid price of 20.
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T27" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="5" tradePrice="20" proceeds="-100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="abc"/>)",
        R"()",
        R"()"));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(result.mDiagnostics.front().mField, "multiplier");
    EXPECT_EQ(eventCount(result), 0U);
}

TEST(IbkrParserTradeRowTest, EmptyMultiplierIsAMissingField) {
    // A bad multiplier is reported on the multiplier, not on the valid price of 20.
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T27" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="5" tradePrice="20" proceeds="-100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier=""/>)",
        R"()",
        R"()"));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(result.mDiagnostics.front().mField, "multiplier");
    EXPECT_EQ(eventCount(result), 0U);
}

TEST(IbkrParserTradeRowTest, TradeWithAnEmptyTransactionIdIsRejected) {
    // Without an ID the merger could not drop the repeat of this row in an overlapping file.
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="5" tradePrice="20" proceeds="-100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>)",
        R"()",
        R"()"));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(result.mDiagnostics.front().mField, "transactionID");
    EXPECT_FALSE(result.mDiagnostics.front().mTransactionId.has_value());
    EXPECT_EQ(eventCount(result), 0U);
}

TEST(IbkrParserTradeRowTest, OptionWithAnEmptyTransactionIdIsStillLeftOut) {
    // A left-out row belongs to no form, so a missing ID blocks nothing.
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="" levelOfDetail="EXECUTION" assetCategory="OPT" tradeDate="20240701"/>)",
        R"()",
        R"()"));

    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_EQ(result.mStatement.mLeftOutRows.size(), 1U);
}

TEST(IbkrParserTradeRowTest, PriceWithGroupingIsRejected) {
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T28" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="5" tradePrice="1,020.5" proceeds="-100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>)",
        "",
        ""));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mField, "tradePrice");
}

TEST(IbkrParserTradeRowTest, NegativePriceIsRejected) {
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T50" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="5" tradePrice="-20" proceeds="-100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>)",
        "",
        ""));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(result.mDiagnostics.front().mField, "tradePrice");
}

TEST(IbkrParserTradeRowTest, PriceWithMoreThanSixtyCharactersIsRejected) {
    // 64 decimals: far more than any price has, and more than the exact reader takes.
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T51" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="5" tradePrice="0.1234567890123456789012345678901234567890123456789012345678901234" proceeds="-100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>)",
        "",
        ""));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(result.mDiagnostics.front().mField, "tradePrice");
}

TEST(IbkrParserTradeRowTest, PriceThatDoesNotFitAfterTheMultiplierIsRejected) {
    // 90,000,000,000 x 90,000,000,000 = 8.1e21, far above what 8 decimals in 64 bits can hold.
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T52" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="5" tradePrice="90000000000" proceeds="-100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="90000000000"/>)",
        "",
        ""));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(result.mDiagnostics.front().mField, "tradePrice");
}

TEST(IbkrParserTradeRowTest, ProceedsThatIsNotANumberIsRejected) {
    const auto result = parseText(statementWithRows(
        R"xml(<Trade transactionID="T29" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="5" tradePrice="20" proceeds="(100)" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>)xml",
        "",
        ""));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mField, "proceeds");
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidValue);
}

TEST(IbkrParserTradeRowTest, CommissionThatIsNotANumberIsRejected) {
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T30" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="5" tradePrice="20" proceeds="-100" ibCommission="one" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>)",
        "",
        ""));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mField, "ibCommission");
}

TEST(IbkrParserTradeRowTest, TaxesThatIsNotANumberIsRejected) {
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T31" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="5" tradePrice="20" proceeds="-100" ibCommission="-1" ibCommissionCurrency="USD" taxes="tax" notes="" multiplier="1"/>)",
        "",
        ""));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mField, "taxes");
}

TEST(IbkrParserTradeRowTest, SellWithAPositiveQuantityIsASignError) {
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T32" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="SELL" quantity="5" tradePrice="20" proceeds="100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>)",
        "",
        ""));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InconsistentValue);
    EXPECT_EQ(result.mDiagnostics.front().mField, "quantity");
    EXPECT_EQ(eventCount(result), 0U);
}

TEST(IbkrParserTradeRowTest, BuyWithPositiveProceedsIsASignError) {
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T33" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="5" tradePrice="20" proceeds="100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>)",
        "",
        ""));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InconsistentValue);
    EXPECT_EQ(result.mDiagnostics.front().mField, "proceeds");
}

TEST(IbkrParserTradeRowTest, ZeroQuantityIsASignError) {
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T34" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="0" tradePrice="20" proceeds="-100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>)",
        "",
        ""));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mField, "quantity");
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InconsistentValue);
}

TEST(IbkrParserTradeRowTest, PositiveCommissionIsASignError) {
    // A fee paid is negative; a positive one would be a refund, which the app does not read.
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T35" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="5" tradePrice="20" proceeds="-100" ibCommission="1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>)",
        "",
        ""));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InconsistentValue);
    EXPECT_EQ(result.mDiagnostics.front().mField, "ibCommission");
}

TEST(IbkrParserTradeRowTest, PositiveTaxesIsASignError) {
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T36" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="5" tradePrice="20" proceeds="-100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0.5" notes="" multiplier="1"/>)",
        "",
        ""));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InconsistentValue);
    EXPECT_EQ(result.mDiagnostics.front().mField, "taxes");
}

TEST(IbkrParserTradeRowTest, EventIndexCountsEveryRowElementInDocumentOrder) {
    const auto result = parseText(statementWithRows(
        R"(<Trade transactionID="T40" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="5" tradePrice="20" proceeds="-100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>
        <Trade transactionID="T41" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" tradeDate="20240702" dateTime="20240702;140000" buySell="BUY" quantity="5" tradePrice="20" proceeds="-100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>)",
        R"(<CorporateAction transactionID="S40" actionID="X40" type="FS" assetCategory="STK" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" dateTime="20240801;202500" quantity="10" proceeds="0"/>)",
        ""));

    ASSERT_TRUE(result.mDiagnostics.empty());

    EXPECT_EQ(findTrade(result, "T40")->mMetadata.mSources.front().mInputSequence.mEventIndex, 0U);
    EXPECT_EQ(findTrade(result, "T41")->mMetadata.mSources.front().mInputSequence.mEventIndex, 1U);
    EXPECT_EQ(findAction(result, "S40")->mMetadata.mSources.front().mInputSequence.mEventIndex, 2U);
    EXPECT_EQ(findTrade(result, "T40")->mMetadata.mSources.front().mSourceRow, 6U);
    EXPECT_EQ(findTrade(result, "T41")->mMetadata.mSources.front().mSourceRow, 7U);
    EXPECT_EQ(findAction(result, "S40")->mMetadata.mSources.front().mSourceRow, 10U);
}

// Rows written in the test: corporate actions

TEST(IbkrParserActionRowTest, MergerIsTheUnsupportedType) {
    const auto result = parseText(statementWithRows(
        "",
        R"(<CorporateAction transactionID="S1" actionID="X1" type="TC" assetCategory="STK" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" dateTime="20240801;202500" quantity="-10" proceeds="0"/>)",
        ""));

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_NE(findAction(result, "S1"), nullptr);
    EXPECT_EQ(findAction(result, "S1")->mType, CorporateActionType::Merger);
    EXPECT_EQ(findAction(result, "S1")->mUnitsDelta, -1'000'000'000);
}

TEST(IbkrParserActionRowTest, UnknownCodeIsTheUnsupportedTypeToo) {
    const auto result = parseText(statementWithRows(
        "",
        R"(<CorporateAction transactionID="S2" actionID="X2" type="SO" assetCategory="STK" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" dateTime="20240801;202500" quantity="0" proceeds="0"/>)",
        ""));

    // A spin-off with quantity 0 is still kept, so the corporate-action stage can block.
    ASSERT_NE(findAction(result, "S2"), nullptr);
    EXPECT_EQ(findAction(result, "S2")->mType, CorporateActionType::Merger);
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(IbkrParserActionRowTest, SplitThatChangesTheIsinIsUnsupported) {
    // Two rows with one action ID and two ISINs: the old ISIN leaves and the new one arrives.
    const auto result = parseText(statementWithRows(
        "",
        R"(<CorporateAction transactionID="S3" actionID="X3" type="FS" assetCategory="STK" description="OLD SYNTHETIC INC" isin="US0000000036" currency="USD" dateTime="20240801;202500" quantity="-10" proceeds="0"/>
        <CorporateAction transactionID="S4" actionID="X3" type="FS" assetCategory="STK" description="NEW SYNTHETIC INC" isin="US0000000044" currency="USD" dateTime="20240801;202500" quantity="40" proceeds="0"/>)",
        ""));

    ASSERT_NE(findAction(result, "S3"), nullptr);
    ASSERT_NE(findAction(result, "S4"), nullptr);
    EXPECT_EQ(findAction(result, "S3")->mType, CorporateActionType::Merger);
    EXPECT_EQ(findAction(result, "S4")->mType, CorporateActionType::Merger);
}

TEST(IbkrParserActionRowTest, SplitsOfTwoIsinsWithoutAnActionIdAreNotGrouped) {
    const auto result = parseText(statementWithRows(
        "",
        R"(<CorporateAction transactionID="S5" actionID="" type="FS" assetCategory="STK" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" dateTime="20240801;202500" quantity="10" proceeds="0"/>
        <CorporateAction transactionID="S6" actionID="" type="RS" assetCategory="STK" description="SYNTHETIC EXAMPLE D INC" isin="US0000000044" currency="USD" dateTime="20240802;202500" quantity="-8" proceeds="0"/>)",
        ""));

    EXPECT_EQ(findAction(result, "S5")->mType, CorporateActionType::UnresolvedSplit);
    EXPECT_EQ(findAction(result, "S6")->mType, CorporateActionType::UnresolvedSplit);
}

TEST(IbkrParserActionRowTest, CashDividendAndCouponRowsAreIgnored) {
    const auto result = parseText(statementWithRows(
        "",
        R"(<CorporateAction transactionID="S7" actionID="X7" type="CD" assetCategory="STK" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" dateTime="20240801;202500" quantity="0" proceeds="12"/>
        <CorporateAction transactionID="S8" actionID="X8" type="CP" assetCategory="BOND" description="SYNBOND" isin="DE0000000041" currency="EUR" dateTime="20240801;202500" quantity="0" proceeds="45"/>)",
        ""));

    EXPECT_EQ(eventCount(result), 0U);
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(IbkrParserActionRowTest, DerivativeActionIsLeftOut) {
    const auto result = parseText(statementWithRows(
        "",
        R"(<CorporateAction transactionID="S9" actionID="X9" type="EX" assetCategory="OPT" dateTime="20240801;202500"/>)",
        ""));

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mLeftOutRows.size(), 1U);
    EXPECT_EQ(result.mStatement.mLeftOutRows.front().mMetadata.mTaxDate, date(2024, 8, 1));
}

TEST(IbkrParserActionRowTest, BondMaturityWithPositiveQuantityIsACancelledMaturity) {
    const auto result = parseText(statementWithRows(
        "",
        R"(<CorporateAction transactionID="S10" actionID="X10" type="BM" assetCategory="BOND" description="SYNBOND" isin="DE0000000041" currency="EUR" dateTime="20241216;202500" quantity="1000" proceeds="-1000"/>)",
        ""));

    ASSERT_EQ(result.mStatement.mBondRedemptions.size(), 1U);
    EXPECT_EQ(result.mStatement.mBondRedemptions.front().mKind,
              BondRedemptionKind::MaturityCancelled);
    EXPECT_EQ(result.mStatement.mBondRedemptions.front().mUnits, 100'000'000'000);
    EXPECT_EQ(result.mStatement.mBondRedemptions.front().mAmount, 10'000'000); // |proceeds|
}

TEST(IbkrParserActionRowTest, TreasuryBillMaturityIsARedemption) {
    const auto result = parseText(statementWithRows(
        "",
        R"(<CorporateAction transactionID="S11" actionID="X11" type="TM" assetCategory="BILL" description="SYNTHETIC T-BILL" isin="US0000000051" currency="USD" dateTime="20241216;202500" quantity="-1000" proceeds="1000"/>)",
        ""));

    ASSERT_EQ(result.mStatement.mBondRedemptions.size(), 1U);
    EXPECT_EQ(result.mStatement.mBondRedemptions.front().mKind, BondRedemptionKind::Maturity);
    EXPECT_EQ(result.mStatement.mBondRedemptions.front().mCurrency, Currency::USD);
    EXPECT_EQ(result.mStatement.mBondRedemptions.front().mFeeCurrency, Currency::USD);
}

TEST(IbkrParserActionRowTest, MaturityOfAShareIsUnsupported) {
    // Only a bond or a bill matures; the same code on another class must not hide a sale.
    const auto result = parseText(statementWithRows(
        "",
        R"(<CorporateAction transactionID="S12" actionID="X12" type="BM" assetCategory="STK" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" dateTime="20241216;202500" quantity="-10" proceeds="100"/>)",
        ""));

    EXPECT_TRUE(result.mStatement.mBondRedemptions.empty());
    ASSERT_NE(findAction(result, "S12"), nullptr);
    EXPECT_EQ(findAction(result, "S12")->mType, CorporateActionType::Merger);
}

TEST(IbkrParserActionRowTest, MaturityWithoutProceedsAttributeIsRejected) {
    const auto result = parseText(statementWithRows(
        "",
        R"(<CorporateAction transactionID="S13" actionID="X13" type="BM" assetCategory="BOND" description="SYNBOND" isin="DE0000000041" currency="EUR" dateTime="20241216;202500" quantity="-1000"/>)",
        ""));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(result.mDiagnostics.front().mField, "proceeds");
    EXPECT_EQ(result.mDiagnostics.front().mRowKind, ParsedRowKind::BondRedemption);
    EXPECT_TRUE(result.mStatement.mBondRedemptions.empty());
}

TEST(IbkrParserActionRowTest, MaturityWithBadProceedsIsRejected) {
    const auto result = parseText(statementWithRows(
        "",
        R"(<CorporateAction transactionID="S14" actionID="X14" type="BM" assetCategory="BOND" description="SYNBOND" isin="DE0000000041" currency="EUR" dateTime="20241216;202500" quantity="-1000" proceeds="1.000,00"/>)",
        ""));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(result.mDiagnostics.front().mField, "proceeds");
    EXPECT_EQ(result.mDiagnostics.front().mRowKind, ParsedRowKind::BondRedemption);
}

TEST(IbkrParserActionRowTest, SplitWithZeroQuantityIsRejected) {
    const auto result = parseText(statementWithRows(
        "",
        R"(<CorporateAction transactionID="S15" actionID="X15" type="FS" assetCategory="STK" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" dateTime="20240801;202500" quantity="0" proceeds="0"/>)",
        ""));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(result.mDiagnostics.front().mField, "quantity");
    EXPECT_EQ(result.mDiagnostics.front().mRowKind, ParsedRowKind::CorporateAction);
    EXPECT_EQ(eventCount(result), 0U);
}

TEST(IbkrParserActionRowTest, ActionWithADateButNoTimeIsRejected) {
    const auto result = parseText(statementWithRows(
        "",
        R"(<CorporateAction transactionID="S16" actionID="X16" type="FS" assetCategory="STK" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" dateTime="20240801" quantity="10" proceeds="0"/>)",
        ""));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mField, "dateTime");
    EXPECT_EQ(eventCount(result), 0U);
}

TEST(IbkrParserActionRowTest, ActionWithAnEmptyTransactionIdIsRejected) {
    const auto result = parseText(statementWithRows(
        R"()",
        R"(<CorporateAction transactionID="" actionID="X" type="FS" assetCategory="STK" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" dateTime="20240801;202500" quantity="10" proceeds="0"/>)",
        R"()"));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(result.mDiagnostics.front().mField, "transactionID");
    EXPECT_EQ(result.mDiagnostics.front().mRowKind, ParsedRowKind::CorporateAction);
    EXPECT_EQ(eventCount(result), 0U);
}

TEST(IbkrParserActionRowTest, CashPaymentActionNeedsNoTransactionId) {
    // The cash dividend action is not read; its cash row is the payment.
    const auto result = parseText(statementWithRows(
        R"()",
        R"(<CorporateAction transactionID="" actionID="X" type="CD" assetCategory="STK" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" dateTime="20240801;202500" quantity="10" proceeds="0"/>)",
        R"()"));

    EXPECT_EQ(eventCount(result), 0U);
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(IbkrParserActionRowTest, ActionWithAnEmptyIsinIsAMissingField) {
    const auto result = parseText(statementWithRows(
        "",
        R"(<CorporateAction transactionID="S17" actionID="X17" type="FS" assetCategory="STK" description="SYNTHETIC EXAMPLE C INC" isin="" currency="USD" dateTime="20240801;202500" quantity="10" proceeds="0"/>)",
        ""));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(result.mDiagnostics.front().mField, "isin");
}

TEST(IbkrParserActionRowTest, ActionWithAnUnknownCurrencyIsRejected) {
    const auto result = parseText(statementWithRows(
        "",
        R"(<CorporateAction transactionID="S18" actionID="X18" type="FS" assetCategory="STK" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="XXX" dateTime="20240801;202500" quantity="10" proceeds="0"/>)",
        ""));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mField, "currency");
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidValue);
}

TEST(IbkrParserActionRowTest, ActionWithQuantityThatIsNotANumberIsRejected) {
    const auto result = parseText(statementWithRows(
        "",
        R"(<CorporateAction transactionID="S19" actionID="X19" type="TC" assetCategory="STK" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" dateTime="20240801;202500" quantity="ten" proceeds="0"/>)",
        ""));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mField, "quantity");
}

TEST(IbkrParserActionRowTest, ActionWithoutTheQuantityAttributeIsRejected) {
    const auto result = parseText(statementWithRows(
        "",
        R"(<CorporateAction transactionID="S20" actionID="X20" type="FS" assetCategory="STK" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" dateTime="20240801;202500" proceeds="0"/>)",
        ""));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(result.mDiagnostics.front().mField, "quantity");
}

TEST(IbkrParserActionRowTest, ActionWithoutTheTypeAttributeIsRejected) {
    const auto result = parseText(statementWithRows(
        "",
        R"(<CorporateAction transactionID="S21" actionID="X21" assetCategory="STK" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" dateTime="20240801;202500" quantity="10" proceeds="0"/>)",
        ""));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(result.mDiagnostics.front().mField, "type");
    EXPECT_EQ(result.mDiagnostics.front().mRowKind, ParsedRowKind::CorporateAction);
}

TEST(IbkrParserActionRowTest, ASplitCreatesTheInstrumentWithAnUnknownClassUntilATradeNamesIt) {
    // The split row comes first: it must not fix the class of the instrument.
    const auto result = parseText(statementWithRows(
        "",
        R"(<CorporateAction transactionID="S22" actionID="X22" type="FS" assetCategory="STK" description="SYNTHETIC EXAMPLE C INC" isin="US0000000036" currency="USD" dateTime="20240801;202500" quantity="10" proceeds="0"/>)",
        ""));

    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);
    EXPECT_EQ(result.mStatement.mTradeInstruments.front().mAssetClass, AssetClass::Unknown);
    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_EQ(result.mStatement.mTradeInstruments.front().mName, "SYNTHETIC EXAMPLE C INC");
}

// Rows written in the test: cash rows

TEST(IbkrParserCashRowTest, DividendWithTimeOfDayHasATimestamp) {
    const auto result = parseText(statementWithRows(
        "",
        "",
        R"(<CashTransaction transactionID="C1" levelOfDetail="DETAIL" actionID="A1" type="Dividends" assetCategory="STK" description="SYNC CASH DIVIDEND" isin="US0000000036" currency="USD" dateTime="20240315;093000" reportDate="20240318" amount="12"/>)"));

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_NE(findPayment(result, "C1"), nullptr);
    EXPECT_EQ(findPayment(result, "C1")->mMetadata.mTaxDate, date(2024, 3, 15));
    EXPECT_EQ(findPayment(result, "C1")->mMetadata.mSourceTimestamp,
              utc(2024, 3, 15, 13, 30, 0)); // EDT
    EXPECT_EQ(findPayment(result, "C1")->mName, "");
    EXPECT_EQ(findPayment(result, "C1")->mInstrumentEvidence.front().mAssetClass,
              AssetClass::Stock);
    EXPECT_EQ(findPayment(result, "C1")->mInstrumentEvidence.front().mAssetClassLabel, "STK");
}

TEST(IbkrParserCashRowTest, NegativeDividendIsAReversalStoredPositive) {
    const auto result = parseText(statementWithRows(
        "",
        "",
        R"(<CashTransaction transactionID="C2" levelOfDetail="DETAIL" actionID="A2" type="Dividends" assetCategory="STK" description="SYNC CASH DIVIDEND" isin="US0000000036" currency="USD" dateTime="20240315" reportDate="20250115" amount="-12"/>)"));

    ASSERT_NE(findPayment(result, "C2"), nullptr);
    EXPECT_EQ(findPayment(result, "C2")->mKind, IncomePaymentKind::DividendReversal);
    EXPECT_EQ(findPayment(result, "C2")->mAmount, 120'000);
    EXPECT_EQ(findPayment(result, "C2")->mBookedDate, date(2025, 1, 15));
}

TEST(IbkrParserCashRowTest, PositiveWithholdingIsARefund) {
    const auto result = parseText(statementWithRows(
        "",
        "",
        R"(<CashTransaction transactionID="C3" levelOfDetail="DETAIL" actionID="A3" type="Withholding Tax" assetCategory="STK" description="SYNC TAX" isin="US0000000036" currency="USD" dateTime="20240315" reportDate="20250115" amount="0.3"/>)"));

    ASSERT_NE(findPayment(result, "C3"), nullptr);
    EXPECT_EQ(findPayment(result, "C3")->mKind, IncomePaymentKind::TaxRefund);
    EXPECT_EQ(findPayment(result, "C3")->mAmount, 3'000);
}

TEST(IbkrParserCashRowTest, WithholdingOfZeroIsATaxRowOfZero) {
    const auto result = parseText(statementWithRows(
        "",
        "",
        R"(<CashTransaction transactionID="C4" levelOfDetail="DETAIL" actionID="A4" type="Withholding Tax" assetCategory="STK" description="SYNC TAX" isin="US0000000036" currency="USD" dateTime="20240315" reportDate="20240318" amount="0"/>)"));

    ASSERT_NE(findPayment(result, "C4"), nullptr);
    EXPECT_EQ(findPayment(result, "C4")->mKind, IncomePaymentKind::Tax);
    EXPECT_EQ(findPayment(result, "C4")->mAmount, 0);
}

TEST(IbkrParserCashRowTest, ThePaymentInLieuIsASubstituteAndCanBeReversed) {
    const auto result = parseText(statementWithRows(
        "",
        "",
        R"(<CashTransaction transactionID="C5" levelOfDetail="DETAIL" actionID="A5" type="Payment In Lieu Of Dividends" assetCategory="STK" description="SYNC IN LIEU" isin="US0000000036" currency="USD" dateTime="20240315" reportDate="20240318" amount="-3"/>)"));

    ASSERT_NE(findPayment(result, "C5"), nullptr);
    EXPECT_EQ(findPayment(result, "C5")->mKind, IncomePaymentKind::DividendReversal);
    EXPECT_TRUE(findPayment(result, "C5")->mSubstitutePayment);
}

TEST(IbkrParserCashRowTest, SummaryCashRowIsNotReadAndIsCounted) {
    // The detail row is the booking of EUR 3.21. The summary row repeats it as a total; it has no
    // ID, and reading it would count the interest twice (EUR 6.42).
    const auto result = parseText(statementWithRows(
        R"()",
        R"()",
        R"(<CashTransaction transactionID="C20" levelOfDetail="DETAIL" actionID="" type="Broker Interest Received" assetCategory="CASH" description="EUR CREDIT INT" isin="" currency="EUR" dateTime="20240403" reportDate="20240403" amount="3.21"/>
        <CashTransaction transactionID="" levelOfDetail="SUMMARY" actionID="" type="Broker Interest Received" assetCategory="CASH" description="EUR CREDIT INT" isin="" currency="EUR" dateTime="20240403" reportDate="20240403" amount="3.21"/>)"));

    ASSERT_NE(findInterest(result, "C20"), nullptr);
    EXPECT_EQ(findInterest(result, "C20")->mGrossAmount, 32'100); // EUR 3.21 at 4 decimals
    EXPECT_EQ(eventCount(result), 1U);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mSeverity, DiagnosticSeverity::Info);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::UnsupportedRowType);
    EXPECT_EQ(result.mDiagnostics.front().mMessageKey, MessageKey::UnsupportedRowTypeNotDetail);
    ASSERT_EQ(result.mDiagnostics.front().mMessageParams.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mMessageParams.front(),
              taxbroker::numberParam("count", 1));
}

TEST(IbkrParserCashRowTest, SummaryCashRowWithoutAnIdOrAnIsinIsNotAnError) {
    // A total row of dividends has no ISIN and no ID: it is counted, not rejected.
    const auto result = parseText(statementWithRows(
        R"()",
        R"()",
        R"(<CashTransaction transactionID="" levelOfDetail="SUMMARY" type="Dividends" assetCategory="STK" description="TOTAL" currency="USD" dateTime="20240403" reportDate="20240403" amount="12"/>)"));

    EXPECT_EQ(eventCount(result), 0U);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mMessageKey, MessageKey::UnsupportedRowTypeNotDetail);
}

TEST(IbkrParserCashRowTest, CashRowWithAnEmptyLevelOfDetailIsAMissingField) {
    const auto result = parseText(statementWithRows(
        R"()",
        R"()",
        R"(<CashTransaction transactionID="C21" levelOfDetail="" actionID="" type="Broker Interest Received" assetCategory="CASH" description="EUR CREDIT INT" isin="" currency="EUR" dateTime="20240403" reportDate="20240403" amount="3.21"/>)"));

    // An empty value must not hide a lost booking behind a notice.
    EXPECT_EQ(eventCount(result), 0U);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(result.mDiagnostics.front().mField, "levelOfDetail");
    EXPECT_FALSE(result.mDiagnostics.front().mRowKind.has_value()); // every form
}

TEST(IbkrParserCashRowTest, CashRowWithoutTheLevelOfDetailAttributeIsRejected) {
    const auto result = parseText(statementWithRows(
        R"()",
        R"()",
        R"(<CashTransaction transactionID="C22" actionID="" type="Broker Interest Received" assetCategory="CASH" description="EUR CREDIT INT" isin="" currency="EUR" dateTime="20240403" reportDate="20240403" amount="3.21"/>)"));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(result.mDiagnostics.front().mField, "levelOfDetail");
    EXPECT_EQ(eventCount(result), 0U);
}

TEST(IbkrParserCashRowTest, UnexpectedChildrenOfTheCashAndActionSectionsAreCounted) {
    // One unknown element in each section: two rows, one notice with the count 2.
    const auto result = parseText(statementWithRows("",
                                                    R"(<CorporateActionTotal total="1"/>)",
                                                    R"(<CashTransactionTotal total="1"/>)"));

    EXPECT_EQ(eventCount(result), 0U);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mSeverity, DiagnosticSeverity::Info);
    EXPECT_EQ(result.mDiagnostics.front().mMessageKey, MessageKey::UnsupportedRowTypeNotDetail);
    ASSERT_EQ(result.mDiagnostics.front().mMessageParams.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mMessageParams.front(),
              taxbroker::numberParam("count", 2));
}

TEST(IbkrParserCashRowTest, PaymentRowWithAnEmptyTransactionIdIsRejected) {
    const auto result = parseText(statementWithRows(
        R"()",
        R"()",
        R"(<CashTransaction transactionID="" levelOfDetail="DETAIL" actionID="A23" type="Dividends" assetCategory="STK" description="SYNC" isin="US0000000036" currency="USD" dateTime="20240315" reportDate="20240318" amount="12"/>)"));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(result.mDiagnostics.front().mField, "transactionID");
    EXPECT_EQ(result.mDiagnostics.front().mRowKind, ParsedRowKind::Dividend);
    EXPECT_EQ(eventCount(result), 0U);
}

TEST(IbkrParserCashRowTest, InterestRowWithAnEmptyTransactionIdIsRejected) {
    const auto result = parseText(statementWithRows(
        R"()",
        R"()",
        R"(<CashTransaction transactionID="" levelOfDetail="DETAIL" actionID="" type="Broker Interest Received" assetCategory="CASH" description="EUR CREDIT INT" isin="" currency="EUR" dateTime="20240403" reportDate="20240403" amount="3.21"/>)"));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(result.mDiagnostics.front().mField, "transactionID");
    EXPECT_EQ(result.mDiagnostics.front().mRowKind, ParsedRowKind::BrokerInterest);
    EXPECT_EQ(eventCount(result), 0U);
}

TEST(IbkrParserCashRowTest, MarginInterestWithAnEmptyTransactionIdIsStillLeftOut) {
    const auto result = parseText(statementWithRows(
        R"()",
        R"()",
        R"(<CashTransaction transactionID="" levelOfDetail="DETAIL" actionID="" type="Broker Interest Paid" assetCategory="CASH" description="EUR CREDIT INT" isin="" currency="EUR" dateTime="20240403" reportDate="20240403" amount="-1"/>)"));

    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_EQ(result.mStatement.mLeftOutRows.size(), 1U);
}

TEST(IbkrParserCashRowTest, IgnoredCashRowNeedsNoTransactionId) {
    // A fee row is not read at all, so its ID is not needed.
    const auto result = parseText(statementWithRows(
        R"()",
        R"()",
        R"(<CashTransaction transactionID="" levelOfDetail="DETAIL" actionID="" type="Other Fees" assetCategory="CASH" description="FEE" isin="" currency="USD" dateTime="20240403" reportDate="20240403" amount="-1"/>)"));

    EXPECT_EQ(eventCount(result), 0U);
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(IbkrParserCashRowTest, UnknownCashTypeIsAnErrorWithoutAForm) {
    const auto result = parseText(statementWithRows(
        "",
        "",
        R"(<CashTransaction transactionID="C6" levelOfDetail="DETAIL" actionID="" type="Price Adjustments" assetCategory="CASH" description="ADJUSTMENT" isin="" currency="USD" dateTime="20240315" reportDate="20240318" amount="1"/>)"));

    EXPECT_EQ(eventCount(result), 0U);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::UnknownRowType);
    EXPECT_EQ(result.mDiagnostics.front().mMessageKey, MessageKey::UnknownRowType);
    EXPECT_EQ(result.mDiagnostics.front().mField, "type");
    EXPECT_FALSE(result.mDiagnostics.front().mRowKind.has_value()); // it may belong to any form
    ASSERT_EQ(result.mDiagnostics.front().mMessageParams.size(), 2U);
    EXPECT_EQ(result.mDiagnostics.front().mMessageParams[0],
              taxbroker::textParam("type", "Price Adjustments"));
    EXPECT_EQ(result.mDiagnostics.front().mMessageParams[1],
              taxbroker::textParam("category", "CashTransaction"));
}

TEST(IbkrParserCashRowTest, IgnoredTypesGiveNothing) {
    const auto result = parseText(statementWithRows(
        "",
        "",
        R"(<CashTransaction transactionID="C7" levelOfDetail="DETAIL" actionID="" type="Other Fees" assetCategory="CASH" description="FEE" isin="" currency="USD" dateTime="20240315" reportDate="20240318" amount="-1"/>
        <CashTransaction transactionID="C8" levelOfDetail="DETAIL" actionID="" type="Commission Adjustments" assetCategory="CASH" description="ADJ" isin="" currency="USD" dateTime="20240315" reportDate="20240318" amount="1"/>
        <CashTransaction transactionID="C9" levelOfDetail="DETAIL" actionID="" type="Advisor Fees" assetCategory="CASH" description="ADV" isin="" currency="USD" dateTime="20240315" reportDate="20240318" amount="-1"/>
        <CashTransaction transactionID="C10" levelOfDetail="DETAIL" actionID="" type="Bond Interest Paid" assetCategory="BOND" description="ACCRUED" isin="DE0000000041" currency="EUR" dateTime="20240315" reportDate="20240318" amount="-1"/>)"));

    EXPECT_EQ(eventCount(result), 0U);
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(IbkrParserCashRowTest, CashRowWithoutTheTypeAttributeIsRejected) {
    const auto result = parseText(statementWithRows(
        "",
        "",
        R"(<CashTransaction transactionID="C11" levelOfDetail="DETAIL" actionID="A11" assetCategory="STK" description="SYNC" isin="US0000000036" currency="USD" dateTime="20240315" reportDate="20240318" amount="12"/>)"));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(result.mDiagnostics.front().mField, "type");
    EXPECT_EQ(eventCount(result), 0U);

    // The kind of the row is not known, so it is named a cash row (not a dividend row) and the
    // error names no form.
    EXPECT_FALSE(result.mDiagnostics.front().mRowKind.has_value());
    ASSERT_EQ(result.mDiagnostics.front().mMessageParams.size(), 2U);
    EXPECT_EQ(result.mDiagnostics.front().mMessageParams[1],
              taxbroker::termParam("rowKind", taxbroker::term::kRowKindCash));
}

TEST(IbkrParserCashRowTest, DerivativeCashRowIsLeftOut) {
    const auto result = parseText(statementWithRows(
        "",
        "",
        R"(<CashTransaction transactionID="C12" levelOfDetail="DETAIL" actionID="" type="Dividends" assetCategory="OPT" description="OPTION" isin="" currency="USD" dateTime="20240315" reportDate="20240318" amount="12"/>)"));

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mLeftOutRows.size(), 1U);
    EXPECT_TRUE(result.mStatement.mIncomePaymentRows.empty());
}

TEST(IbkrParserCashRowTest, PaymentRowWithoutTheIsinAttributeIsRejected) {
    const auto result = parseText(statementWithRows(
        "",
        "",
        R"(<CashTransaction transactionID="C13" levelOfDetail="DETAIL" actionID="A13" type="Dividends" assetCategory="STK" description="SYNC" currency="USD" dateTime="20240315" reportDate="20240318" amount="12"/>)"));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(result.mDiagnostics.front().mField, "isin");
    EXPECT_EQ(result.mDiagnostics.front().mRowKind, ParsedRowKind::Dividend);
}

TEST(IbkrParserCashRowTest, WithholdingWithAnEmptyIsinCannotBeAssignedToAPayment) {
    // The tax on cash interest has no ISIN; it is reported instead of being attached to nothing.
    // The tax may be on a dividend or on interest, so the row kind is the tax row.
    const auto result = parseText(statementWithRows(
        R"()",
        R"()",
        R"(<CashTransaction transactionID="C14" levelOfDetail="DETAIL" actionID="A14" type="Withholding Tax" assetCategory="CASH" description="INTEREST TAX" isin="" currency="USD" dateTime="20240315" reportDate="20240318" amount="-1"/>)"));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(result.mDiagnostics.front().mField, "isin");
    EXPECT_EQ(result.mDiagnostics.front().mRowKind, ParsedRowKind::IncomeTax);
    ASSERT_EQ(result.mDiagnostics.front().mMessageParams.size(), 2U);
    EXPECT_EQ(result.mDiagnostics.front().mMessageParams[1],
              taxbroker::termParam("rowKind", taxbroker::term::kRowKindIncomeTax));
    EXPECT_EQ(eventCount(result), 0U);
}

TEST(IbkrParserCashRowTest, WithholdingWithoutTheIsinAttributeIsATaxRowError) {
    const auto result = parseText(statementWithRows(
        R"()",
        R"()",
        R"(<CashTransaction transactionID="C14b" levelOfDetail="DETAIL" actionID="A14" type="Withholding Tax" assetCategory="CASH" description="INTEREST TAX" currency="USD" dateTime="20240315" reportDate="20240318" amount="-1"/>)"));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(result.mDiagnostics.front().mField, "isin");
    EXPECT_EQ(result.mDiagnostics.front().mRowKind, ParsedRowKind::IncomeTax);
}

TEST(IbkrParserCashRowTest, WithholdingWithAnIsinStaysADividendRow) {
    // The tax names its instrument, so a fault in it concerns the dividend only.
    const auto result = parseText(statementWithRows(
        R"()",
        R"()",
        R"(<CashTransaction transactionID="C14c" levelOfDetail="DETAIL" actionID="A14" type="Withholding Tax" assetCategory="STK" description="SYNC TAX" isin="US0000000036" currency="USD" dateTime="20240315" reportDate="20240318" amount="abc"/>)"));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mField, "amount");
    EXPECT_EQ(result.mDiagnostics.front().mRowKind, ParsedRowKind::Dividend);
}

TEST(IbkrParserCashRowTest, PaymentRowWithoutTheActionIdAttributeIsRejected) {
    const auto result = parseText(statementWithRows(
        "",
        "",
        R"(<CashTransaction transactionID="C15" levelOfDetail="DETAIL" type="Dividends" assetCategory="STK" description="SYNC" isin="US0000000036" currency="USD" dateTime="20240315" reportDate="20240318" amount="12"/>)"));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(result.mDiagnostics.front().mField, "actionID");
}

TEST(IbkrParserCashRowTest, PaymentRowWithAnEmptyActionIdIsRejected) {
    // Without an action ID the rows of one payment cannot be added up.
    const auto result = parseText(statementWithRows(
        "",
        "",
        R"(<CashTransaction transactionID="C16" levelOfDetail="DETAIL" actionID="" type="Dividends" assetCategory="STK" description="SYNC" isin="US0000000036" currency="USD" dateTime="20240315" reportDate="20240318" amount="12"/>)"));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(result.mDiagnostics.front().mField, "actionID");
    EXPECT_EQ(eventCount(result), 0U);
}

TEST(IbkrParserCashRowTest, PaymentRowWithoutTheReportDateAttributeIsRejected) {
    const auto result = parseText(statementWithRows(
        "",
        "",
        R"(<CashTransaction transactionID="C17" levelOfDetail="DETAIL" actionID="A17" type="Dividends" assetCategory="STK" description="SYNC" isin="US0000000036" currency="USD" dateTime="20240315" amount="12"/>)"));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(result.mDiagnostics.front().mField, "reportDate");
}

TEST(IbkrParserCashRowTest, PaymentRowWithABadReportDateIsRejected) {
    const auto result = parseText(statementWithRows(
        "",
        "",
        R"(<CashTransaction transactionID="C18" levelOfDetail="DETAIL" actionID="A18" type="Dividends" assetCategory="STK" description="SYNC" isin="US0000000036" currency="USD" dateTime="20240315" reportDate="2024-03-18" amount="12"/>)"));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(result.mDiagnostics.front().mField, "reportDate");
}

TEST(IbkrParserCashRowTest, PaymentRowWithABadDateTimeIsRejected) {
    const auto result = parseText(statementWithRows(
        "",
        "",
        R"(<CashTransaction transactionID="C19" levelOfDetail="DETAIL" actionID="A19" type="Dividends" assetCategory="STK" description="SYNC" isin="US0000000036" currency="USD" dateTime="15.03.2024" reportDate="20240318" amount="12"/>)"));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mField, "dateTime");
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidValue);
}

TEST(IbkrParserCashRowTest, PaymentRowWithAnUnknownCurrencyIsRejected) {
    const auto result = parseText(statementWithRows(
        "",
        "",
        R"(<CashTransaction transactionID="C20" levelOfDetail="DETAIL" actionID="A20" type="Dividends" assetCategory="STK" description="SYNC" isin="US0000000036" currency="XXX" dateTime="20240315" reportDate="20240318" amount="12"/>)"));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mField, "currency");
}

TEST(IbkrParserCashRowTest, PaymentRowWithABadAmountIsRejected) {
    const auto result = parseText(statementWithRows(
        "",
        "",
        R"(<CashTransaction transactionID="C21" levelOfDetail="DETAIL" actionID="A21" type="Dividends" assetCategory="STK" description="SYNC" isin="US0000000036" currency="USD" dateTime="20240315" reportDate="20240318" amount="12,50"/>)"));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mField, "amount");
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidValue);
}

TEST(IbkrParserCashRowTest, PaymentRowWithoutTheAmountAttributeIsRejected) {
    const auto result = parseText(statementWithRows(
        "",
        "",
        R"(<CashTransaction transactionID="C22" levelOfDetail="DETAIL" actionID="A22" type="Dividends" assetCategory="STK" description="SYNC" isin="US0000000036" currency="USD" dateTime="20240315" reportDate="20240318"/>)"));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(result.mDiagnostics.front().mField, "amount");
}

TEST(IbkrParserCashRowTest, NegativeBrokerInterestIsKept) {
    // Debit interest on cash is read as it is; a later stage warns about it.
    const auto result = parseText(statementWithRows(
        "",
        "",
        R"(<CashTransaction transactionID="C23" levelOfDetail="DETAIL" actionID="" type="Broker Interest Received" assetCategory="CASH" description="EUR DEBIT INT" isin="" currency="EUR" dateTime="20240403" reportDate="20240403" amount="-0.5"/>)"));

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_NE(findInterest(result, "C23"), nullptr);
    EXPECT_EQ(findInterest(result, "C23")->mGrossAmount, -5'000);
}

TEST(IbkrParserCashRowTest, BrokerInterestWithABadAmountIsRejected) {
    const auto result = parseText(statementWithRows(
        "",
        "",
        R"(<CashTransaction transactionID="C24" levelOfDetail="DETAIL" actionID="" type="Broker Interest Received" assetCategory="CASH" description="EUR CREDIT INT" isin="" currency="EUR" dateTime="20240403" reportDate="20240403" amount="x"/>)"));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mField, "amount");
    EXPECT_EQ(result.mDiagnostics.front().mRowKind, ParsedRowKind::BrokerInterest);
}

TEST(IbkrParserCashRowTest, BrokerInterestWithoutTheAmountAttributeIsRejected) {
    const auto result = parseText(statementWithRows(
        "",
        "",
        R"(<CashTransaction transactionID="C25" levelOfDetail="DETAIL" actionID="" type="Broker Interest Received" assetCategory="CASH" description="EUR CREDIT INT" isin="" currency="EUR" dateTime="20240403" reportDate="20240403"/>)"));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(result.mDiagnostics.front().mField, "amount");
}

TEST(IbkrParserCashRowTest, NegativeCouponIsRejected) {
    const auto result = parseText(statementWithRows(
        "",
        "",
        R"(<CashTransaction transactionID="C26" levelOfDetail="DETAIL" actionID="" type="Bond Interest Received" assetCategory="BOND" description="SYNBOND COUPON" isin="DE0000000041" currency="EUR" dateTime="20240617" reportDate="20240617" amount="-45"/>)"));

    EXPECT_EQ(eventCount(result), 0U);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(result.mDiagnostics.front().mField, "amount");
    EXPECT_EQ(result.mDiagnostics.front().mRowKind, ParsedRowKind::BondInterest);
}

TEST(IbkrParserCashRowTest, CouponWithoutTheIsinAttributeIsRejected) {
    const auto result = parseText(statementWithRows(
        "",
        "",
        R"(<CashTransaction transactionID="C27" levelOfDetail="DETAIL" actionID="" type="Bond Interest Received" assetCategory="BOND" description="SYNBOND COUPON" currency="EUR" dateTime="20240617" reportDate="20240617" amount="45"/>)"));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(result.mDiagnostics.front().mField, "isin");
}

TEST(IbkrParserCashRowTest, CouponWithAnEmptyIsinIsAMissingField) {
    const auto result = parseText(statementWithRows(
        "",
        "",
        R"(<CashTransaction transactionID="C28" levelOfDetail="DETAIL" actionID="" type="Bond Interest Received" assetCategory="BOND" description="SYNBOND COUPON" isin="" currency="EUR" dateTime="20240617" reportDate="20240617" amount="45"/>)"));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(result.mDiagnostics.front().mField, "isin");
}

TEST(IbkrParserCashRowTest, TwoCouponsOfOneBondShareOneInterestInstrument) {
    const auto result = parseText(statementWithRows(
        "",
        "",
        R"(<CashTransaction transactionID="C29" levelOfDetail="DETAIL" actionID="" type="Bond Interest Received" assetCategory="BOND" description="SYNBOND COUPON" isin="DE0000000041" currency="EUR" dateTime="20240617" reportDate="20240617" amount="45"/>
        <CashTransaction transactionID="C30" levelOfDetail="DETAIL" actionID="" type="Bond Interest Received" assetCategory="BOND" description="SYNBOND COUPON" isin="DE0000000041" currency="EUR" dateTime="20241217" reportDate="20241217" amount="45"/>
        <CashTransaction transactionID="C31" levelOfDetail="DETAIL" actionID="" type="Bond Interest Received" assetCategory="BOND" description="OTHER COUPON" isin="FR0000000085" currency="EUR" dateTime="20241217" reportDate="20241217" amount="10"/>)"));

    ASSERT_EQ(result.mStatement.mInterestInstruments.size(), 2U);
    EXPECT_EQ(result.mStatement.mInterestInstruments[0].mTransactions.size(), 2U);
    EXPECT_EQ(result.mStatement.mInterestInstruments[1].mTransactions.size(), 1U);
}

TEST(IbkrParserCashRowTest, InterestWithABadDateIsRejected) {
    const auto result = parseText(statementWithRows(
        "",
        "",
        R"(<CashTransaction transactionID="C32" levelOfDetail="DETAIL" actionID="" type="Broker Interest Received" assetCategory="CASH" description="EUR CREDIT INT" isin="" currency="EUR" dateTime="April" reportDate="20240403" amount="1"/>)"));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mField, "dateTime");
}

// Rows outside the sections

TEST(IbkrParserStructureTest, UnknownSectionsAndElementsAreNotRead) {
    const std::string xml =
        R"(<FlexQueryResponse queryName="synthetic" type="AF">
  <FlexStatements count="1">
    <FlexStatement accountId="U0000000">
      <AccountInformation accountId="U0000000" ibEntity="IB-IE"/>
      <EquitySummaryInBase><EquitySummaryByReportDateInBase total="1"/></EquitySummaryInBase>
      <Transfers><Transfer transactionID="4001" type="INTERNAL"/></Transfers>
      <CashTransactions><SomethingElse transactionID="1"/></CashTransactions>
      <CorporateActions><SomethingElse transactionID="2"/></CorporateActions>
    </FlexStatement>
  </FlexStatements>
</FlexQueryResponse>
)";
    const auto result = parseText(xml);

    EXPECT_EQ(eventCount(result), 0U);

    // The unknown sections are skipped without a diagnostic. The unknown element inside the cash
    // section and the one inside the action section are rows the app cannot read: one notice,
    // count 2.
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mSeverity, DiagnosticSeverity::Info);
    EXPECT_EQ(result.mDiagnostics.front().mMessageKey, MessageKey::UnsupportedRowTypeNotDetail);
    EXPECT_EQ(result.mDiagnostics.front().mMessageParams.front(),
              taxbroker::numberParam("count", 2));
}

TEST(IbkrParserStructureTest, TwoStatementsOfOneFileKeepOneEventSequence) {
    const auto result = parseFixture("supported.xml");
    const auto* first = findTrade(result, "1001");
    const auto* second = findTrade(result, "5001");

    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    // The event index grows over the whole file, not per statement.
    EXPECT_LT(first->mMetadata.mSources.front().mInputSequence.mEventIndex,
              second->mMetadata.mSources.front().mInputSequence.mEventIndex);
}

// Running out of memory

TEST(IbkrParserFileLevelTest, RunningOutOfMemoryGivesAStructuredErrorNotAnException) {
    SKIP_IF_ALLOCATION_HOOK_IS_UNAVAILABLE();

    // The description is the size of a large allocation, so copying it for the instrument fails.
    const std::string description(taxbroker::test::kLargeAllocationBytes, 'x');
    const std::string xml = statementWithRows(
        std::string{
            R"(<Trade transactionID="T60" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description=")"} +
            description +
            R"(" isin="US0000000036" currency="USD" tradeDate="20240701" dateTime="20240701;140000" buySell="BUY" quantity="5" tradePrice="20" proceeds="-100" ibCommission="-1" ibCommissionCurrency="USD" taxes="0" notes="" multiplier="1"/>)",
        "",
        "");

    ParseResult result;

    {
        const taxbroker::test::ScopedAllocationFailure failure;
        IbkrParser parser;

        result = parser.parse(xml, "memory.xml", 0);
    }

    EXPECT_EQ(eventCount(result), 0U);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics.front().mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(result.mDiagnostics.front().mCode, DiagnosticCode::InputFileOutOfMemory);
    EXPECT_EQ(result.mDiagnostics.front().mMessageKey, MessageKey::InputFileOutOfMemory);
    EXPECT_EQ(result.mDiagnostics.front().mSourceFile, "memory.xml");
}
