#include "parsers/traderepublic_parser.hpp"
#include "taxbroker/errors.hpp"
#include "taxbroker/types.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace taxbroker;

// All names, identifiers, values, and CSV rows in this test suite are synthetic.
constexpr std::string_view kCsvHeader =
    "\"datetime\",\"date\",\"account_type\",\"category\",\"type\",\"asset_class\","
    "\"name\",\"symbol\",\"shares\",\"price\",\"amount\",\"fee\",\"tax\",\"currency\","
    "\"original_amount\",\"original_currency\",\"fx_rate\",\"description\",\"transaction_id\","
    "\"counterparty_name\",\"counterparty_iban\",\"payment_reference\",\"mcc_code\"";

struct SyntheticCsvRow {
    std::string mDatetime{"2024-01-15T10:00:00.000Z"};
    std::string mDate{"2024-01-15"};
    std::string mAccountType{"DEFAULT"};
    std::string mCategory{"TRADING"};
    std::string mType{"BUY"};
    std::string mAssetClass{"STOCK"};
    std::string mName{"Synthetic Test Share"};
    std::string mSymbol{"XX9000000001"};
    std::string mShares{"1.0000000000"};
    std::string mPrice{"10.000000"};
    std::string mAmount{"-10.000000"};
    std::string mFee;
    std::string mTax;
    std::string mCurrency{"EUR"};
    std::string mOriginalAmount;
    std::string mOriginalCurrency;
    std::string mFxRate;
    std::string mDescription{"Synthetic test row"};
    std::string mTransactionId{"synthetic-test-transaction"};
    std::string mCounterpartyName;
    std::string mCounterpartyIban;
    std::string mPaymentReference;
    std::string mMccCode;
};

void writeCsvField(std::ostream& aOutput, std::string_view aValue) {
    aOutput << '"';
    for (const char character : aValue)
    {
        if (character == '"')
        {
            aOutput << "\"\"";
        }
        else
        {
            aOutput << character;
        }
    }
    aOutput << '"';
}

void writeCsvRow(std::ostream& aOutput, const SyntheticCsvRow& aRow) {
    const std::array<std::string_view, 23> fields{
        aRow.mDatetime,
        aRow.mDate,
        aRow.mAccountType,
        aRow.mCategory,
        aRow.mType,
        aRow.mAssetClass,
        aRow.mName,
        aRow.mSymbol,
        aRow.mShares,
        aRow.mPrice,
        aRow.mAmount,
        aRow.mFee,
        aRow.mTax,
        aRow.mCurrency,
        aRow.mOriginalAmount,
        aRow.mOriginalCurrency,
        aRow.mFxRate,
        aRow.mDescription,
        aRow.mTransactionId,
        aRow.mCounterpartyName,
        aRow.mCounterpartyIban,
        aRow.mPaymentReference,
        aRow.mMccCode,
    };

    for (std::size_t index = 0; index < fields.size(); ++index)
    {
        if (index != 0)
        {
            aOutput << ',';
        }
        writeCsvField(aOutput, fields[index]);
    }
    aOutput << '\n';
}

std::string sanitizedTestName() {
    const auto* testInfo = testing::UnitTest::GetInstance()->current_test_info();
    std::string name = std::string{testInfo->test_suite_name()} + '_' + testInfo->name();
    std::replace_if(
        name.begin(),
        name.end(),
        [](const char character) {
            return std::isalnum(static_cast<unsigned char>(character)) == 0;
        },
        '_');
    return name;
}

class TemporaryCsvFile {
  public:
    explicit TemporaryCsvFile(std::initializer_list<SyntheticCsvRow> aRows)
        : mPath{std::filesystem::temp_directory_path() /
                ("taxbroker_" + sanitizedTestName() + ".csv")} {
        std::ofstream output{mPath, std::ios::trunc};
        if (!output)
        {
            throw std::runtime_error{"Unable to create synthetic CSV fixture"};
        }

        output << kCsvHeader << '\n';
        for (const auto& row : aRows)
        {
            writeCsvRow(output, row);
        }

        if (!output)
        {
            throw std::runtime_error{"Unable to write synthetic CSV fixture"};
        }
    }

    TemporaryCsvFile(const TemporaryCsvFile&) = delete;
    TemporaryCsvFile& operator=(const TemporaryCsvFile&) = delete;

    ~TemporaryCsvFile() {
        std::error_code error;
        std::filesystem::remove(mPath, error);
    }

    const std::filesystem::path& path() const {
        return mPath;
    }

  private:
    std::filesystem::path mPath;
};

ParseResult parseRows(std::initializer_list<SyntheticCsvRow> aRows) {
    const TemporaryCsvFile csvFile{aRows};
    tr::TradeRepublicParser parser;
    return parser.parse(csvFile.path());
}

Date makeDate(int aYear, unsigned aMonth, unsigned aDay) {
    const auto calendarDate =
        std::chrono::year{aYear} / std::chrono::month{aMonth} / std::chrono::day{aDay};
    return Date{std::chrono::sys_days{calendarDate}.time_since_epoch()};
}

const TradeInstrument* findTradeInstrument(const BrokerStatement& aStatement,
                                           std::string_view aIsin) {
    const auto instrument = std::find_if(
        aStatement.mTradeInstruments.begin(),
        aStatement.mTradeInstruments.end(),
        [&](const TradeInstrument& aTradeInstrument) { return aTradeInstrument.mIsin == aIsin; });

    return instrument == aStatement.mTradeInstruments.end() ? nullptr : &*instrument;
}

const TradeTransaction* findTradeTransaction(const BrokerStatement& aStatement,
                                             std::string_view aTransactionId) {
    for (const auto& instrument : aStatement.mTradeInstruments)
    {
        const auto transaction = std::find_if(instrument.mTransactions.begin(),
                                              instrument.mTransactions.end(),
                                              [&](const TradeTransaction& aTrade) {
                                                  return aTrade.mTransactionId == aTransactionId;
                                              });
        if (transaction != instrument.mTransactions.end())
        {
            return &*transaction;
        }
    }
    return nullptr;
}

const DividendTransaction* findDividendTransaction(const BrokerStatement& aStatement,
                                                   std::string_view aTransactionId) {
    for (const auto& instrument : aStatement.mDividendInstruments)
    {
        const auto transaction = std::find_if(instrument.mTransactions.begin(),
                                              instrument.mTransactions.end(),
                                              [&](const DividendTransaction& aDividend) {
                                                  return aDividend.mTransactionId == aTransactionId;
                                              });
        if (transaction != instrument.mTransactions.end())
        {
            return &*transaction;
        }
    }
    return nullptr;
}

const InterestInstrument* findInterestInstrument(const BrokerStatement& aStatement,
                                                 InterestType aInterestType) {
    const auto instrument = std::find_if(aStatement.mInterestInstruments.begin(),
                                         aStatement.mInterestInstruments.end(),
                                         [&](const InterestInstrument& aInterest) {
                                             return aInterest.mInterestType == aInterestType;
                                         });
    return instrument == aStatement.mInterestInstruments.end() ? nullptr : &*instrument;
}

const PrivateMarketEvent* findPrivateMarketEvent(const BrokerStatement& aStatement,
                                                 std::string_view aTransactionId) {
    const auto event = std::find_if(aStatement.mPrivateMarketEvents.begin(),
                                    aStatement.mPrivateMarketEvents.end(),
                                    [&](const PrivateMarketEvent& aPrivateEvent) {
                                        return aPrivateEvent.mTransactionId == aTransactionId;
                                    });
    return event == aStatement.mPrivateMarketEvents.end() ? nullptr : &*event;
}

std::size_t tradeTransactionCount(const BrokerStatement& aStatement) {
    return std::accumulate(aStatement.mTradeInstruments.begin(),
                           aStatement.mTradeInstruments.end(),
                           std::size_t{0},
                           [](std::size_t aCount, const TradeInstrument& aInstrument) {
                               return aCount + aInstrument.mTransactions.size();
                           });
}

const BenefitEvent* findBenefitEvent(const BrokerStatement& aStatement, BenefitType aBenefitType) {
    const auto benefit =
        std::find_if(aStatement.mBenefitEvents.begin(),
                     aStatement.mBenefitEvents.end(),
                     [&](const BenefitEvent& aEvent) { return aEvent.mType == aBenefitType; });

    return benefit == aStatement.mBenefitEvents.end() ? nullptr : &*benefit;
}

const BenefitEvent* findBenefitEvent(const BrokerStatement& aStatement,
                                     std::string_view aTransactionId) {
    const auto benefit = std::find_if(
        aStatement.mBenefitEvents.begin(),
        aStatement.mBenefitEvents.end(),
        [&](const BenefitEvent& aEvent) { return aEvent.mTransactionId == aTransactionId; });

    return benefit == aStatement.mBenefitEvents.end() ? nullptr : &*benefit;
}

std::size_t countWarnings(const ParseResult& aParseResult, WarningCode aWarningCode) {
    return static_cast<std::size_t>(std::count_if(
        aParseResult.mWarnings.begin(),
        aParseResult.mWarnings.end(),
        [&](const ParseWarning& aWarning) { return aWarning.mCode == aWarningCode; }));
}

bool hasWarningContaining(const ParseResult& aParseResult, std::string_view aText) {
    return std::any_of(aParseResult.mWarnings.begin(),
                       aParseResult.mWarnings.end(),
                       [&](const ParseWarning& aWarning) {
                           return aWarning.mMessage.find(aText) != std::string::npos;
                       });
}

std::filesystem::path fixturePath() {
    return std::filesystem::path{__FILE__}.parent_path().parent_path() / "test_data" / "csv" /
           "traderepublic_parser_fixture.csv";
}

std::filesystem::path supportedFixturePath() {
    return std::filesystem::path{__FILE__}.parent_path().parent_path() / "test_data" / "csv" /
           "traderepublic_parser_supported_fixture.csv";
}

ParseResult parseFixture() {
    tr::TradeRepublicParser parser;
    return parser.parse(fixturePath());
}

ParseResult parseSupportedFixture() {
    tr::TradeRepublicParser parser;
    return parser.parse(supportedFixturePath());
}

void expectNoParsedRecords(const ParseResult& aParseResult) {
    const auto& statement = aParseResult.mStatement;
    EXPECT_TRUE(statement.mTradeInstruments.empty());
    EXPECT_TRUE(statement.mDividendInstruments.empty());
    EXPECT_TRUE(statement.mInterestInstruments.empty());
    EXPECT_TRUE(statement.mBenefitEvents.empty());
    EXPECT_TRUE(statement.mPrivateMarketEvents.empty());
}

SyntheticCsvRow makeDividendRow() {
    SyntheticCsvRow row;
    row.mCategory = "CASH";
    row.mType = "DIVIDEND";
    row.mShares.clear();
    row.mPrice.clear();
    row.mAmount = "10.00";
    row.mTax = "-1.00";
    row.mTransactionId = "synthetic-dividend-validation";
    return row;
}

SyntheticCsvRow makeBrokerInterestRow() {
    SyntheticCsvRow row = makeDividendRow();
    row.mType = "INTEREST_PAYMENT";
    row.mAssetClass.clear();
    row.mName.clear();
    row.mSymbol.clear();
    row.mTransactionId = "synthetic-broker-interest-validation";
    return row;
}

SyntheticCsvRow makeBondInterestRow() {
    SyntheticCsvRow row = makeDividendRow();
    row.mType = "BOND_INTEREST";
    row.mAssetClass = "BOND";
    row.mName = "Synthetic Validation Bond";
    row.mSymbol = "XX9000000002";
    row.mTransactionId = "synthetic-bond-interest-validation";
    return row;
}

SyntheticCsvRow makeCorporateActionRow() {
    SyntheticCsvRow row;
    row.mCategory = "CORPORATE_ACTION";
    row.mType = "SPLIT";
    row.mShares = "1.00";
    row.mPrice.clear();
    row.mAmount.clear();
    row.mCurrency.clear();
    row.mTransactionId = "synthetic-split-validation";
    return row;
}

SyntheticCsvRow makeBenefitRow() {
    SyntheticCsvRow row;
    row.mCategory = "CASH";
    row.mType = "BENEFITS_SAVEBACK";
    row.mAssetClass.clear();
    row.mName.clear();
    row.mSymbol.clear();
    row.mShares.clear();
    row.mPrice.clear();
    row.mAmount = "2.00";
    row.mTransactionId = "synthetic-benefit-validation";
    return row;
}

SyntheticCsvRow makePrivateMarketRow() {
    SyntheticCsvRow row;
    row.mCategory = "CASH";
    row.mType = "PRIVATE_MARKET_BUY";
    row.mAssetClass = "PRIVATE_FUND";
    row.mName = "Synthetic Validation Private Fund";
    row.mSymbol = "XX9000000003";
    row.mShares.clear();
    row.mPrice.clear();
    row.mAmount = "-100.00";
    row.mTransactionId = "synthetic-private-market-validation";
    return row;
}

TEST(TradeRepublicParserTest, PreservesBenefitsWithoutDuplicatingBuyTransactions) {
    const ParseResult parseResult = parseFixture();

    const auto transactionCount =
        std::accumulate(parseResult.mStatement.mTradeInstruments.begin(),
                        parseResult.mStatement.mTradeInstruments.end(),
                        std::size_t{0},
                        [](std::size_t aCount, const TradeInstrument& aInstrument) {
                            return aCount + aInstrument.mTransactions.size();
                        });
    EXPECT_EQ(transactionCount, 3U);

    const auto* stockperkInstrument = findTradeInstrument(parseResult.mStatement, "XX1000000001");
    ASSERT_NE(stockperkInstrument, nullptr);
    ASSERT_EQ(stockperkInstrument->mTransactions.size(), 1U);
    EXPECT_EQ(stockperkInstrument->mTransactions.front().mUnits, 10'000'000);
    ASSERT_TRUE(stockperkInstrument->mTransactions.front().mAmount.has_value());
    EXPECT_EQ(*stockperkInstrument->mTransactions.front().mAmount, 123'400);
    EXPECT_EQ(stockperkInstrument->mTransactions.front().mFeePaid, 0);
    EXPECT_EQ(stockperkInstrument->mTransactions.front().mTransactionId, "synthetic-stockperk-buy");

    const auto* savebackInstrument = findTradeInstrument(parseResult.mStatement, "XX1000000002");
    ASSERT_NE(savebackInstrument, nullptr);
    ASSERT_EQ(savebackInstrument->mTransactions.size(), 1U);
    EXPECT_EQ(savebackInstrument->mTransactions.front().mUnits, 25'000'000);
    ASSERT_TRUE(savebackInstrument->mTransactions.front().mAmount.has_value());
    EXPECT_EQ(*savebackInstrument->mTransactions.front().mAmount, 25'000);
    EXPECT_EQ(savebackInstrument->mTransactions.front().mTransactionId, "synthetic-saveback-buy");

    ASSERT_EQ(parseResult.mStatement.mBenefitEvents.size(), 3U);

    const auto* stockperk = findBenefitEvent(parseResult.mStatement, BenefitType::Stockperk);
    ASSERT_NE(stockperk, nullptr);
    EXPECT_EQ(stockperk->mAmount, 123'400);
    EXPECT_EQ(stockperk->mCurrency, Currency::EUR);
    EXPECT_EQ(stockperk->mAssetClass, AssetClass::Stock);
    EXPECT_EQ(stockperk->mName, "Synthetic Reward Share");
    ASSERT_TRUE(stockperk->mIsin.has_value());
    EXPECT_EQ(*stockperk->mIsin, "XX1000000001");
    EXPECT_EQ(stockperk->mTransactionId, "synthetic-stockperk-cash");

    const auto* saveback = findBenefitEvent(parseResult.mStatement, BenefitType::Saveback);
    ASSERT_NE(saveback, nullptr);
    EXPECT_EQ(saveback->mAmount, 25'000);
    EXPECT_EQ(saveback->mCurrency, Currency::EUR);
    EXPECT_EQ(saveback->mAssetClass, AssetClass::Fund);
    EXPECT_EQ(saveback->mName, "Synthetic Reward Fund");
    ASSERT_TRUE(saveback->mIsin.has_value());
    EXPECT_EQ(*saveback->mIsin, "XX1000000002");
    EXPECT_EQ(saveback->mTransactionId, "synthetic-saveback-cash");

    const auto* unassignedSaveback =
        findBenefitEvent(parseResult.mStatement, "synthetic-saveback-unassigned");
    ASSERT_NE(unassignedSaveback, nullptr);
    EXPECT_EQ(unassignedSaveback->mType, BenefitType::Saveback);
    EXPECT_EQ(unassignedSaveback->mAmount, 11'100);
    EXPECT_TRUE(unassignedSaveback->mName.empty());
    EXPECT_FALSE(unassignedSaveback->mIsin.has_value());
    EXPECT_EQ(unassignedSaveback->mAssetClass, AssetClass::Unknown);

    EXPECT_EQ(countWarnings(parseResult, WarningCode::UnsupportedRowType), 0U);
    EXPECT_FALSE(hasWarningContaining(parseResult, "STOCKPERK"));
    EXPECT_FALSE(hasWarningContaining(parseResult, "BENEFITS_SAVEBACK"));
}

TEST(TradeRepublicParserTest, PreservesPrivateMarketEventsAndFundExecution) {
    const ParseResult parseResult = parseFixture();

    const auto* privateFund = findTradeInstrument(parseResult.mStatement, "XX1000000003");
    ASSERT_NE(privateFund, nullptr);
    EXPECT_EQ(privateFund->mAssetClass, AssetClass::PrivateFund);
    ASSERT_EQ(privateFund->mTransactions.size(), 1U);
    EXPECT_FALSE(privateFund->mTransactions.front().mAmount.has_value());
    EXPECT_EQ(privateFund->mTransactions.front().mTransactionId, "synthetic-private-buy");

    ASSERT_EQ(parseResult.mStatement.mPrivateMarketEvents.size(), 1U);
    const auto& prepayment = parseResult.mStatement.mPrivateMarketEvents.front();
    EXPECT_EQ(prepayment.mType, PrivateMarketEventType::Buy);
    EXPECT_EQ(prepayment.mAmount, -2'500'000);
    EXPECT_EQ(prepayment.mFeePaid, 7'500);
    EXPECT_EQ(prepayment.mTransactionId, "synthetic-private-prepayment");

    EXPECT_EQ(countWarnings(parseResult, WarningCode::UnsupportedAssetClass), 1U);
}

TEST(TradeRepublicParserTest, PreservesUnresolvedSplitWithoutCreatingATrade) {
    const ParseResult parseResult = parseFixture();

    const auto* splitInstrument = findTradeInstrument(parseResult.mStatement, "XX1000000004");
    ASSERT_NE(splitInstrument, nullptr);
    EXPECT_TRUE(splitInstrument->mTransactions.empty());
    ASSERT_EQ(splitInstrument->mCorporateActions.size(), 1U);
    EXPECT_EQ(splitInstrument->mCorporateActions.front().mType, CorporateActionType::Split);
    EXPECT_EQ(splitInstrument->mCorporateActions.front().mUnitsDelta, 75'000'000);
    EXPECT_FALSE(splitInstrument->mCorporateActions.front().mRatio.has_value());
    EXPECT_EQ(splitInstrument->mCorporateActions.front().mTransactionId, "synthetic-split-action");
}

TEST(TradeRepublicParserTest, ReportsUnknownRowsWithSourceLocation) {
    const ParseResult parseResult = parseFixture();

    EXPECT_EQ(countWarnings(parseResult, WarningCode::UnsupportedRowType), 0U);
    EXPECT_EQ(countWarnings(parseResult, WarningCode::UnsupportedAssetClass), 1U);
    EXPECT_EQ(countWarnings(parseResult, WarningCode::UnknownRowType), 1U);
    ASSERT_EQ(parseResult.mWarnings.size(), 2U);
    EXPECT_TRUE(hasWarningContaining(parseResult, "FUTURE_EVENT"));
    EXPECT_EQ(parseResult.mWarnings.back().mRowIndex, 11U);
    EXPECT_EQ(parseResult.mWarnings.back().mSourceFile, fixturePath().string());
}

TEST(TradeRepublicParserTest, ParsesAndNormalizesEverySupportedTradeExecution) {
    const ParseResult parseResult = parseSupportedFixture();
    const auto& statement = parseResult.mStatement;

    EXPECT_EQ(tradeTransactionCount(statement), 6U);

    const auto* buy = findTradeTransaction(statement, "synthetic-buy-001");
    ASSERT_NE(buy, nullptr);
    EXPECT_EQ(buy->mDate, makeDate(2024, 2, 29));
    EXPECT_EQ(buy->mTradeSide, TradeSide::Buy);
    EXPECT_EQ(buy->mUnitPrice, 123'457);
    EXPECT_EQ(buy->mUnits, 123'456'790);
    ASSERT_TRUE(buy->mAmount.has_value());
    EXPECT_EQ(*buy->mAmount, 152'400);
    EXPECT_EQ(buy->mFeePaid, 12'500);
    EXPECT_EQ(buy->mExchangeRate, EXCHANGE_RATE_SCALE);
    EXPECT_EQ(buy->mCurrency, Currency::EUR);

    const auto* sell = findTradeTransaction(statement, "synthetic-sell-001");
    ASSERT_NE(sell, nullptr);
    EXPECT_EQ(sell->mTradeSide, TradeSide::Sell);
    EXPECT_EQ(sell->mUnits, 23'456'790);
    ASSERT_TRUE(sell->mAmount.has_value());
    EXPECT_EQ(*sell->mAmount, 30'500);
    EXPECT_EQ(sell->mFeePaid, 0);

    const auto* stock = findTradeInstrument(statement, "XX0000000001");
    ASSERT_NE(stock, nullptr);
    EXPECT_EQ(stock->mName, "Synthetic, \"Quoted\" Share");
    EXPECT_EQ(stock->mAssetClass, AssetClass::Stock);
    EXPECT_EQ(stock->mTransactions.size(), 2U);

    const std::array expectedExecutions{
        std::pair{"synthetic-savings-001", AssetClass::Fund},
        std::pair{"synthetic-spare-001", AssetClass::Bond},
        std::pair{"synthetic-saveback-execution-001", AssetClass::Derivative},
        std::pair{"synthetic-crypto-001", AssetClass::Crypto},
    };
    for (const auto& [transactionId, expectedAssetClass] : expectedExecutions)
    {
        const auto* transaction = findTradeTransaction(statement, transactionId);
        ASSERT_NE(transaction, nullptr) << transactionId;
        EXPECT_EQ(transaction->mTradeSide, TradeSide::Buy) << transactionId;

        const auto instrument = std::find_if(
            statement.mTradeInstruments.begin(),
            statement.mTradeInstruments.end(),
            [&](const TradeInstrument& aInstrument) {
                return std::any_of(aInstrument.mTransactions.begin(),
                                   aInstrument.mTransactions.end(),
                                   [&](const TradeTransaction& aTransaction) {
                                       return aTransaction.mTransactionId == transactionId;
                                   });
            });
        ASSERT_NE(instrument, statement.mTradeInstruments.end());
        EXPECT_EQ(instrument->mAssetClass, expectedAssetClass) << transactionId;
    }

    EXPECT_EQ(countWarnings(parseResult, WarningCode::UnsupportedAssetClass), 1U);
    EXPECT_TRUE(hasWarningContaining(parseResult, "synthetic-crypto-001"));
}

TEST(TradeRepublicParserTest, ParsesLocalAndForeignIncomeCurrenciesExactly) {
    const ParseResult parseResult = parseSupportedFixture();
    const auto& statement = parseResult.mStatement;

    ASSERT_EQ(statement.mDividendInstruments.size(), 5U);
    const auto localInstrument = std::find_if(
        statement.mDividendInstruments.begin(),
        statement.mDividendInstruments.end(),
        [](const DividendInstrument& aInstrument) { return aInstrument.mIsin == "XX0000000001"; });
    ASSERT_NE(localInstrument, statement.mDividendInstruments.end());
    EXPECT_EQ(localInstrument->mTransactions.size(), 2U);

    const auto* eur = findDividendTransaction(statement, "synthetic-dividend-eur-001");
    ASSERT_NE(eur, nullptr);
    EXPECT_EQ(eur->mGrossAmount, 123'400);
    EXPECT_EQ(eur->mTaxPaid, 23'400);
    EXPECT_EQ(eur->mExchangeRate, EXCHANGE_RATE_SCALE);
    EXPECT_EQ(eur->mCurrency, Currency::EUR);
    EXPECT_EQ(eur->mTaxCurrency, Currency::EUR);

    struct ExpectedForeignDividend {
        std::string_view mTransactionId;
        Money mGrossAmount;
        Money mTaxPaid;
        ExchangeRate mExchangeRate;
        Currency mCurrency;
    };
    const std::array expectedForeignDividends{
        ExpectedForeignDividend{"synthetic-distribution-usd-001",
                                100'000,
                                10'000,
                                90'000'000,
                                Currency::USD},
        ExpectedForeignDividend{"synthetic-dividend-gbp-001",
                                40'000,
                                8'000,
                                120'000'000,
                                Currency::GBP},
        ExpectedForeignDividend{"synthetic-dividend-chf-001",
                                50'000,
                                2'500,
                                105'000'000,
                                Currency::CHF},
        ExpectedForeignDividend{"synthetic-dividend-jpy-001", 1'000'000, 0, 610'000, Currency::JPY},
    };

    for (const auto& expected : expectedForeignDividends)
    {
        const auto* dividend = findDividendTransaction(statement, expected.mTransactionId);
        ASSERT_NE(dividend, nullptr) << expected.mTransactionId;
        EXPECT_EQ(dividend->mGrossAmount, expected.mGrossAmount) << expected.mTransactionId;
        EXPECT_EQ(dividend->mTaxPaid, expected.mTaxPaid) << expected.mTransactionId;
        EXPECT_EQ(dividend->mExchangeRate, expected.mExchangeRate) << expected.mTransactionId;
        EXPECT_EQ(dividend->mCurrency, expected.mCurrency) << expected.mTransactionId;
        EXPECT_EQ(dividend->mTaxCurrency, Currency::EUR) << expected.mTransactionId;
    }
}

TEST(TradeRepublicParserTest, GroupsBrokerInterestAndPreservesBondInterest) {
    const ParseResult parseResult = parseSupportedFixture();
    const auto& statement = parseResult.mStatement;

    ASSERT_EQ(statement.mInterestInstruments.size(), 2U);

    const auto* brokerInterest = findInterestInstrument(statement, InterestType::BrokerInterest);
    ASSERT_NE(brokerInterest, nullptr);
    EXPECT_EQ(brokerInterest->mName, "Trade Republic");
    EXPECT_FALSE(brokerInterest->mIsin.has_value());
    ASSERT_EQ(brokerInterest->mTransactions.size(), 2U);
    EXPECT_EQ(brokerInterest->mTransactions[0].mGrossAmount, 12'300);
    EXPECT_EQ(brokerInterest->mTransactions[0].mTaxPaid, 0);
    EXPECT_EQ(brokerInterest->mTransactions[0].mTransactionId, "synthetic-interest-001");
    EXPECT_EQ(brokerInterest->mTransactions[1].mGrossAmount, 20'000);
    EXPECT_EQ(brokerInterest->mTransactions[1].mTaxPaid, 5'000);
    EXPECT_EQ(brokerInterest->mTransactions[1].mCurrency, Currency::EUR);
    EXPECT_EQ(brokerInterest->mTransactions[1].mTaxCurrency, Currency::EUR);

    const auto* bondInterest = findInterestInstrument(statement, InterestType::BondInterest);
    ASSERT_NE(bondInterest, nullptr);
    ASSERT_TRUE(bondInterest->mIsin.has_value());
    EXPECT_EQ(*bondInterest->mIsin, "XX0000000003");
    ASSERT_EQ(bondInterest->mTransactions.size(), 1U);
    const auto& transaction = bondInterest->mTransactions.front();
    EXPECT_EQ(transaction.mGrossAmount, 20'000);
    EXPECT_EQ(transaction.mTaxPaid, 2'000);
    EXPECT_EQ(transaction.mExchangeRate, 90'000'000);
    EXPECT_EQ(transaction.mCurrency, Currency::USD);
    EXPECT_EQ(transaction.mTaxCurrency, Currency::EUR);
    EXPECT_EQ(transaction.mTransactionId, "synthetic-bond-interest-001");
}

TEST(TradeRepublicParserTest, PreservesSplitAndReverseSplitOnTheExistingInstrument) {
    const ParseResult parseResult = parseSupportedFixture();
    const auto* instrument = findTradeInstrument(parseResult.mStatement, "XX0000000001");

    ASSERT_NE(instrument, nullptr);
    ASSERT_EQ(instrument->mCorporateActions.size(), 2U);

    const auto& split = instrument->mCorporateActions[0];
    EXPECT_EQ(split.mType, CorporateActionType::Split);
    EXPECT_EQ(split.mUnitsDelta, 200'000'000);
    EXPECT_FALSE(split.mRatio.has_value());
    EXPECT_EQ(split.mTransactionId, "synthetic-split-001");

    const auto& reverseSplit = instrument->mCorporateActions[1];
    EXPECT_EQ(reverseSplit.mType, CorporateActionType::ReverseSplit);
    EXPECT_EQ(reverseSplit.mUnitsDelta, -50'000'000);
    EXPECT_FALSE(reverseSplit.mRatio.has_value());
    EXPECT_EQ(reverseSplit.mTransactionId, "synthetic-reverse-split-001");
}

TEST(TradeRepublicParserTest, PreservesBenefitsAndEveryPrivateMarketEventType) {
    const ParseResult parseResult = parseSupportedFixture();
    const auto& statement = parseResult.mStatement;

    ASSERT_EQ(statement.mBenefitEvents.size(), 2U);
    const auto* saveback = findBenefitEvent(statement, "synthetic-saveback-001");
    ASSERT_NE(saveback, nullptr);
    EXPECT_EQ(saveback->mType, BenefitType::Saveback);
    EXPECT_EQ(saveback->mAmount, 25'000);
    EXPECT_TRUE(saveback->mName.empty());
    EXPECT_FALSE(saveback->mIsin.has_value());
    EXPECT_EQ(saveback->mAssetClass, AssetClass::Unknown);

    const auto* stockperk = findBenefitEvent(statement, "synthetic-stockperk-001");
    ASSERT_NE(stockperk, nullptr);
    EXPECT_EQ(stockperk->mType, BenefitType::Stockperk);
    EXPECT_EQ(stockperk->mAmount, 75'000);
    ASSERT_TRUE(stockperk->mIsin.has_value());
    EXPECT_EQ(*stockperk->mIsin, "XX0000000009");
    EXPECT_EQ(stockperk->mAssetClass, AssetClass::Stock);

    ASSERT_EQ(statement.mPrivateMarketEvents.size(), 3U);
    const auto* bonus = findPrivateMarketEvent(statement, "synthetic-private-bonus-001");
    ASSERT_NE(bonus, nullptr);
    EXPECT_EQ(bonus->mType, PrivateMarketEventType::Bonus);
    EXPECT_EQ(bonus->mAmount, 30'000);
    EXPECT_EQ(bonus->mFeePaid, 0);
    EXPECT_FALSE(bonus->mIsin.has_value());

    const auto* buy = findPrivateMarketEvent(statement, "synthetic-private-buy-001");
    ASSERT_NE(buy, nullptr);
    EXPECT_EQ(buy->mType, PrivateMarketEventType::Buy);
    EXPECT_EQ(buy->mAmount, -1'000'000);
    EXPECT_EQ(buy->mFeePaid, 10'000);
    EXPECT_EQ(buy->mAssetClass, AssetClass::PrivateFund);

    const auto* sell = findPrivateMarketEvent(statement, "synthetic-private-sell-001");
    ASSERT_NE(sell, nullptr);
    EXPECT_EQ(sell->mType, PrivateMarketEventType::Sell);
    EXPECT_EQ(sell->mAmount, 1'250'000);
    EXPECT_EQ(sell->mFeePaid, 0);
    EXPECT_EQ(sell->mDescription, "Synthetic private-market sell");

    EXPECT_EQ(countWarnings(parseResult, WarningCode::UnknownRowType), 0U);
    EXPECT_EQ(countWarnings(parseResult, WarningCode::UnsupportedRowType), 0U);
}

TEST(TradeRepublicParserTest, HeaderOnlyCsvProducesAnEmptyResult) {
    const ParseResult parseResult = parseRows({});

    EXPECT_TRUE(parseResult.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(parseResult.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(parseResult.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(parseResult.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(parseResult.mStatement.mPrivateMarketEvents.empty());
    EXPECT_TRUE(parseResult.mWarnings.empty());
}

TEST(TradeRepublicParserTest, ParsesAStandaloneDividendInstrument) {
    const SyntheticCsvRow row = makeDividendRow();
    const ParseResult parseResult = parseRows({row});

    ASSERT_EQ(parseResult.mStatement.mDividendInstruments.size(), 1U);
    const auto& instrument = parseResult.mStatement.mDividendInstruments.front();
    EXPECT_EQ(instrument.mName, "Synthetic Test Share");
    EXPECT_EQ(instrument.mIsin, "XX9000000001");
    ASSERT_EQ(instrument.mTransactions.size(), 1U);

    const auto& transaction = instrument.mTransactions.front();
    EXPECT_EQ(transaction.mDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transaction.mGrossAmount, 100'000);
    EXPECT_EQ(transaction.mTaxPaid, 10'000);
    EXPECT_EQ(transaction.mExchangeRate, EXCHANGE_RATE_SCALE);
    EXPECT_EQ(transaction.mCurrency, Currency::EUR);
    EXPECT_EQ(transaction.mTaxCurrency, Currency::EUR);
    EXPECT_EQ(transaction.mTransactionId, "synthetic-dividend-validation");
    EXPECT_TRUE(parseResult.mWarnings.empty());
}

TEST(TradeRepublicParserTest, ReportsAStandaloneUnknownTransaction) {
    SyntheticCsvRow row;
    row.mCategory = "SYNTHETIC_CATEGORY";
    row.mType = "SYNTHETIC_UNKNOWN_EVENT";
    row.mTransactionId = "synthetic-standalone-unknown";

    const TemporaryCsvFile csvFile{{row}};
    tr::TradeRepublicParser parser;
    const ParseResult parseResult = parser.parse(csvFile.path());

    expectNoParsedRecords(parseResult);
    ASSERT_EQ(parseResult.mWarnings.size(), 1U);
    const auto& warning = parseResult.mWarnings.front();
    EXPECT_EQ(warning.mCode, WarningCode::UnknownRowType);
    EXPECT_EQ(warning.mSourceFile, csvFile.path().string());
    EXPECT_EQ(warning.mRowIndex, 2U);
    EXPECT_NE(warning.mMessage.find(row.mType), std::string::npos);
    EXPECT_NE(warning.mMessage.find(row.mTransactionId), std::string::npos);
}

TEST(TradeRepublicParserTest, UsesTheDateColumnAndPreservesTheBrokerReportedAmount) {
    SyntheticCsvRow row;
    row.mDatetime = "synthetic-value-that-is-not-a-timestamp";
    row.mDate = "2024-02-29";
    row.mShares = "2.00";
    row.mPrice = "3.00";
    row.mAmount = "-99.00";

    const ParseResult parseResult = parseRows({row});
    const auto* transaction =
        findTradeTransaction(parseResult.mStatement, "synthetic-test-transaction");

    ASSERT_NE(transaction, nullptr);
    EXPECT_EQ(transaction->mDate, makeDate(2024, 2, 29));
    EXPECT_EQ(transaction->mUnitPrice, 30'000);
    EXPECT_EQ(transaction->mUnits, 200'000'000);
    ASSERT_TRUE(transaction->mAmount.has_value());
    EXPECT_EQ(*transaction->mAmount, 990'000);
}

TEST(TradeRepublicParserTest, ContinuesParsingAfterAnInvalidRow) {
    SyntheticCsvRow invalidRow;
    invalidRow.mPrice = "not-a-number";
    invalidRow.mTransactionId = "synthetic-invalid-transaction";

    SyntheticCsvRow validRow;
    validRow.mDate = "2024-01-16";
    validRow.mTransactionId = "synthetic-valid-after-invalid";

    const ParseResult parseResult = parseRows({invalidRow, validRow});

    EXPECT_EQ(tradeTransactionCount(parseResult.mStatement), 1U);
    EXPECT_EQ(findTradeTransaction(parseResult.mStatement, invalidRow.mTransactionId), nullptr);
    const auto* validTransaction =
        findTradeTransaction(parseResult.mStatement, validRow.mTransactionId);
    ASSERT_NE(validTransaction, nullptr);
    EXPECT_EQ(validTransaction->mDate, makeDate(2024, 1, 16));
}

TEST(TradeRepublicParserTest, AcceptsAnOmittedTradeAmountAndAnExplicitZeroFee) {
    SyntheticCsvRow row;
    row.mAmount.clear();
    row.mFee = "0.0000";

    const ParseResult parseResult = parseRows({row});
    ASSERT_EQ(tradeTransactionCount(parseResult.mStatement), 1U);

    const auto* transaction =
        findTradeTransaction(parseResult.mStatement, "synthetic-test-transaction");
    ASSERT_NE(transaction, nullptr);
    EXPECT_FALSE(transaction->mAmount.has_value());
    EXPECT_EQ(transaction->mFeePaid, 0);
    EXPECT_TRUE(parseResult.mWarnings.empty());
}

TEST(TradeRepublicParserTest, RejectsAnAssetClassConflictForTheSameIsin) {
    SyntheticCsvRow stock;
    SyntheticCsvRow conflictingFund = stock;
    conflictingFund.mAssetClass = "FUND";
    conflictingFund.mTransactionId = "synthetic-conflicting-asset";

    const ParseResult parseResult = parseRows({stock, conflictingFund});
    ASSERT_EQ(parseResult.mStatement.mTradeInstruments.size(), 1U);
    EXPECT_EQ(parseResult.mStatement.mTradeInstruments.front().mAssetClass, AssetClass::Stock);
    EXPECT_EQ(parseResult.mStatement.mTradeInstruments.front().mTransactions.size(), 1U);
    EXPECT_EQ(parseResult.mStatement.mTradeInstruments.front().mTransactions.front().mTransactionId,
              "synthetic-test-transaction");
}

TEST(TradeRepublicParserTest, WarnsAboutUnsupportedAssetsOnlyAfterAValidTradeIsPreserved) {
    SyntheticCsvRow malformedCrypto;
    malformedCrypto.mAssetClass = "CRYPTO";
    malformedCrypto.mPrice = "not-a-number";

    const ParseResult parseResult = parseRows({malformedCrypto});
    expectNoParsedRecords(parseResult);
    EXPECT_EQ(countWarnings(parseResult, WarningCode::UnsupportedAssetClass), 0U);
}

struct InvalidRowCase {
    const char* mName;
    void (*mMutate)(SyntheticCsvRow&);
};

void PrintTo(const InvalidRowCase& aCase, std::ostream* aOutput) {
    *aOutput << aCase.mName;
}

std::string invalidRowCaseName(const testing::TestParamInfo<InvalidRowCase>& aInfo) {
    return aInfo.param.mName;
}

class InvalidTradeRowTest : public testing::TestWithParam<InvalidRowCase> {};

TEST_P(InvalidTradeRowTest, SkipsTheRowWithoutCreatingPartialData) {
    SyntheticCsvRow row;
    GetParam().mMutate(row);

    const ParseResult parseResult = parseRows({row});
    expectNoParsedRecords(parseResult);
}

INSTANTIATE_TEST_SUITE_P(
    TradeValidation,
    InvalidTradeRowTest,
    testing::Values(
        InvalidRowCase{"MissingIdentity",
                       +[](SyntheticCsvRow& aRow) {
                           aRow.mName.clear();
                           aRow.mSymbol.clear();
                       }},
        InvalidRowCase{"MissingIsin", +[](SyntheticCsvRow& aRow) { aRow.mSymbol.clear(); }},
        InvalidRowCase{"MissingName", +[](SyntheticCsvRow& aRow) { aRow.mName.clear(); }},
        InvalidRowCase{"ShortDate", +[](SyntheticCsvRow& aRow) { aRow.mDate = "2024-1-15"; }},
        InvalidRowCase{"WrongDateSeparators",
                       +[](SyntheticCsvRow& aRow) { aRow.mDate = "2024/01/15"; }},
        InvalidRowCase{"NonNumericDate", +[](SyntheticCsvRow& aRow) { aRow.mDate = "202A-01-15"; }},
        InvalidRowCase{"ImpossibleDate", +[](SyntheticCsvRow& aRow) { aRow.mDate = "2023-02-29"; }},
        InvalidRowCase{"MissingPrice", +[](SyntheticCsvRow& aRow) { aRow.mPrice.clear(); }},
        InvalidRowCase{"MalformedPrice",
                       +[](SyntheticCsvRow& aRow) { aRow.mPrice = "not-a-number"; }},
        InvalidRowCase{"ZeroPrice", +[](SyntheticCsvRow& aRow) { aRow.mPrice = "0"; }},
        InvalidRowCase{"NegativePrice", +[](SyntheticCsvRow& aRow) { aRow.mPrice = "-0.01"; }},
        InvalidRowCase{"OverflowingPrice",
                       +[](SyntheticCsvRow& aRow) { aRow.mPrice = "922337203685477.5808"; }},
        InvalidRowCase{"MissingUnits", +[](SyntheticCsvRow& aRow) { aRow.mShares.clear(); }},
        InvalidRowCase{"MalformedUnits",
                       +[](SyntheticCsvRow& aRow) { aRow.mShares = "not-a-number"; }},
        InvalidRowCase{"OverflowingUnits",
                       +[](SyntheticCsvRow& aRow) { aRow.mShares = "92233720368.54775808"; }},
        InvalidRowCase{"ZeroUnits", +[](SyntheticCsvRow& aRow) { aRow.mShares = "0"; }},
        InvalidRowCase{"NegativeBuyUnits", +[](SyntheticCsvRow& aRow) { aRow.mShares = "-1.00"; }},
        InvalidRowCase{"PositiveSellUnits",
                       +[](SyntheticCsvRow& aRow) {
                           aRow.mType = "SELL";
                           aRow.mShares = "1.00";
                           aRow.mAmount = "10.00";
                       }},
        InvalidRowCase{"MinimumSignedUnits",
                       +[](SyntheticCsvRow& aRow) {
                           aRow.mType = "SELL";
                           aRow.mShares = "-92233720368.54775808";
                           aRow.mAmount = "10.00";
                       }},
        InvalidRowCase{"MalformedAmount",
                       +[](SyntheticCsvRow& aRow) { aRow.mAmount = "not-a-number"; }},
        InvalidRowCase{"OverflowingAmount",
                       +[](SyntheticCsvRow& aRow) { aRow.mAmount = "-922337203685477.5809"; }},
        InvalidRowCase{"ZeroAmount", +[](SyntheticCsvRow& aRow) { aRow.mAmount = "0"; }},
        InvalidRowCase{"PositiveBuyAmount", +[](SyntheticCsvRow& aRow) { aRow.mAmount = "10.00"; }},
        InvalidRowCase{"NegativeSellAmount",
                       +[](SyntheticCsvRow& aRow) {
                           aRow.mType = "SELL";
                           aRow.mShares = "-1.00";
                           aRow.mAmount = "-10.00";
                       }},
        InvalidRowCase{"MinimumSignedAmount",
                       +[](SyntheticCsvRow& aRow) { aRow.mAmount = "-922337203685477.5808"; }},
        InvalidRowCase{"MalformedFee", +[](SyntheticCsvRow& aRow) { aRow.mFee = "not-a-number"; }},
        InvalidRowCase{"PositiveFee", +[](SyntheticCsvRow& aRow) { aRow.mFee = "0.01"; }},
        InvalidRowCase{"MinimumSignedFee",
                       +[](SyntheticCsvRow& aRow) { aRow.mFee = "-922337203685477.5808"; }},
        InvalidRowCase{"MissingCurrency", +[](SyntheticCsvRow& aRow) { aRow.mCurrency.clear(); }},
        InvalidRowCase{"UnsupportedCurrency",
                       +[](SyntheticCsvRow& aRow) { aRow.mCurrency = "CAD"; }},
        InvalidRowCase{"MissingAssetClass",
                       +[](SyntheticCsvRow& aRow) { aRow.mAssetClass.clear(); }},
        InvalidRowCase{"UnsupportedAssetClass",
                       +[](SyntheticCsvRow& aRow) { aRow.mAssetClass = "COMMODITY"; }}),
    invalidRowCaseName);

struct IncomeKindCase {
    const char* mName;
    SyntheticCsvRow (*mMakeRow)();
};

void PrintTo(const IncomeKindCase& aCase, std::ostream* aOutput) {
    *aOutput << aCase.mName;
}

std::string incomeKindCaseName(const testing::TestParamInfo<IncomeKindCase>& aInfo) {
    return aInfo.param.mName;
}

class InvalidIncomeRowTest : public testing::TestWithParam<IncomeKindCase> {};

TEST_P(InvalidIncomeRowTest, RejectsInvalidCommonIncomeFields) {
    const std::array invalidCases{
        InvalidRowCase{"InvalidDate", +[](SyntheticCsvRow& aRow) { aRow.mDate = "2024-02-30"; }},
        InvalidRowCase{"MalformedTax", +[](SyntheticCsvRow& aRow) { aRow.mTax = "not-a-number"; }},
        InvalidRowCase{"PositiveTax", +[](SyntheticCsvRow& aRow) { aRow.mTax = "1.00"; }},
        InvalidRowCase{"MinimumSignedTax",
                       +[](SyntheticCsvRow& aRow) { aRow.mTax = "-922337203685477.5808"; }},
        InvalidRowCase{"OverflowingTax",
                       +[](SyntheticCsvRow& aRow) { aRow.mTax = "-922337203685477.5809"; }},
        InvalidRowCase{"MissingTaxCurrency",
                       +[](SyntheticCsvRow& aRow) { aRow.mCurrency.clear(); }},
        InvalidRowCase{"UnsupportedTaxCurrency",
                       +[](SyntheticCsvRow& aRow) { aRow.mCurrency = "CAD"; }},
        InvalidRowCase{"MissingGrossAmount", +[](SyntheticCsvRow& aRow) { aRow.mAmount.clear(); }},
        InvalidRowCase{"MalformedGrossAmount",
                       +[](SyntheticCsvRow& aRow) { aRow.mAmount = "not-a-number"; }},
        InvalidRowCase{"OverflowingGrossAmount",
                       +[](SyntheticCsvRow& aRow) { aRow.mAmount = "922337203685477.5808"; }},
        InvalidRowCase{"UnsupportedOriginalCurrency",
                       +[](SyntheticCsvRow& aRow) {
                           aRow.mOriginalAmount = "11.00";
                           aRow.mOriginalCurrency = "CAD";
                           aRow.mFxRate = "0.90";
                       }},
        InvalidRowCase{"MissingOriginalAmount",
                       +[](SyntheticCsvRow& aRow) {
                           aRow.mOriginalAmount.clear();
                           aRow.mOriginalCurrency = "USD";
                           aRow.mFxRate = "0.90";
                       }},
        InvalidRowCase{"MalformedOriginalAmount",
                       +[](SyntheticCsvRow& aRow) {
                           aRow.mOriginalAmount = "not-a-number";
                           aRow.mOriginalCurrency = "USD";
                           aRow.mFxRate = "0.90";
                       }},
        InvalidRowCase{"MissingExchangeRate",
                       +[](SyntheticCsvRow& aRow) {
                           aRow.mOriginalAmount = "11.00";
                           aRow.mOriginalCurrency = "USD";
                           aRow.mFxRate.clear();
                       }},
        InvalidRowCase{"ZeroExchangeRate",
                       +[](SyntheticCsvRow& aRow) {
                           aRow.mOriginalAmount = "11.00";
                           aRow.mOriginalCurrency = "USD";
                           aRow.mFxRate = "0";
                       }},
        InvalidRowCase{"NegativeExchangeRate",
                       +[](SyntheticCsvRow& aRow) {
                           aRow.mOriginalAmount = "11.00";
                           aRow.mOriginalCurrency = "USD";
                           aRow.mFxRate = "-0.90";
                       }},
        InvalidRowCase{"MalformedExchangeRate",
                       +[](SyntheticCsvRow& aRow) {
                           aRow.mOriginalAmount = "11.00";
                           aRow.mOriginalCurrency = "USD";
                           aRow.mFxRate = "not-a-number";
                       }},
        InvalidRowCase{"OverflowingExchangeRate",
                       +[](SyntheticCsvRow& aRow) {
                           aRow.mOriginalAmount = "11.00";
                           aRow.mOriginalCurrency = "USD";
                           aRow.mFxRate = "92233720368.54775808";
                       }},
    };

    for (const auto& invalidCase : invalidCases)
    {
        SCOPED_TRACE(std::string{GetParam().mName} + '/' + invalidCase.mName);
        SyntheticCsvRow row = GetParam().mMakeRow();
        invalidCase.mMutate(row);

        const ParseResult parseResult = parseRows({row});
        expectNoParsedRecords(parseResult);
    }
}

INSTANTIATE_TEST_SUITE_P(IncomeValidation,
                         InvalidIncomeRowTest,
                         testing::Values(IncomeKindCase{"Dividend", makeDividendRow},
                                         IncomeKindCase{"BrokerInterest", makeBrokerInterestRow},
                                         IncomeKindCase{"BondInterest", makeBondInterestRow}),
                         incomeKindCaseName);

struct InstrumentIncomeKindCase {
    const char* mName;
    SyntheticCsvRow (*mMakeRow)();
};

void PrintTo(const InstrumentIncomeKindCase& aCase, std::ostream* aOutput) {
    *aOutput << aCase.mName;
}

class InvalidIncomeInstrumentTest : public testing::TestWithParam<InstrumentIncomeKindCase> {};

TEST_P(InvalidIncomeInstrumentTest, RequiresBothIsinAndName) {
    const std::array invalidCases{
        InvalidRowCase{"MissingIdentity",
                       +[](SyntheticCsvRow& aRow) {
                           aRow.mName.clear();
                           aRow.mSymbol.clear();
                       }},
        InvalidRowCase{"MissingIsin", +[](SyntheticCsvRow& aRow) { aRow.mSymbol.clear(); }},
        InvalidRowCase{"MissingName", +[](SyntheticCsvRow& aRow) { aRow.mName.clear(); }},
    };

    for (const auto& invalidCase : invalidCases)
    {
        SCOPED_TRACE(std::string{GetParam().mName} + '/' + invalidCase.mName);
        SyntheticCsvRow row = GetParam().mMakeRow();
        invalidCase.mMutate(row);

        const ParseResult parseResult = parseRows({row});
        expectNoParsedRecords(parseResult);
    }
}

INSTANTIATE_TEST_SUITE_P(IncomeInstrumentValidation,
                         InvalidIncomeInstrumentTest,
                         testing::Values(InstrumentIncomeKindCase{"Dividend", makeDividendRow},
                                         InstrumentIncomeKindCase{"BondInterest",
                                                                  makeBondInterestRow}),
                         [](const testing::TestParamInfo<InstrumentIncomeKindCase>& aInfo) {
                             return std::string{aInfo.param.mName};
                         });

TEST(TradeRepublicParserTest, RejectsInvalidCorporateActions) {
    const std::array invalidCases{
        InvalidRowCase{"MissingIdentity",
                       +[](SyntheticCsvRow& aRow) {
                           aRow.mName.clear();
                           aRow.mSymbol.clear();
                       }},
        InvalidRowCase{"MissingIsin", +[](SyntheticCsvRow& aRow) { aRow.mSymbol.clear(); }},
        InvalidRowCase{"MissingName", +[](SyntheticCsvRow& aRow) { aRow.mName.clear(); }},
        InvalidRowCase{"InvalidDate", +[](SyntheticCsvRow& aRow) { aRow.mDate = "2024-13-01"; }},
        InvalidRowCase{"MissingUnits", +[](SyntheticCsvRow& aRow) { aRow.mShares.clear(); }},
        InvalidRowCase{"MalformedUnits",
                       +[](SyntheticCsvRow& aRow) { aRow.mShares = "not-a-number"; }},
        InvalidRowCase{"OverflowingUnits",
                       +[](SyntheticCsvRow& aRow) { aRow.mShares = "92233720368.54775808"; }},
        InvalidRowCase{"ZeroUnits", +[](SyntheticCsvRow& aRow) { aRow.mShares = "0"; }},
        InvalidRowCase{"MissingAssetClass",
                       +[](SyntheticCsvRow& aRow) { aRow.mAssetClass.clear(); }},
        InvalidRowCase{"UnsupportedAssetClass",
                       +[](SyntheticCsvRow& aRow) { aRow.mAssetClass = "COMMODITY"; }},
    };

    for (const auto& invalidCase : invalidCases)
    {
        SCOPED_TRACE(invalidCase.mName);
        SyntheticCsvRow row = makeCorporateActionRow();
        invalidCase.mMutate(row);

        const ParseResult parseResult = parseRows({row});
        expectNoParsedRecords(parseResult);
    }
}

TEST(TradeRepublicParserTest, RejectsACorporateActionAssetConflictForTheSameIsin) {
    SyntheticCsvRow trade;
    SyntheticCsvRow conflictingAction = makeCorporateActionRow();
    conflictingAction.mSymbol = trade.mSymbol;
    conflictingAction.mName = trade.mName;
    conflictingAction.mAssetClass = "FUND";

    const ParseResult parseResult = parseRows({trade, conflictingAction});
    ASSERT_EQ(parseResult.mStatement.mTradeInstruments.size(), 1U);
    const auto& instrument = parseResult.mStatement.mTradeInstruments.front();
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Stock);
    EXPECT_EQ(instrument.mTransactions.size(), 1U);
    EXPECT_TRUE(instrument.mCorporateActions.empty());
}

TEST(TradeRepublicParserTest, RejectsInvalidBenefits) {
    const std::array invalidCases{
        InvalidRowCase{"InvalidDate", +[](SyntheticCsvRow& aRow) { aRow.mDate = "invalid-date"; }},
        InvalidRowCase{"MissingAmount", +[](SyntheticCsvRow& aRow) { aRow.mAmount.clear(); }},
        InvalidRowCase{"MalformedAmount",
                       +[](SyntheticCsvRow& aRow) { aRow.mAmount = "not-a-number"; }},
        InvalidRowCase{"OverflowingAmount",
                       +[](SyntheticCsvRow& aRow) { aRow.mAmount = "922337203685477.5808"; }},
        InvalidRowCase{"MissingCurrency", +[](SyntheticCsvRow& aRow) { aRow.mCurrency.clear(); }},
        InvalidRowCase{"UnsupportedCurrency",
                       +[](SyntheticCsvRow& aRow) { aRow.mCurrency = "CAD"; }},
    };

    for (const auto& invalidCase : invalidCases)
    {
        SCOPED_TRACE(invalidCase.mName);
        SyntheticCsvRow row = makeBenefitRow();
        invalidCase.mMutate(row);

        const ParseResult parseResult = parseRows({row});
        expectNoParsedRecords(parseResult);
    }
}

TEST(TradeRepublicParserTest, RejectsInvalidPrivateMarketEvents) {
    const std::array invalidCases{
        InvalidRowCase{"InvalidDate", +[](SyntheticCsvRow& aRow) { aRow.mDate = "2024-04-31"; }},
        InvalidRowCase{"MissingAmount", +[](SyntheticCsvRow& aRow) { aRow.mAmount.clear(); }},
        InvalidRowCase{"MalformedAmount",
                       +[](SyntheticCsvRow& aRow) { aRow.mAmount = "not-a-number"; }},
        InvalidRowCase{"OverflowingAmount",
                       +[](SyntheticCsvRow& aRow) { aRow.mAmount = "-922337203685477.5809"; }},
        InvalidRowCase{"MalformedFee", +[](SyntheticCsvRow& aRow) { aRow.mFee = "not-a-number"; }},
        InvalidRowCase{"PositiveFee", +[](SyntheticCsvRow& aRow) { aRow.mFee = "0.01"; }},
        InvalidRowCase{"MinimumSignedFee",
                       +[](SyntheticCsvRow& aRow) { aRow.mFee = "-922337203685477.5808"; }},
        InvalidRowCase{"MissingCurrency", +[](SyntheticCsvRow& aRow) { aRow.mCurrency.clear(); }},
        InvalidRowCase{"UnsupportedCurrency",
                       +[](SyntheticCsvRow& aRow) { aRow.mCurrency = "CAD"; }},
    };

    for (const auto& invalidCase : invalidCases)
    {
        SCOPED_TRACE(invalidCase.mName);
        SyntheticCsvRow row = makePrivateMarketRow();
        invalidCase.mMutate(row);

        const ParseResult parseResult = parseRows({row});
        expectNoParsedRecords(parseResult);
    }
}

TEST(TradeRepublicParserTest, IgnoresEveryKnownNonTaxCashTransaction) {
    constexpr std::array ignoredTypes{
        "CARD_FAILED_TRANSACTION",
        "CARD_ORDER_BILLED",
        "CARD_ORDERING_FEE",
        "CARD_TRANSACTION",
        "CARD_TRANSACTION_INTERNATIONAL",
        "CUSTOMER_INBOUND",
        "CUSTOMER_INPAYMENT",
        "CUSTOMER_OUTBOUND_REQUEST",
        "TRANSFER_INSTANT_INBOUND",
        "TRANSFER_INSTANT_OUTBOUND",
        "TRANSFER_OUTBOUND",
        "CREDIT",
    };

    for (const std::string_view ignoredType : ignoredTypes)
    {
        SCOPED_TRACE(ignoredType);
        SyntheticCsvRow row;
        row.mCategory = "CASH";
        row.mType = ignoredType;
        row.mTransactionId = "synthetic-ignored-transaction";

        const ParseResult parseResult = parseRows({row});
        expectNoParsedRecords(parseResult);
        EXPECT_TRUE(parseResult.mWarnings.empty());
    }
}

TEST(TradeRepublicParserTest, ReportsEveryRecognizedButUnsupportedTransaction) {
    struct UnsupportedType {
        std::string_view mType;
        std::string_view mCategory;
    };
    constexpr std::array unsupportedTypes{
        UnsupportedType{"FIXED_INCOME", "CASH"},
        UnsupportedType{"TAX_OPTIMIZATION", "SYNTHETIC_CATEGORY"},
        UnsupportedType{"TAX_REFUND", "SYNTHETIC_CATEGORY"},
        UnsupportedType{"SSP_TAX_CORRECTION_INVOICE", "SYNTHETIC_CATEGORY"},
        UnsupportedType{"SSP_CORPORATE_ACTION_INVOICE_CASH", "SYNTHETIC_CATEGORY"},
        UnsupportedType{"WARRANT_EXERCISE", "SYNTHETIC_CATEGORY"},
        UnsupportedType{"TILG", "SYNTHETIC_CATEGORY"},
        UnsupportedType{"CRYPTO_INVOICE", "SYNTHETIC_CATEGORY"},
    };

    for (const auto& unsupportedType : unsupportedTypes)
    {
        SCOPED_TRACE(unsupportedType.mType);
        SyntheticCsvRow row;
        row.mCategory = unsupportedType.mCategory;
        row.mType = unsupportedType.mType;
        row.mTransactionId = "synthetic-unsupported-transaction";

        const ParseResult parseResult = parseRows({row});
        expectNoParsedRecords(parseResult);
        ASSERT_EQ(parseResult.mWarnings.size(), 1U);
        EXPECT_EQ(parseResult.mWarnings.front().mCode, WarningCode::UnsupportedRowType);
        EXPECT_EQ(parseResult.mWarnings.front().mRowIndex, 2U);
        EXPECT_NE(parseResult.mWarnings.front().mMessage.find(unsupportedType.mType),
                  std::string::npos);
        EXPECT_NE(parseResult.mWarnings.front().mMessage.find(row.mTransactionId),
                  std::string::npos);
    }
}

TEST(TradeRepublicParserTest, ReportsUnknownAndKnownTypesInTheWrongCategory) {
    const std::array unknownCases{
        std::pair{std::string_view{"FUTURE_EVENT"}, std::string_view{"CASH"}},
        std::pair{std::string_view{"BUY"}, std::string_view{"CASH"}},
        std::pair{std::string_view{"buy"}, std::string_view{"TRADING"}},
    };

    for (const auto& [type, category] : unknownCases)
    {
        SCOPED_TRACE(type);
        SyntheticCsvRow row;
        row.mType = type;
        row.mCategory = category;
        row.mTransactionId = "synthetic-unknown-transaction";

        const ParseResult parseResult = parseRows({row});
        expectNoParsedRecords(parseResult);
        ASSERT_EQ(parseResult.mWarnings.size(), 1U);
        EXPECT_EQ(parseResult.mWarnings.front().mCode, WarningCode::UnknownRowType);
        EXPECT_EQ(parseResult.mWarnings.front().mRowIndex, 2U);
        EXPECT_NE(parseResult.mWarnings.front().mMessage.find(type), std::string::npos);
        EXPECT_NE(parseResult.mWarnings.front().mMessage.find(row.mTransactionId),
                  std::string::npos);
    }
}

} // namespace
