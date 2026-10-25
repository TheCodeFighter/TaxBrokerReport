#include "parsers/traderepublic_parser.hpp"
#include "taxbroker/diagnostics.hpp"
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
#include <optional>
#include <sstream>
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

/// CSV text written exactly as given, for records that writeCsvRow cannot produce.
struct RawCsvText {
    std::string_view mText;
};

class TemporaryCsvFile {
  public:
    explicit TemporaryCsvFile(const RawCsvText& aRawText)
        : mPath{std::filesystem::temp_directory_path() /
                ("taxbroker_" + sanitizedTestName() + ".csv")} {
        std::ofstream output{mPath, std::ios::binary | std::ios::trunc};
        if (!output)
        {
            throw std::runtime_error{"Unable to create synthetic CSV fixture"};
        }

        output << aRawText.mText;
        if (!output)
        {
            throw std::runtime_error{"Unable to write synthetic CSV fixture"};
        }
    }

    explicit TemporaryCsvFile(std::initializer_list<SyntheticCsvRow> aRows,
                              std::string_view aHeader = kCsvHeader)
        : mPath{std::filesystem::temp_directory_path() /
                ("taxbroker_" + sanitizedTestName() + ".csv")} {
        std::ofstream output{mPath, std::ios::trunc};
        if (!output)
        {
            throw std::runtime_error{"Unable to create synthetic CSV fixture"};
        }

        output << aHeader << '\n';
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

ParseResult parseRawText(std::string_view aText) {
    const TemporaryCsvFile csvFile{RawCsvText{aText}};
    tr::TradeRepublicParser parser;
    return parser.parse(csvFile.path());
}

/// One CSV record as text, without the final newline.
std::string csvLine(const SyntheticCsvRow& aRow) {
    std::ostringstream output;
    writeCsvRow(output, aRow);
    std::string line = output.str();
    line.pop_back();

    return line;
}

Date makeDate(int aYear, unsigned aMonth, unsigned aDay) {
    const auto calendarDate =
        std::chrono::year{aYear} / std::chrono::month{aMonth} / std::chrono::day{aDay};
    return Date{std::chrono::sys_days{calendarDate}.time_since_epoch()};
}

std::string_view transactionId(const EventMetadata& aMetadata) {
    const auto& source = primarySource(aMetadata);
    return source.mTransactionId ? *source.mTransactionId : std::string_view{};
}

void expectSourceMetadata(const EventMetadata& aMetadata,
                          const std::filesystem::path& aSourcePath,
                          std::size_t aSourceRow,
                          std::string_view aTransactionId,
                          std::size_t aSourceIndex,
                          std::size_t aEventIndex) {
    const auto expectedTimestamp =
        SourceTimestamp{std::chrono::sys_days{std::chrono::year{2024} / 1 / 14}.time_since_epoch() +
                        std::chrono::hours{23} + std::chrono::milliseconds{123}};

    EXPECT_EQ(aMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(aMetadata.mSourceTimestamp, expectedTimestamp);
    ASSERT_EQ(aMetadata.mSources.size(), 1U);
    const auto& source = primarySource(aMetadata);
    EXPECT_EQ(source.mBroker, Broker::TradeRepublic);
    EXPECT_EQ(source.mFilename.value(), aSourcePath.filename().string());
    EXPECT_EQ(source.mSourceRow, aSourceRow);
    EXPECT_EQ(transactionId(aMetadata), aTransactionId);
    EXPECT_EQ(source.mInputSequence.mSourceIndex, aSourceIndex);
    EXPECT_EQ(source.mInputSequence.mEventIndex, aEventIndex);
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
        const auto transaction =
            std::find_if(instrument.mTransactions.begin(),
                         instrument.mTransactions.end(),
                         [&](const TradeTransaction& aTrade) {
                             return transactionId(aTrade.mMetadata) == aTransactionId;
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
        const auto transaction =
            std::find_if(instrument.mTransactions.begin(),
                         instrument.mTransactions.end(),
                         [&](const DividendTransaction& aDividend) {
                             return transactionId(aDividend.mMetadata) == aTransactionId;
                         });
        if (transaction != instrument.mTransactions.end())
        {
            return &*transaction;
        }
    }
    return nullptr;
}

const InterestTransaction* findInterestTransaction(const BrokerStatement& aStatement,
                                                   std::string_view aTransactionId) {
    for (const auto& instrument : aStatement.mInterestInstruments)
    {
        const auto transaction =
            std::find_if(instrument.mTransactions.begin(),
                         instrument.mTransactions.end(),
                         [&](const InterestTransaction& aInterest) {
                             return transactionId(aInterest.mMetadata) == aTransactionId;
                         });
        if (transaction != instrument.mTransactions.end())
        {
            return &*transaction;
        }
    }
    return nullptr;
}

const CorporateAction* findCorporateAction(const BrokerStatement& aStatement,
                                           std::string_view aTransactionId) {
    for (const auto& instrument : aStatement.mTradeInstruments)
    {
        const auto action =
            std::find_if(instrument.mCorporateActions.begin(),
                         instrument.mCorporateActions.end(),
                         [&](const CorporateAction& aCorporateAction) {
                             return transactionId(aCorporateAction.mMetadata) == aTransactionId;
                         });
        if (action != instrument.mCorporateActions.end())
        {
            return &*action;
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
    const auto event =
        std::find_if(aStatement.mPrivateMarketEvents.begin(),
                     aStatement.mPrivateMarketEvents.end(),
                     [&](const PrivateMarketEvent& aPrivateEvent) {
                         return transactionId(aPrivateEvent.mMetadata) == aTransactionId;
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
    const auto benefit = std::find_if(aStatement.mBenefitEvents.begin(),
                                      aStatement.mBenefitEvents.end(),
                                      [&](const BenefitEvent& aEvent) {
                                          return transactionId(aEvent.mMetadata) == aTransactionId;
                                      });

    return benefit == aStatement.mBenefitEvents.end() ? nullptr : &*benefit;
}

std::size_t countDiagnostics(const ParseResult& aParseResult, DiagnosticCode aDiagnosticCode) {
    return static_cast<std::size_t>(std::count_if(
        aParseResult.mDiagnostics.begin(),
        aParseResult.mDiagnostics.end(),
        [&](const ParseDiagnostic& aDiagnostic) { return aDiagnostic.mCode == aDiagnosticCode; }));
}

bool hasDiagnosticContaining(const ParseResult& aParseResult, std::string_view aText) {
    return std::any_of(aParseResult.mDiagnostics.begin(),
                       aParseResult.mDiagnostics.end(),
                       [&](const ParseDiagnostic& aDiagnostic) {
                           return aDiagnostic.mMessage.find(aText) != std::string::npos;
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
    row.mType = "INTEREST_PAYMENT";
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

    const auto* stockPerkInstrument = findTradeInstrument(parseResult.mStatement, "XX1000000001");
    ASSERT_NE(stockPerkInstrument, nullptr);
    ASSERT_EQ(stockPerkInstrument->mTransactions.size(), 1U);
    EXPECT_EQ(stockPerkInstrument->mTransactions.front().mUnits, 10'000'000);
    ASSERT_TRUE(stockPerkInstrument->mTransactions.front().mAmount.has_value());
    EXPECT_EQ(*stockPerkInstrument->mTransactions.front().mAmount, 123'400);
    EXPECT_EQ(stockPerkInstrument->mTransactions.front().mFeePaid, 0);
    EXPECT_EQ(transactionId(stockPerkInstrument->mTransactions.front().mMetadata),
              "synthetic-stock-perk-buy");

    const auto* savebackInstrument = findTradeInstrument(parseResult.mStatement, "XX1000000002");
    ASSERT_NE(savebackInstrument, nullptr);
    ASSERT_EQ(savebackInstrument->mTransactions.size(), 1U);
    EXPECT_EQ(savebackInstrument->mTransactions.front().mUnits, 25'000'000);
    ASSERT_TRUE(savebackInstrument->mTransactions.front().mAmount.has_value());
    EXPECT_EQ(*savebackInstrument->mTransactions.front().mAmount, 25'000);
    EXPECT_EQ(transactionId(savebackInstrument->mTransactions.front().mMetadata),
              "synthetic-saveback-buy");

    ASSERT_EQ(parseResult.mStatement.mBenefitEvents.size(), 3U);

    const auto* stockPerk = findBenefitEvent(parseResult.mStatement, BenefitType::StockPerk);
    ASSERT_NE(stockPerk, nullptr);
    EXPECT_EQ(stockPerk->mAmount, 123'400);
    EXPECT_EQ(stockPerk->mCurrency, Currency::EUR);
    EXPECT_EQ(stockPerk->mAssetClass, AssetClass::Stock);
    EXPECT_EQ(stockPerk->mName, "Synthetic Reward Share");
    ASSERT_TRUE(stockPerk->mIsin.has_value());
    EXPECT_EQ(*stockPerk->mIsin, "XX1000000001");
    EXPECT_EQ(transactionId(stockPerk->mMetadata), "synthetic-stock-perk-cash");

    const auto* saveback = findBenefitEvent(parseResult.mStatement, BenefitType::Saveback);
    ASSERT_NE(saveback, nullptr);
    EXPECT_EQ(saveback->mAmount, 25'000);
    EXPECT_EQ(saveback->mCurrency, Currency::EUR);
    EXPECT_EQ(saveback->mAssetClass, AssetClass::Fund);
    EXPECT_EQ(saveback->mName, "Synthetic Reward Fund");
    ASSERT_TRUE(saveback->mIsin.has_value());
    EXPECT_EQ(*saveback->mIsin, "XX1000000002");
    EXPECT_EQ(transactionId(saveback->mMetadata), "synthetic-saveback-cash");

    const auto* unassignedSaveback =
        findBenefitEvent(parseResult.mStatement, "synthetic-saveback-unassigned");
    ASSERT_NE(unassignedSaveback, nullptr);
    EXPECT_EQ(unassignedSaveback->mType, BenefitType::Saveback);
    EXPECT_EQ(unassignedSaveback->mAmount, 11'100);
    EXPECT_TRUE(unassignedSaveback->mName.empty());
    EXPECT_FALSE(unassignedSaveback->mIsin.has_value());
    EXPECT_EQ(unassignedSaveback->mAssetClass, AssetClass::Unknown);

    EXPECT_EQ(countDiagnostics(parseResult, DiagnosticCode::UnsupportedRowType), 0U);
    EXPECT_FALSE(hasDiagnosticContaining(parseResult, "STOCKPERK"));
    EXPECT_FALSE(hasDiagnosticContaining(parseResult, "BENEFITS_SAVEBACK"));
}

TEST(TradeRepublicParserTest, PreservesPrivateMarketEventsAndFundExecution) {
    const ParseResult parseResult = parseFixture();

    const auto* privateFund = findTradeInstrument(parseResult.mStatement, "XX1000000003");
    ASSERT_NE(privateFund, nullptr);
    EXPECT_EQ(privateFund->mAssetClass, AssetClass::PrivateFund);
    ASSERT_EQ(privateFund->mTransactions.size(), 1U);
    EXPECT_FALSE(privateFund->mTransactions.front().mAmount.has_value());
    EXPECT_EQ(transactionId(privateFund->mTransactions.front().mMetadata), "synthetic-private-buy");

    ASSERT_EQ(parseResult.mStatement.mPrivateMarketEvents.size(), 1U);
    const auto& prepayment = parseResult.mStatement.mPrivateMarketEvents.front();
    EXPECT_EQ(prepayment.mType, PrivateMarketEventType::Buy);
    EXPECT_EQ(prepayment.mAmount, -2'500'000);
    EXPECT_EQ(prepayment.mFeePaid, 7'500);
    EXPECT_EQ(transactionId(prepayment.mMetadata), "synthetic-private-prepayment");

    EXPECT_EQ(countDiagnostics(parseResult, DiagnosticCode::UnsupportedAssetClass), 1U);

    const auto warning =
        std::find_if(parseResult.mDiagnostics.begin(),
                     parseResult.mDiagnostics.end(),
                     [](const ParseDiagnostic& aDiagnostic) {
                         return aDiagnostic.mCode == DiagnosticCode::UnsupportedAssetClass;
                     });

    ASSERT_NE(warning, parseResult.mDiagnostics.end());
    EXPECT_EQ(warning->mMessageKey, MessageKey::UnsupportedAssetClassTaxUnsupported);
    EXPECT_EQ(warning->mMessageParams,
              (std::vector<MessageParam>{textParam("assetClass", "PRIVATE_FUND")}));
}

TEST(TradeRepublicParserTest, PreservesUnresolvedSplitWithoutCreatingATrade) {
    const ParseResult parseResult = parseFixture();

    const auto* splitInstrument = findTradeInstrument(parseResult.mStatement, "XX1000000004");
    ASSERT_NE(splitInstrument, nullptr);
    EXPECT_TRUE(splitInstrument->mTransactions.empty());
    ASSERT_EQ(splitInstrument->mCorporateActions.size(), 1U);
    EXPECT_EQ(splitInstrument->mCorporateActions.front().mType,
              CorporateActionType::UnresolvedSplit);
    EXPECT_EQ(splitInstrument->mCorporateActions.front().mUnitsDelta, 75'000'000);
    EXPECT_FALSE(splitInstrument->mCorporateActions.front().mRatio.has_value());
    EXPECT_EQ(transactionId(splitInstrument->mCorporateActions.front().mMetadata),
              "synthetic-split-action");
}

TEST(TradeRepublicParserTest, ReportsUnknownRowsWithSourceLocation) {
    const ParseResult parseResult = parseFixture();

    EXPECT_EQ(countDiagnostics(parseResult, DiagnosticCode::UnsupportedRowType), 0U);
    EXPECT_EQ(countDiagnostics(parseResult, DiagnosticCode::UnsupportedAssetClass), 1U);
    EXPECT_EQ(countDiagnostics(parseResult, DiagnosticCode::UnknownRowType), 1U);
    ASSERT_EQ(parseResult.mDiagnostics.size(), 2U);
    EXPECT_TRUE(hasDiagnosticContaining(parseResult, "FUTURE_EVENT"));
    EXPECT_EQ(parseResult.mDiagnostics.back().mRowIndex, 11U);
    EXPECT_EQ(parseResult.mDiagnostics.back().mSourceFile, fixturePath().filename().string());
}

TEST(TradeRepublicParserTest, ParsesAndNormalizesEverySupportedTradeExecution) {
    const ParseResult parseResult = parseSupportedFixture();
    const auto& statement = parseResult.mStatement;

    EXPECT_EQ(tradeTransactionCount(statement), 6U);

    const auto* buy = findTradeTransaction(statement, "synthetic-buy-001");
    ASSERT_NE(buy, nullptr);
    EXPECT_EQ(buy->mMetadata.mTaxDate, makeDate(2024, 2, 29));
    EXPECT_EQ(buy->mTradeSide, TradeSide::Buy);
    EXPECT_EQ(buy->mUnitPrice, 1'234'565'000); // Source 12.345650 at eight decimal places.
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
    for (const auto& [expectedTransactionId, expectedAssetClass] : expectedExecutions)
    {
        const auto* transaction = findTradeTransaction(statement, expectedTransactionId);
        ASSERT_NE(transaction, nullptr) << expectedTransactionId;
        EXPECT_EQ(transaction->mTradeSide, TradeSide::Buy) << expectedTransactionId;

        const auto instrument = std::find_if(
            statement.mTradeInstruments.begin(),
            statement.mTradeInstruments.end(),
            [&](const TradeInstrument& aInstrument) {
                return std::any_of(aInstrument.mTransactions.begin(),
                                   aInstrument.mTransactions.end(),
                                   [&](const TradeTransaction& aTransaction) {
                                       return ::transactionId(aTransaction.mMetadata) ==
                                              expectedTransactionId;
                                   });
            });
        ASSERT_NE(instrument, statement.mTradeInstruments.end());
        EXPECT_EQ(instrument->mAssetClass, expectedAssetClass) << expectedTransactionId;
    }

    EXPECT_EQ(countDiagnostics(parseResult, DiagnosticCode::UnsupportedAssetClass), 1U);
    const auto diagnostic =
        std::find_if(parseResult.mDiagnostics.begin(),
                     parseResult.mDiagnostics.end(),
                     [](const ParseDiagnostic& aDiagnostic) {
                         return aDiagnostic.mCode == DiagnosticCode::UnsupportedAssetClass;
                     });
    ASSERT_NE(diagnostic, parseResult.mDiagnostics.end());
    EXPECT_EQ(diagnostic->mSeverity, DiagnosticSeverity::Warning);
    ASSERT_TRUE(diagnostic->mTransactionId.has_value());
    EXPECT_EQ(*diagnostic->mTransactionId, "synthetic-crypto-001");
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
        Money mGrossAmount{};
        std::optional<Money> mTaxPaid;
        ExchangeRate mExchangeRate{};
        Currency mCurrency{};
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
        ExpectedForeignDividend{"synthetic-dividend-jpy-001",
                                1'000'000,
                                std::nullopt,
                                610'000,
                                Currency::JPY},
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
    EXPECT_FALSE(brokerInterest->mTransactions[0].mTaxPaid.has_value());
    EXPECT_EQ(transactionId(brokerInterest->mTransactions[0].mMetadata), "synthetic-interest-001");
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
    EXPECT_EQ(transactionId(transaction.mMetadata), "synthetic-bond-interest-001");
}

TEST(TradeRepublicParserTest, PreservesBothQuantitySignsWithoutInferringSplitDirection) {
    const ParseResult parseResult = parseSupportedFixture();
    const auto* instrument = findTradeInstrument(parseResult.mStatement, "XX0000000001");

    ASSERT_NE(instrument, nullptr);
    ASSERT_EQ(instrument->mCorporateActions.size(), 2U);

    const auto& positiveQuantityAction = instrument->mCorporateActions[0];
    EXPECT_EQ(positiveQuantityAction.mType, CorporateActionType::UnresolvedSplit);
    EXPECT_EQ(positiveQuantityAction.mUnitsDelta, 200'000'000);
    EXPECT_FALSE(positiveQuantityAction.mRatio.has_value());
    EXPECT_EQ(transactionId(positiveQuantityAction.mMetadata), "synthetic-split-001");
    ASSERT_EQ(positiveQuantityAction.mUnitEvidence.size(), 1U);

    const auto& positiveEvidence = positiveQuantityAction.mUnitEvidence.front();

    EXPECT_EQ(positiveEvidence.mSourceText, "2.0000000000");
    EXPECT_EQ(positiveEvidence.mCanonicalValue, "2");
    EXPECT_EQ(positiveEvidence.mSource.mSourceRow, 17U);
    EXPECT_EQ(positiveEvidence.mSource, positiveQuantityAction.mMetadata.mSources.front());

    const auto& negativeQuantityAction = instrument->mCorporateActions[1];
    EXPECT_EQ(negativeQuantityAction.mType, CorporateActionType::UnresolvedSplit);
    EXPECT_EQ(negativeQuantityAction.mUnitsDelta, -50'000'000);
    EXPECT_FALSE(negativeQuantityAction.mRatio.has_value());
    EXPECT_EQ(transactionId(negativeQuantityAction.mMetadata), "synthetic-negative-split-001");
    ASSERT_EQ(negativeQuantityAction.mUnitEvidence.size(), 1U);

    const auto& negativeEvidence = negativeQuantityAction.mUnitEvidence.front();

    EXPECT_EQ(negativeEvidence.mSourceText, "-0.5000000000");
    EXPECT_EQ(negativeEvidence.mCanonicalValue, "-0.5");
    EXPECT_EQ(negativeEvidence.mSource.mSourceRow, 18U);
    EXPECT_EQ(negativeEvidence.mSource, negativeQuantityAction.mMetadata.mSources.front());
}

TEST(TradeRepublicParserTest, PreservesPositiveHalfStepSplitEvidenceWithoutEconomics) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "0.12345678500",
        .mPrice = "",
        .mAmount = "",
        .mCurrency = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-validation",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mTradeInstruments.front();

    EXPECT_TRUE(instrument.mTransactions.empty());
    ASSERT_EQ(instrument.mCorporateActions.size(), 1U);

    const auto& action = instrument.mCorporateActions.front();

    EXPECT_EQ(action.mType, CorporateActionType::UnresolvedSplit);
    // 0.12345678500 rounds to 0.12345679; 0.12345679 * 100,000,000 = 12,345,679.
    EXPECT_EQ(action.mUnitsDelta, 12'345'679);
    EXPECT_FALSE(action.mRatio.has_value());
    ASSERT_EQ(action.mUnitEvidence.size(), 1U);

    const auto& evidence = action.mUnitEvidence.front();

    EXPECT_EQ(evidence.mSourceText, "0.12345678500");
    EXPECT_EQ(evidence.mCanonicalValue, "0.123456785");
    EXPECT_EQ(evidence.mDiscardedDigits, "500");
    EXPECT_TRUE(evidence.mRoundedAwayFromZero);
    EXPECT_EQ(evidence.mSource.mBroker, Broker::TradeRepublic);
    EXPECT_EQ(evidence.mSource.mSourceRow, 2U);
    EXPECT_EQ(evidence.mSource.mTransactionId, "synthetic-split-validation");
    EXPECT_EQ(evidence.mSource.mInputSequence, (StableInputSequence{0, 0}));
    EXPECT_EQ(evidence.mSource, action.mMetadata.mSources.front());
}

TEST(TradeRepublicParserTest, PreservesPositiveSplitEvidenceWhenRoundingTowardZero) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "0.12345678400",
        .mPrice = "",
        .mAmount = "",
        .mCurrency = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-validation",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mTradeInstruments.front();

    EXPECT_TRUE(instrument.mTransactions.empty());
    ASSERT_EQ(instrument.mCorporateActions.size(), 1U);

    const auto& action = instrument.mCorporateActions.front();

    EXPECT_EQ(action.mType, CorporateActionType::UnresolvedSplit);
    // 0.12345678400 rounds to 0.12345678; 0.12345678 * 100,000,000 = 12,345,678.
    EXPECT_EQ(action.mUnitsDelta, 12'345'678);
    EXPECT_FALSE(action.mRatio.has_value());
    ASSERT_EQ(action.mUnitEvidence.size(), 1U);

    const auto& evidence = action.mUnitEvidence.front();

    EXPECT_EQ(evidence.mSourceText, "0.12345678400");
    EXPECT_EQ(evidence.mCanonicalValue, "0.123456784");
    EXPECT_EQ(evidence.mDiscardedDigits, "400");
    EXPECT_FALSE(evidence.mRoundedAwayFromZero);
    EXPECT_EQ(evidence.mSource.mBroker, Broker::TradeRepublic);
    EXPECT_EQ(evidence.mSource.mSourceRow, 2U);
    EXPECT_EQ(evidence.mSource.mTransactionId, "synthetic-split-validation");
    EXPECT_EQ(evidence.mSource.mInputSequence, (StableInputSequence{0, 0}));
    EXPECT_EQ(evidence.mSource, action.mMetadata.mSources.front());
}

TEST(TradeRepublicParserTest, PreservesPositiveSplitEvidenceWhenRoundingAboveHalfStep) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "0.12345678600",
        .mPrice = "",
        .mAmount = "",
        .mCurrency = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-validation",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mTradeInstruments.front();

    EXPECT_TRUE(instrument.mTransactions.empty());
    ASSERT_EQ(instrument.mCorporateActions.size(), 1U);

    const auto& action = instrument.mCorporateActions.front();

    EXPECT_EQ(action.mType, CorporateActionType::UnresolvedSplit);
    // 0.12345678600 rounds to 0.12345679; 0.12345679 * 100,000,000 = 12,345,679.
    EXPECT_EQ(action.mUnitsDelta, 12'345'679);
    EXPECT_FALSE(action.mRatio.has_value());
    ASSERT_EQ(action.mUnitEvidence.size(), 1U);

    const auto& evidence = action.mUnitEvidence.front();

    EXPECT_EQ(evidence.mSourceText, "0.12345678600");
    EXPECT_EQ(evidence.mCanonicalValue, "0.123456786");
    EXPECT_EQ(evidence.mDiscardedDigits, "600");
    EXPECT_TRUE(evidence.mRoundedAwayFromZero);
    EXPECT_EQ(evidence.mSource.mBroker, Broker::TradeRepublic);
    EXPECT_EQ(evidence.mSource.mSourceRow, 2U);
    EXPECT_EQ(evidence.mSource.mTransactionId, "synthetic-split-validation");
    EXPECT_EQ(evidence.mSource.mInputSequence, (StableInputSequence{0, 0}));
    EXPECT_EQ(evidence.mSource, action.mMetadata.mSources.front());
}

TEST(TradeRepublicParserTest, PreservesNegativeSplitEvidenceWhenRoundingAboveHalfStep) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "-0.12345678600",
        .mPrice = "",
        .mAmount = "",
        .mCurrency = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-validation",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mTradeInstruments.front();

    EXPECT_TRUE(instrument.mTransactions.empty());
    ASSERT_EQ(instrument.mCorporateActions.size(), 1U);

    const auto& action = instrument.mCorporateActions.front();

    EXPECT_EQ(action.mType, CorporateActionType::UnresolvedSplit);
    // -0.12345678600 rounds to -0.12345679; -0.12345679 * 100,000,000 = -12,345,679.
    EXPECT_EQ(action.mUnitsDelta, -12'345'679);
    EXPECT_FALSE(action.mRatio.has_value());
    ASSERT_EQ(action.mUnitEvidence.size(), 1U);

    const auto& evidence = action.mUnitEvidence.front();

    EXPECT_EQ(evidence.mSourceText, "-0.12345678600");
    EXPECT_EQ(evidence.mCanonicalValue, "-0.123456786");
    EXPECT_EQ(evidence.mDiscardedDigits, "600");
    EXPECT_TRUE(evidence.mRoundedAwayFromZero);
    EXPECT_EQ(evidence.mSource.mBroker, Broker::TradeRepublic);
    EXPECT_EQ(evidence.mSource.mSourceRow, 2U);
    EXPECT_EQ(evidence.mSource.mTransactionId, "synthetic-split-validation");
    EXPECT_EQ(evidence.mSource.mInputSequence, (StableInputSequence{0, 0}));
    EXPECT_EQ(evidence.mSource, action.mMetadata.mSources.front());
}

TEST(TradeRepublicParserTest, PreservesNegativeHalfStepSplitEvidenceWithoutEconomics) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "-0.12345678500",
        .mPrice = "",
        .mAmount = "",
        .mCurrency = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-validation",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mTradeInstruments.front();

    EXPECT_TRUE(instrument.mTransactions.empty());
    ASSERT_EQ(instrument.mCorporateActions.size(), 1U);

    const auto& action = instrument.mCorporateActions.front();

    EXPECT_EQ(action.mType, CorporateActionType::UnresolvedSplit);
    // -0.12345678500 rounds to -0.12345679; -0.12345679 * 100,000,000 = -12,345,679.
    EXPECT_EQ(action.mUnitsDelta, -12'345'679);
    EXPECT_FALSE(action.mRatio.has_value());
    ASSERT_EQ(action.mUnitEvidence.size(), 1U);

    const auto& evidence = action.mUnitEvidence.front();

    EXPECT_EQ(evidence.mSourceText, "-0.12345678500");
    EXPECT_EQ(evidence.mCanonicalValue, "-0.123456785");
    EXPECT_EQ(evidence.mDiscardedDigits, "500");
    EXPECT_TRUE(evidence.mRoundedAwayFromZero);
    EXPECT_EQ(evidence.mSource.mBroker, Broker::TradeRepublic);
    EXPECT_EQ(evidence.mSource.mSourceRow, 2U);
    EXPECT_EQ(evidence.mSource.mTransactionId, "synthetic-split-validation");
    EXPECT_EQ(evidence.mSource.mInputSequence, (StableInputSequence{0, 0}));
    EXPECT_EQ(evidence.mSource, action.mMetadata.mSources.front());
}

TEST(TradeRepublicParserTest, PreservesNegativeSplitEvidenceWhenRoundingTowardZero) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "-0.12345678400",
        .mPrice = "",
        .mAmount = "",
        .mCurrency = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-validation",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mTradeInstruments.front();

    EXPECT_TRUE(instrument.mTransactions.empty());
    ASSERT_EQ(instrument.mCorporateActions.size(), 1U);

    const auto& action = instrument.mCorporateActions.front();

    EXPECT_EQ(action.mType, CorporateActionType::UnresolvedSplit);
    // -0.12345678400 rounds to -0.12345678; -0.12345678 * 100,000,000 = -12,345,678.
    EXPECT_EQ(action.mUnitsDelta, -12'345'678);
    EXPECT_FALSE(action.mRatio.has_value());
    ASSERT_EQ(action.mUnitEvidence.size(), 1U);

    const auto& evidence = action.mUnitEvidence.front();

    EXPECT_EQ(evidence.mSourceText, "-0.12345678400");
    EXPECT_EQ(evidence.mCanonicalValue, "-0.123456784");
    EXPECT_EQ(evidence.mDiscardedDigits, "400");
    EXPECT_FALSE(evidence.mRoundedAwayFromZero);
    EXPECT_EQ(evidence.mSource.mBroker, Broker::TradeRepublic);
    EXPECT_EQ(evidence.mSource.mSourceRow, 2U);
    EXPECT_EQ(evidence.mSource.mTransactionId, "synthetic-split-validation");
    EXPECT_EQ(evidence.mSource.mInputSequence, (StableInputSequence{0, 0}));
    EXPECT_EQ(evidence.mSource, action.mMetadata.mSources.front());
}

TEST(TradeRepublicParserTest, PreservesSmallestPositiveRoundedSplitQuantity) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "0.000000005",
        .mPrice = "",
        .mAmount = "",
        .mCurrency = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-validation",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mTradeInstruments.front();

    EXPECT_TRUE(instrument.mTransactions.empty());
    ASSERT_EQ(instrument.mCorporateActions.size(), 1U);

    const auto& action = instrument.mCorporateActions.front();

    EXPECT_EQ(action.mType, CorporateActionType::UnresolvedSplit);
    // 0.000000005 rounds away from zero to 0.00000001 shares, stored as 1.
    EXPECT_EQ(action.mUnitsDelta, 1);
    EXPECT_FALSE(action.mRatio.has_value());
    ASSERT_EQ(action.mUnitEvidence.size(), 1U);

    const auto& evidence = action.mUnitEvidence.front();

    EXPECT_EQ(evidence.mSourceText, "0.000000005");
    EXPECT_EQ(evidence.mCanonicalValue, "0.000000005");
    EXPECT_EQ(evidence.mDiscardedDigits, "5");
    EXPECT_TRUE(evidence.mRoundedAwayFromZero);
    EXPECT_EQ(evidence.mSource.mBroker, Broker::TradeRepublic);
    EXPECT_EQ(evidence.mSource.mSourceRow, 2U);
    EXPECT_EQ(evidence.mSource.mTransactionId, "synthetic-split-validation");
    EXPECT_EQ(evidence.mSource.mInputSequence, (StableInputSequence{0, 0}));
    EXPECT_EQ(evidence.mSource, action.mMetadata.mSources.front());
}

TEST(TradeRepublicParserTest, PreservesSmallestNegativeRoundedSplitQuantity) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "-0.000000005",
        .mPrice = "",
        .mAmount = "",
        .mCurrency = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-validation",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mTradeInstruments.front();

    EXPECT_TRUE(instrument.mTransactions.empty());
    ASSERT_EQ(instrument.mCorporateActions.size(), 1U);

    const auto& action = instrument.mCorporateActions.front();

    EXPECT_EQ(action.mType, CorporateActionType::UnresolvedSplit);
    // -0.000000005 rounds away from zero to -0.00000001 shares, stored as -1.
    EXPECT_EQ(action.mUnitsDelta, -1);
    EXPECT_FALSE(action.mRatio.has_value());
    ASSERT_EQ(action.mUnitEvidence.size(), 1U);

    const auto& evidence = action.mUnitEvidence.front();

    EXPECT_EQ(evidence.mSourceText, "-0.000000005");
    EXPECT_EQ(evidence.mCanonicalValue, "-0.000000005");
    EXPECT_EQ(evidence.mDiscardedDigits, "5");
    EXPECT_TRUE(evidence.mRoundedAwayFromZero);
    EXPECT_EQ(evidence.mSource.mBroker, Broker::TradeRepublic);
    EXPECT_EQ(evidence.mSource.mSourceRow, 2U);
    EXPECT_EQ(evidence.mSource.mTransactionId, "synthetic-split-validation");
    EXPECT_EQ(evidence.mSource.mInputSequence, (StableInputSequence{0, 0}));
    EXPECT_EQ(evidence.mSource, action.mMetadata.mSources.front());
}

TEST(TradeRepublicParserTest, RejectsPositiveSplitQuantityBelowHalfStep) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "0.000000004",
        .mPrice = "",
        .mAmount = "",
        .mCurrency = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-validation",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(diagnostic.mField, "shares");
    EXPECT_EQ(diagnostic.mRowIndex, 2U);
    EXPECT_EQ(diagnostic.mTransactionId, "synthetic-split-validation");
}

TEST(TradeRepublicParserTest, RejectsNegativeSplitQuantityBelowHalfStep) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "-0.000000004",
        .mPrice = "",
        .mAmount = "",
        .mCurrency = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-validation",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(diagnostic.mField, "shares");
    EXPECT_EQ(diagnostic.mRowIndex, 2U);
    EXPECT_EQ(diagnostic.mTransactionId, "synthetic-split-validation");
}

TEST(TradeRepublicParserTest, RejectsNegativeZeroSplitQuantity) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "-0.000000000",
        .mPrice = "",
        .mAmount = "",
        .mCurrency = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-validation",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(diagnostic.mField, "shares");
    EXPECT_EQ(diagnostic.mRowIndex, 2U);
    EXPECT_EQ(diagnostic.mTransactionId, "synthetic-split-validation");
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

    const auto* stockPerk = findBenefitEvent(statement, "synthetic-stock-perk-001");
    ASSERT_NE(stockPerk, nullptr);
    EXPECT_EQ(stockPerk->mType, BenefitType::StockPerk);
    EXPECT_EQ(stockPerk->mAmount, 75'000);
    ASSERT_TRUE(stockPerk->mIsin.has_value());
    EXPECT_EQ(*stockPerk->mIsin, "XX0000000009");
    EXPECT_EQ(stockPerk->mAssetClass, AssetClass::Stock);

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

    EXPECT_EQ(countDiagnostics(parseResult, DiagnosticCode::UnknownRowType), 0U);
    EXPECT_EQ(countDiagnostics(parseResult, DiagnosticCode::UnsupportedRowType), 0U);
}

TEST(TradeRepublicParserTest, HeaderOnlyCsvProducesAnEmptyResult) {
    const ParseResult parseResult = parseRows({});

    EXPECT_TRUE(parseResult.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(parseResult.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(parseResult.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(parseResult.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(parseResult.mStatement.mPrivateMarketEvents.empty());
    EXPECT_TRUE(parseResult.mDiagnostics.empty());
}

TEST(TradeRepublicParserTest, ReturnsAStructuredErrorWhenTheCsvCannotBeOpened) {
    const auto missingPath = std::filesystem::temp_directory_path() /
                             "taxbroker_nonexistent_parser_input" / "missing.csv";
    ASSERT_FALSE(std::filesystem::exists(missingPath));

    tr::TradeRepublicParser parser;
    const ParseResult parseResult = parser.parse(missingPath);

    EXPECT_EQ(parseResult.mBroker, Broker::TradeRepublic);
    expectNoParsedRecords(parseResult);
    ASSERT_EQ(parseResult.mDiagnostics.size(), 1U);

    const auto& diagnostic = parseResult.mDiagnostics.front();
    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::ParseError);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::ParseErrorUnreadableFile);
    EXPECT_TRUE(diagnostic.mMessageParams.empty());
    EXPECT_EQ(diagnostic.mSourceFile, "missing.csv");
    EXPECT_FALSE(diagnostic.mRowIndex.has_value());
    EXPECT_FALSE(diagnostic.mTransactionId.has_value());
    EXPECT_FALSE(diagnostic.mField.has_value());
    EXPECT_EQ(diagnostic.mMessage.find(missingPath.parent_path().string()), std::string::npos);
}

TEST(TradeRepublicParserTest, RejectsAHeaderOnlyFileSeparatedBySemicolons) {
    // The 23 Trade Republic column names, separated by semicolons: one column for the reader.
    const ParseResult parseResult = parseRawText(
        R"("datetime";"date";"account_type";"category";"type";"asset_class";"name";"symbol";)"
        R"("shares";"price";"amount";"fee";"tax";"currency";"original_amount";)"
        R"("original_currency";"fx_rate";"description";"transaction_id";"counterparty_name";)"
        R"("counterparty_iban";"payment_reference";"mcc_code")"
        "\n");

    expectNoParsedRecords(parseResult);
    ASSERT_EQ(parseResult.mDiagnostics.size(), 1U);

    const auto& diagnostic = parseResult.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::InvalidHeaderOtherSeparator);
    EXPECT_TRUE(diagnostic.mMessageParams.empty());
    EXPECT_EQ(diagnostic.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsAHeaderOnlyFileSeparatedBySemicolons.csv");
    EXPECT_FALSE(diagnostic.mRowIndex.has_value());
    EXPECT_FALSE(diagnostic.mField.has_value());
    EXPECT_FALSE(diagnostic.mTransactionId.has_value());
    EXPECT_NE(diagnostic.mMessage.find("separated by semicolons or tabs"), std::string::npos);
}

TEST(TradeRepublicParserTest, RejectsASemicolonFileWithDecimalCommasOnce) {
    // A spreadsheet program wrote semicolons and decimal commas. Without the header check, every
    // row would be a field count error.
    const ParseResult parseResult = parseRawText(
        R"("datetime";"date";"account_type";"category";"type";"asset_class";"name";"symbol";)"
        R"("shares";"price";"amount";"fee";"tax";"currency";"original_amount";)"
        R"("original_currency";"fx_rate";"description";"transaction_id";"counterparty_name";)"
        R"("counterparty_iban";"payment_reference";"mcc_code")"
        "\n"
        R"("2024-01-15T10:00:00.000Z";"2024-01-15";"DEFAULT";"TRADING";"BUY";"STOCK";)"
        R"("Synthetic Test Share";"XX9000000001";"0,5";"10,25";"-5,125";"";"";"EUR";"";"";"";)"
        R"("Synthetic test row";"synthetic-semicolon-buy";"";"";"";"")"
        "\n"
        R"("2024-01-16T10:00:00.000Z";"2024-01-16";"DEFAULT";"TRADING";"SELL";"STOCK";)"
        R"("Synthetic Test Share";"XX9000000001";"-0,5";"12,50";"6,25";"";"";"EUR";"";"";"";)"
        R"("Synthetic test row";"synthetic-semicolon-sell";"";"";"";"")"
        "\n");

    expectNoParsedRecords(parseResult);
    ASSERT_EQ(parseResult.mDiagnostics.size(), 1U);
    EXPECT_EQ(parseResult.mDiagnostics.front().mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(countDiagnostics(parseResult, DiagnosticCode::FieldCountMismatch), 0U);
    EXPECT_EQ(countDiagnostics(parseResult, DiagnosticCode::LocalizedNumberFormat), 0U);
}

TEST(TradeRepublicParserTest, RejectsAHeaderOnlyFileWithARenamedColumn) {
    // The 23 column names, but "category" is written as "kategorie".
    const ParseResult parseResult = parseRawText(
        R"("datetime","date","account_type","kategorie","type","asset_class","name","symbol",)"
        R"("shares","price","amount","fee","tax","currency","original_amount",)"
        R"("original_currency","fx_rate","description","transaction_id","counterparty_name",)"
        R"("counterparty_iban","payment_reference","mcc_code")"
        "\n");

    expectNoParsedRecords(parseResult);
    ASSERT_EQ(parseResult.mDiagnostics.size(), 1U);

    const auto& diagnostic = parseResult.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(diagnostic.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsAHeaderOnlyFileWithARenamedColumn.csv");
    EXPECT_FALSE(diagnostic.mRowIndex.has_value());
    EXPECT_FALSE(diagnostic.mField.has_value());
    EXPECT_FALSE(diagnostic.mTransactionId.has_value());
    EXPECT_EQ(diagnostic.mMessage,
              "The file is not a Trade Republic transaction export. Required columns are "
              "missing: 1 (category). No rows were imported.");
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::InvalidHeaderMissingColumns);
    EXPECT_EQ(
        diagnostic.mMessageParams,
        (std::vector<MessageParam>{numberParam("count", 1U), textParam("columns", "category")}));
}

TEST(TradeRepublicParserTest, RejectsAHeaderWithSeveralMissingColumns) {
    // 20 columns: "price", "amount" and "fx_rate" are missing. The row has 20 fields and must not
    // be read.
    const ParseResult parseResult = parseRawText(
        R"("datetime","date","account_type","category","type","asset_class","name","symbol",)"
        R"("shares","fee","tax","currency","original_amount","original_currency",)"
        R"("description","transaction_id","counterparty_name","counterparty_iban",)"
        R"("payment_reference","mcc_code")"
        "\n"
        R"("2024-01-15T10:00:00.000Z","2024-01-15","DEFAULT","TRADING","BUY","STOCK",)"
        R"("Synthetic Test Share","XX9000000001","1.0000000000","","","EUR","","",)"
        R"("Synthetic test row","synthetic-test-transaction","","","","")"
        "\n");

    expectNoParsedRecords(parseResult);
    ASSERT_EQ(parseResult.mDiagnostics.size(), 1U);
    EXPECT_EQ(parseResult.mDiagnostics.front().mCode, DiagnosticCode::InvalidHeader);
    EXPECT_FALSE(parseResult.mDiagnostics.front().mRowIndex.has_value());
    EXPECT_EQ(parseResult.mDiagnostics.front().mMessage,
              "The file is not a Trade Republic transaction export. Required columns are "
              "missing: 3 (price, amount, fx_rate). No rows were imported.");
    EXPECT_EQ(parseResult.mDiagnostics.front().mMessageKey,
              MessageKey::InvalidHeaderMissingColumns);
    EXPECT_EQ(parseResult.mDiagnostics.front().mMessageParams,
              (std::vector<MessageParam>{numberParam("count", 3U),
                                         textParam("columns", "price, amount, fx_rate")}));
}

TEST(TradeRepublicParserTest, RejectsAHeaderWithARenamedColumnAndARow) {
    // The header names "category" as "kategorie"; the row below it is a valid BUY.
    const ParseResult parseResult = parseRawText(
        R"("datetime","date","account_type","kategorie","type","asset_class","name","symbol",)"
        R"("shares","price","amount","fee","tax","currency","original_amount",)"
        R"("original_currency","fx_rate","description","transaction_id","counterparty_name",)"
        R"("counterparty_iban","payment_reference","mcc_code")"
        "\n"
        R"("2024-01-15T10:00:00.000Z","2024-01-15","DEFAULT","TRADING","BUY","STOCK",)"
        R"("Synthetic Test Share","XX9000000001","1.0000000000","10.000000","-10.000000",)"
        R"("","","EUR","","","","Synthetic test row","synthetic-test-transaction","","","","")"
        "\n");

    expectNoParsedRecords(parseResult);
    ASSERT_EQ(parseResult.mDiagnostics.size(), 1U);

    const auto& diagnostic = parseResult.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(diagnostic.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsAHeaderWithARenamedColumnAndARow.csv");
    EXPECT_FALSE(diagnostic.mRowIndex.has_value());
    EXPECT_FALSE(diagnostic.mField.has_value());
    EXPECT_FALSE(diagnostic.mTransactionId.has_value());
}

TEST(TradeRepublicParserTest, AcceptsAHeaderWithoutTheUnusedColumns) {
    // Only the 18 columns the parser reads; the 5 unused export columns are absent.
    const ParseResult parseResult = parseRawText(
        R"("datetime","date","category","type","asset_class","name","symbol","shares","price",)"
        R"("amount","fee","tax","currency","original_amount","original_currency","fx_rate",)"
        R"("description","transaction_id")"
        "\n"
        R"("2024-01-15T10:00:00.000Z","2024-01-15","TRADING","BUY","STOCK",)"
        R"("Synthetic Test Share","XX9000000001","1.0000000000","10.000000","-10.000000",)"
        R"("","","EUR","","","","Synthetic test row","synthetic-test-transaction")"
        "\n");

    EXPECT_TRUE(parseResult.mDiagnostics.empty());
    ASSERT_EQ(parseResult.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(parseResult.mStatement.mTradeInstruments.front().mTransactions.size(), 1U);
}

TEST(TradeRepublicParserTest, RejectsASingleColumnHeaderWithoutASeparator) {
    // One column named "datetime": no semicolon or tab, so this is not a separator problem.
    const ParseResult parseResult = parseRawText(R"("datetime")"
                                                 "\n");

    expectNoParsedRecords(parseResult);
    ASSERT_EQ(parseResult.mDiagnostics.size(), 1U);

    const auto& diagnostic = parseResult.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::InvalidHeader);
    EXPECT_EQ(diagnostic.mMessage,
              "The file is not a Trade Republic transaction export. Required columns are "
              "missing: 17 (date, category, type, asset_class, name, symbol, shares, price, "
              "amount, fee, tax, currency, original_amount, original_currency, fx_rate, "
              "description, transaction_id). No rows were imported.");
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::InvalidHeaderMissingColumns);
    EXPECT_EQ(diagnostic.mMessageParams,
              (std::vector<MessageParam>{
                  numberParam("count", 17U),
                  textParam("columns",
                            "date, category, type, asset_class, name, symbol, shares, price, "
                            "amount, fee, tax, currency, original_amount, original_currency, "
                            "fx_rate, description, transaction_id")}));
}

TEST(TradeRepublicParserTest, KeepsParseErrorForAnEmptyFile) {
    // A file of 0 bytes has no header at all.
    const ParseResult parseResult = parseRawText("");

    expectNoParsedRecords(parseResult);
    ASSERT_EQ(parseResult.mDiagnostics.size(), 1U);

    const auto& diagnostic = parseResult.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::ParseError);
    EXPECT_FALSE(diagnostic.mRowIndex.has_value());
}

TEST(TradeRepublicParserTest, ReportsAShortMiddleRowOnItsOwnRow) {
    const SyntheticCsvRow firstBuy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mCurrency = "EUR",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-buy-first",
    };
    // 22 fields: the last field (mcc_code) is missing.
    const std::string shortSellLine =
        R"("2024-01-16T10:00:00.000Z","2024-01-16","DEFAULT","TRADING","SELL","STOCK",)"
        R"("Synthetic Test Share","XX9000000001","-1.0000000000","12.000000","12.000000",)"
        R"("","","EUR","","","","Synthetic test row","synthetic-sell-short",)"
        R"("","","")";
    const SyntheticCsvRow badPriceBuy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "abc",
        .mAmount = "-10.000000",
        .mCurrency = "EUR",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-buy-bad-price",
    };

    const std::string text = std::string{kCsvHeader} + '\n' + csvLine(firstBuy) + '\n' +
                             shortSellLine + '\n' + csvLine(badPriceBuy) + '\n';

    const ParseResult parseResult = parseRawText(text);

    // Row 2 is imported; the SELL on row 3 and the BUY on row 4 are rejected.
    EXPECT_EQ(tradeTransactionCount(parseResult.mStatement), 1U);

    const auto* buy = findTradeTransaction(parseResult.mStatement, "synthetic-buy-first");
    ASSERT_NE(buy, nullptr);

    EXPECT_EQ(primarySource(buy->mMetadata).mSourceRow, 2U);
    EXPECT_EQ(primarySource(buy->mMetadata).mInputSequence.mEventIndex, 0U);

    ASSERT_EQ(parseResult.mDiagnostics.size(), 2U);

    const auto& mismatch = parseResult.mDiagnostics[0];

    EXPECT_EQ(mismatch.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(mismatch.mCode, DiagnosticCode::FieldCountMismatch);
    EXPECT_EQ(mismatch.mMessageKey, MessageKey::FieldCountMismatch);
    EXPECT_EQ(mismatch.mMessageParams,
              (std::vector<MessageParam>{numberParam("expected", 23U), numberParam("found", 22U)}));
    EXPECT_EQ(mismatch.mRowIndex, std::optional<std::size_t>{3});
    EXPECT_FALSE(mismatch.mTransactionId.has_value());
    EXPECT_FALSE(mismatch.mField.has_value());
    EXPECT_EQ(mismatch.mMessage,
              "The row has a different number of fields than the header (expected 23, found "
              "22); the row was skipped.");

    const auto& invalidPrice = parseResult.mDiagnostics[1];

    EXPECT_EQ(invalidPrice.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(invalidPrice.mRowIndex, std::optional<std::size_t>{4}); // not 3 as before the fix
    EXPECT_EQ(invalidPrice.mTransactionId, std::optional<std::string>{"synthetic-buy-bad-price"});
    EXPECT_EQ(invalidPrice.mField, std::optional<std::string>{"price"});
}

TEST(TradeRepublicParserTest, ReportsALongMiddleRowOnItsOwnRow) {
    const SyntheticCsvRow firstBuy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mCurrency = "EUR",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-buy-first",
    };
    // 24 fields: one extra field ("extra") after mcc_code.
    const std::string longSellLine =
        R"("2024-01-16T10:00:00.000Z","2024-01-16","DEFAULT","TRADING","SELL","STOCK",)"
        R"("Synthetic Test Share","XX9000000001","-1.0000000000","12.000000","12.000000",)"
        R"("","","EUR","","","","Synthetic test row","synthetic-sell-long",)"
        R"("","","","","extra")";
    const SyntheticCsvRow badPriceBuy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "abc",
        .mAmount = "-10.000000",
        .mCurrency = "EUR",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-buy-bad-price",
    };

    const std::string text = std::string{kCsvHeader} + '\n' + csvLine(firstBuy) + '\n' +
                             longSellLine + '\n' + csvLine(badPriceBuy) + '\n';

    const ParseResult parseResult = parseRawText(text);

    EXPECT_EQ(tradeTransactionCount(parseResult.mStatement), 1U);

    const auto* buy = findTradeTransaction(parseResult.mStatement, "synthetic-buy-first");
    ASSERT_NE(buy, nullptr);

    EXPECT_EQ(primarySource(buy->mMetadata).mSourceRow, 2U);
    EXPECT_EQ(primarySource(buy->mMetadata).mInputSequence.mEventIndex, 0U);

    ASSERT_EQ(parseResult.mDiagnostics.size(), 2U);

    const auto& mismatch = parseResult.mDiagnostics[0];

    EXPECT_EQ(mismatch.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(mismatch.mCode, DiagnosticCode::FieldCountMismatch);
    EXPECT_EQ(mismatch.mRowIndex, std::optional<std::size_t>{3});
    EXPECT_FALSE(mismatch.mTransactionId.has_value());
    EXPECT_FALSE(mismatch.mField.has_value());
    EXPECT_EQ(mismatch.mMessage,
              "The row has a different number of fields than the header (expected 23, found "
              "24); the row was skipped.");

    const auto& invalidPrice = parseResult.mDiagnostics[1];

    EXPECT_EQ(invalidPrice.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(invalidPrice.mRowIndex, std::optional<std::size_t>{4});
    EXPECT_EQ(invalidPrice.mTransactionId, std::optional<std::string>{"synthetic-buy-bad-price"});
    EXPECT_EQ(invalidPrice.mField, std::optional<std::string>{"price"});
}

TEST(TradeRepublicParserTest, ImportsValidRowsBeforeAndAfterAMalformedRow) {
    const SyntheticCsvRow firstBuy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "2.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-20.000000",
        .mCurrency = "EUR",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-buy-first",
    };
    // 22 fields: the last field (mcc_code) is missing.
    const std::string shortBuyLine =
        R"("2024-01-15T10:00:00.000Z","2024-01-15","DEFAULT","TRADING","BUY","STOCK",)"
        R"("Synthetic Test Share","XX9000000001","1.0000000000","10.000000","-10.000000",)"
        R"("","","EUR","","","","Synthetic test row","synthetic-buy-short",)"
        R"("","","")";
    const SyntheticCsvRow lastSell{
        .mDatetime = "2024-01-16T10:00:00.000Z",
        .mDate = "2024-01-16",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "SELL",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "-1.0000000000",
        .mPrice = "12.000000",
        .mAmount = "12.000000",
        .mCurrency = "EUR",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-sell-last",
    };

    const std::string text = std::string{kCsvHeader} + '\n' + csvLine(firstBuy) + '\n' +
                             shortBuyLine + '\n' + csvLine(lastSell) + '\n';

    const ParseResult parseResult = parseRawText(text);

    EXPECT_EQ(tradeTransactionCount(parseResult.mStatement), 2U);
    EXPECT_EQ(findTradeTransaction(parseResult.mStatement, "synthetic-buy-short"), nullptr);

    const auto* first = findTradeTransaction(parseResult.mStatement, "synthetic-buy-first");
    const auto* last = findTradeTransaction(parseResult.mStatement, "synthetic-sell-last");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(last, nullptr);

    EXPECT_EQ(primarySource(first->mMetadata).mSourceRow, 2U);
    EXPECT_EQ(primarySource(first->mMetadata).mInputSequence.mEventIndex, 0U);
    EXPECT_EQ(primarySource(last->mMetadata).mSourceRow, 4U);
    EXPECT_EQ(primarySource(last->mMetadata).mInputSequence.mEventIndex, 2U);

    ASSERT_EQ(parseResult.mDiagnostics.size(), 1U);

    const auto& mismatch = parseResult.mDiagnostics.front();

    EXPECT_EQ(mismatch.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(mismatch.mCode, DiagnosticCode::FieldCountMismatch);
    EXPECT_EQ(mismatch.mRowIndex, std::optional<std::size_t>{3});
}

TEST(TradeRepublicParserTest, ReportsATruncatedFinalRowCutAtAFieldBoundary) {
    const SyntheticCsvRow firstBuy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mCurrency = "EUR",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-buy-first",
    };
    // 10 fields: the SELL is cut after price, and the file has no final newline.
    const std::string truncatedSell =
        R"("2024-01-16T10:00:00.000Z","2024-01-16","DEFAULT","TRADING","SELL","STOCK",)"
        R"("Synthetic Test Share","XX9000000001","-1.0000000000","12.000000")";

    const std::string text =
        std::string{kCsvHeader} + '\n' + csvLine(firstBuy) + '\n' + truncatedSell;

    const ParseResult parseResult = parseRawText(text);

    EXPECT_EQ(tradeTransactionCount(parseResult.mStatement), 1U);

    const auto* buy = findTradeTransaction(parseResult.mStatement, "synthetic-buy-first");
    ASSERT_NE(buy, nullptr);

    EXPECT_EQ(primarySource(buy->mMetadata).mSourceRow, 2U);
    EXPECT_EQ(primarySource(buy->mMetadata).mInputSequence.mEventIndex, 0U);

    ASSERT_EQ(parseResult.mDiagnostics.size(), 1U);

    const auto& mismatch = parseResult.mDiagnostics.front();

    EXPECT_EQ(mismatch.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(mismatch.mCode, DiagnosticCode::FieldCountMismatch);
    EXPECT_EQ(mismatch.mRowIndex, std::optional<std::size_t>{3});
    EXPECT_FALSE(mismatch.mTransactionId.has_value());
    EXPECT_FALSE(mismatch.mField.has_value());
    EXPECT_EQ(mismatch.mMessage,
              "The row has a different number of fields than the header (expected 23, found "
              "10); the row was skipped.");
}

TEST(TradeRepublicParserTest, FailsTheFileWhenTheFinalRowIsCutInsideAQuotedField) {
    const SyntheticCsvRow firstBuy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mCurrency = "EUR",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-buy-first",
    };
    // The SELL is cut in the middle of the price value ("12.0): the quote never closes.
    const std::string truncatedSell =
        R"("2024-01-16T10:00:00.000Z","2024-01-16","DEFAULT","TRADING","SELL","STOCK",)"
        R"("Synthetic Test Share","XX9000000001","-1.0000000000","12.0)";

    const std::string text =
        std::string{kCsvHeader} + '\n' + csvLine(firstBuy) + '\n' + truncatedSell;

    const ParseResult parseResult = parseRawText(text);

    expectNoParsedRecords(parseResult);

    ASSERT_EQ(parseResult.mDiagnostics.size(), 1U);

    const auto& diagnostic = parseResult.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::ParseError);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::ParseErrorUnclosedQuote);
    EXPECT_TRUE(diagnostic.mMessageParams.empty());
    EXPECT_FALSE(diagnostic.mRowIndex.has_value());
    EXPECT_EQ(diagnostic.mMessage,
              "The CSV file has an unclosed quote; it may be cut off or edited. No rows were "
              "imported.");
}

TEST(TradeRepublicParserTest, FailsTheFileWhenAQuoteInTheLastFieldNeverCloses) {
    const SyntheticCsvRow firstBuy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mCurrency = "EUR",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-buy-first",
    };
    // 23 fields, but the last one ("5411) opens a quote that never closes. Without the quote
    // check the reader would merge row 4 into that field and import no SELL.
    const std::string openQuoteBuyLine =
        R"("2024-01-15T10:00:00.000Z","2024-01-15","DEFAULT","TRADING","BUY","STOCK",)"
        R"("Synthetic Test Share","XX9000000001","1.0000000000","10.000000","-10.000000",)"
        R"("","","EUR","","","","Synthetic test row","synthetic-buy-open-quote",)"
        R"("","","","5411)";
    const SyntheticCsvRow lastSell{
        .mDatetime = "2024-01-16T10:00:00.000Z",
        .mDate = "2024-01-16",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "SELL",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "-1.0000000000",
        .mPrice = "12.000000",
        .mAmount = "12.000000",
        .mCurrency = "EUR",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-sell-last",
    };

    const std::string text = std::string{kCsvHeader} + '\n' + csvLine(firstBuy) + '\n' +
                             openQuoteBuyLine + '\n' + csvLine(lastSell) + '\n';

    const ParseResult parseResult = parseRawText(text);

    expectNoParsedRecords(parseResult);

    ASSERT_EQ(parseResult.mDiagnostics.size(), 1U);

    const auto& diagnostic = parseResult.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::ParseError);
    EXPECT_FALSE(diagnostic.mRowIndex.has_value());
}

TEST(TradeRepublicParserTest, FailsTheFileWhenTwoUnclosedQuotesMergeRows) {
    const SyntheticCsvRow firstBuy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mCurrency = "EUR",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-buy-first",
    };
    // Rows 3 and 5 each open a quote in their last field ("5411). The file has an even number of
    // quote characters, so the unclosed-quote check does not fire. The reader merges rows 3 and 4
    // into one record of 45 fields with a line break inside a field, which fails the file.
    const std::string openQuoteBuyLineOne =
        R"("2024-01-15T10:00:00.000Z","2024-01-15","DEFAULT","TRADING","BUY","STOCK",)"
        R"("Synthetic Test Share","XX9000000001","1.0000000000","10.000000","-10.000000",)"
        R"("","","EUR","","","","Synthetic test row","synthetic-buy-open-quote-one",)"
        R"("","","","5411)";
    const SyntheticCsvRow middleSell{
        .mDatetime = "2024-01-16T10:00:00.000Z",
        .mDate = "2024-01-16",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "SELL",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "-1.0000000000",
        .mPrice = "12.000000",
        .mAmount = "12.000000",
        .mCurrency = "EUR",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-sell-middle",
    };
    const std::string openQuoteBuyLineTwo =
        R"("2024-01-15T10:00:00.000Z","2024-01-15","DEFAULT","TRADING","BUY","STOCK",)"
        R"("Synthetic Test Share","XX9000000001","1.0000000000","10.000000","-10.000000",)"
        R"("","","EUR","","","","Synthetic test row","synthetic-buy-open-quote-two",)"
        R"("","","","5411)";
    const SyntheticCsvRow lastBuy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mCurrency = "EUR",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-buy-last",
    };

    const std::string text = std::string{kCsvHeader} + '\n' + csvLine(firstBuy) + '\n' +
                             openQuoteBuyLineOne + '\n' + csvLine(middleSell) + '\n' +
                             openQuoteBuyLineTwo + '\n' + csvLine(lastBuy) + '\n';

    const ParseResult parseResult = parseRawText(text);

    expectNoParsedRecords(parseResult);

    ASSERT_EQ(parseResult.mDiagnostics.size(), 1U);

    const auto& diagnostic = parseResult.mDiagnostics.front();

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::ParseError);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::ParseErrorLineBreakInField);
    EXPECT_TRUE(diagnostic.mMessageParams.empty());
    EXPECT_FALSE(diagnostic.mRowIndex.has_value());
    EXPECT_EQ(diagnostic.mMessage,
              "A row with the wrong number of fields contains a line break inside a field; a "
              "quote is probably unclosed. No rows were imported.");
}

TEST(TradeRepublicParserTest, QuotedNewlineKeepsLogicalRowNumbers) {
    const SyntheticCsvRow multilineBuy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mCurrency = "EUR",
        .mDescription = "line one\nline two",
        .mTransactionId = "synthetic-buy-multiline",
    };
    // 22 fields: the last field (mcc_code) is missing.
    const std::string shortSellLine =
        R"("2024-01-16T10:00:00.000Z","2024-01-16","DEFAULT","TRADING","SELL","STOCK",)"
        R"("Synthetic Test Share","XX9000000001","-1.0000000000","12.000000","12.000000",)"
        R"("","","EUR","","","","Synthetic test row","synthetic-sell-short",)"
        R"("","","")";
    const SyntheticCsvRow badPriceBuy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "abc",
        .mAmount = "-10.000000",
        .mCurrency = "EUR",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-buy-bad-price",
    };

    // Text lines: header 1, multiline BUY 2-3, short SELL 4, bad-price BUY 5.
    // Logical rows: header 1, multiline BUY 2, short SELL 3, bad-price BUY 4.
    const std::string text = std::string{kCsvHeader} + '\n' + csvLine(multilineBuy) + '\n' +
                             shortSellLine + '\n' + csvLine(badPriceBuy) + '\n';

    const ParseResult parseResult = parseRawText(text);

    EXPECT_EQ(tradeTransactionCount(parseResult.mStatement), 1U);

    const auto* buy = findTradeTransaction(parseResult.mStatement, "synthetic-buy-multiline");
    ASSERT_NE(buy, nullptr);

    EXPECT_EQ(primarySource(buy->mMetadata).mSourceRow, 2U);
    EXPECT_EQ(primarySource(buy->mMetadata).mInputSequence.mEventIndex, 0U);

    ASSERT_EQ(parseResult.mDiagnostics.size(), 2U);

    const auto& mismatch = parseResult.mDiagnostics[0];

    EXPECT_EQ(mismatch.mCode, DiagnosticCode::FieldCountMismatch);
    EXPECT_EQ(mismatch.mRowIndex, std::optional<std::size_t>{3}); // text line 4
    EXPECT_EQ(mismatch.mMessage,
              "The row has a different number of fields than the header (expected 23, found "
              "22); the row was skipped.");

    const auto& invalidPrice = parseResult.mDiagnostics[1];

    EXPECT_EQ(invalidPrice.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(invalidPrice.mRowIndex, std::optional<std::size_t>{4}); // text line 5
    EXPECT_EQ(invalidPrice.mTransactionId, std::optional<std::string>{"synthetic-buy-bad-price"});
    EXPECT_EQ(invalidPrice.mField, std::optional<std::string>{"price"});
}

TEST(TradeRepublicParserTest, IgnoresSeveralBlankLinesBetweenRecords) {
    const SyntheticCsvRow firstBuy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mCurrency = "EUR",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-buy-first",
    };
    const SyntheticCsvRow lastBuy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mCurrency = "EUR",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-buy-last",
    };

    // Two empty lines: neither is a record, so the last buy is on row 3.
    const std::string text =
        std::string{kCsvHeader} + '\n' + csvLine(firstBuy) + "\n\n\n" + csvLine(lastBuy) + '\n';

    const ParseResult parseResult = parseRawText(text);

    EXPECT_EQ(tradeTransactionCount(parseResult.mStatement), 2U);

    const auto* first = findTradeTransaction(parseResult.mStatement, "synthetic-buy-first");
    const auto* last = findTradeTransaction(parseResult.mStatement, "synthetic-buy-last");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(last, nullptr);

    EXPECT_EQ(primarySource(first->mMetadata).mSourceRow, 2U);
    EXPECT_EQ(primarySource(first->mMetadata).mInputSequence.mEventIndex, 0U);
    EXPECT_EQ(primarySource(last->mMetadata).mSourceRow, 3U);
    EXPECT_EQ(primarySource(last->mMetadata).mInputSequence.mEventIndex, 1U);

    EXPECT_TRUE(parseResult.mDiagnostics.empty());
}

TEST(TradeRepublicParserTest, ReportsASpacesOnlyLineAsAFieldCountError) {
    const SyntheticCsvRow firstBuy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mCurrency = "EUR",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-buy-first",
    };
    const SyntheticCsvRow lastBuy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mCurrency = "EUR",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-buy-last",
    };

    // A line with one space is not empty: the reader returns a record with one field on row 3.
    const std::string text =
        std::string{kCsvHeader} + '\n' + csvLine(firstBuy) + "\n \n" + csvLine(lastBuy) + '\n';

    const ParseResult parseResult = parseRawText(text);

    EXPECT_EQ(tradeTransactionCount(parseResult.mStatement), 2U);

    const auto* first = findTradeTransaction(parseResult.mStatement, "synthetic-buy-first");
    const auto* last = findTradeTransaction(parseResult.mStatement, "synthetic-buy-last");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(last, nullptr);

    EXPECT_EQ(primarySource(first->mMetadata).mSourceRow, 2U);
    EXPECT_EQ(primarySource(first->mMetadata).mInputSequence.mEventIndex, 0U);
    EXPECT_EQ(primarySource(last->mMetadata).mSourceRow, 4U);
    EXPECT_EQ(primarySource(last->mMetadata).mInputSequence.mEventIndex, 2U);

    ASSERT_EQ(parseResult.mDiagnostics.size(), 1U);

    const auto& mismatch = parseResult.mDiagnostics.front();

    EXPECT_EQ(mismatch.mCode, DiagnosticCode::FieldCountMismatch);
    EXPECT_EQ(mismatch.mRowIndex, std::optional<std::size_t>{3});
    EXPECT_EQ(mismatch.mMessage,
              "The row has a different number of fields than the header (expected 23, found "
              "1); the row was skipped.");
}

TEST(TradeRepublicParserTest, IgnoresASingleBlankLineBetweenRecords) {
    const SyntheticCsvRow firstBuy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mCurrency = "EUR",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-buy-first",
    };
    const SyntheticCsvRow lastBuy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mCurrency = "EUR",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-buy-last",
    };

    // One empty line: it is not a record, so the last buy is on row 3.
    const std::string text =
        std::string{kCsvHeader} + '\n' + csvLine(firstBuy) + "\n\n" + csvLine(lastBuy) + '\n';

    const ParseResult parseResult = parseRawText(text);

    EXPECT_EQ(tradeTransactionCount(parseResult.mStatement), 2U);

    const auto* first = findTradeTransaction(parseResult.mStatement, "synthetic-buy-first");
    const auto* last = findTradeTransaction(parseResult.mStatement, "synthetic-buy-last");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(last, nullptr);

    EXPECT_EQ(primarySource(first->mMetadata).mSourceRow, 2U);
    EXPECT_EQ(primarySource(first->mMetadata).mInputSequence.mEventIndex, 0U);
    EXPECT_EQ(primarySource(last->mMetadata).mSourceRow, 3U);
    EXPECT_EQ(primarySource(last->mMetadata).mInputSequence.mEventIndex, 1U);

    EXPECT_TRUE(parseResult.mDiagnostics.empty());
}

TEST(TradeRepublicParserTest, AcceptsWindowsLineEndings) {
    const SyntheticCsvRow firstBuy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mCurrency = "EUR",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-buy-first",
    };
    const SyntheticCsvRow lastBuy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mCurrency = "EUR",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-buy-last",
    };

    const std::string text =
        std::string{kCsvHeader} + "\r\n" + csvLine(firstBuy) + "\r\n" + csvLine(lastBuy) + "\r\n";

    const ParseResult parseResult = parseRawText(text);

    EXPECT_EQ(tradeTransactionCount(parseResult.mStatement), 2U);

    const auto* first = findTradeTransaction(parseResult.mStatement, "synthetic-buy-first");
    const auto* last = findTradeTransaction(parseResult.mStatement, "synthetic-buy-last");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(last, nullptr);

    EXPECT_EQ(primarySource(first->mMetadata).mSourceRow, 2U);
    EXPECT_EQ(primarySource(first->mMetadata).mInputSequence.mEventIndex, 0U);
    EXPECT_EQ(primarySource(last->mMetadata).mSourceRow, 3U);
    EXPECT_EQ(primarySource(last->mMetadata).mInputSequence.mEventIndex, 1U);

    EXPECT_TRUE(parseResult.mDiagnostics.empty());
}

TEST(TradeRepublicParserTest, IgnoresBlankLinesInAFileWithWindowsLineEndings) {
    const SyntheticCsvRow firstBuy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mCurrency = "EUR",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-buy-first",
    };
    const SyntheticCsvRow lastBuy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mCurrency = "EUR",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-buy-last",
    };

    // Two empty CRLF lines: neither is a record, so the last buy is on row 3.
    const std::string text = std::string{kCsvHeader} + "\r\n" + csvLine(firstBuy) + "\r\n\r\n\r\n" +
                             csvLine(lastBuy) + "\r\n";

    const ParseResult parseResult = parseRawText(text);

    EXPECT_EQ(tradeTransactionCount(parseResult.mStatement), 2U);

    const auto* first = findTradeTransaction(parseResult.mStatement, "synthetic-buy-first");
    const auto* last = findTradeTransaction(parseResult.mStatement, "synthetic-buy-last");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(last, nullptr);

    EXPECT_EQ(primarySource(first->mMetadata).mSourceRow, 2U);
    EXPECT_EQ(primarySource(first->mMetadata).mInputSequence.mEventIndex, 0U);
    EXPECT_EQ(primarySource(last->mMetadata).mSourceRow, 3U);
    EXPECT_EQ(primarySource(last->mMetadata).mInputSequence.mEventIndex, 1U);

    EXPECT_TRUE(parseResult.mDiagnostics.empty());
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
    EXPECT_EQ(transaction.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transaction.mGrossAmount, 100'000);
    EXPECT_EQ(transaction.mTaxPaid, 10'000);
    EXPECT_EQ(transaction.mExchangeRate, EXCHANGE_RATE_SCALE);
    EXPECT_EQ(transaction.mCurrency, Currency::EUR);
    EXPECT_EQ(transaction.mTaxCurrency, Currency::EUR);
    EXPECT_EQ(transactionId(transaction.mMetadata), "synthetic-dividend-validation");
    EXPECT_TRUE(parseResult.mDiagnostics.empty());
}

TEST(TradeRepublicParserTest, PopulatesCompleteSourceMetadataForEveryEventType) {
    SyntheticCsvRow trade;
    SyntheticCsvRow dividend = makeDividendRow();
    SyntheticCsvRow brokerInterest = makeBrokerInterestRow();
    SyntheticCsvRow bondInterest = makeBondInterestRow();
    SyntheticCsvRow corporateAction = makeCorporateActionRow();
    SyntheticCsvRow benefit = makeBenefitRow();
    SyntheticCsvRow privateMarket = makePrivateMarketRow();

    const std::array rows{
        &trade,
        &dividend,
        &brokerInterest,
        &bondInterest,
        &corporateAction,
        &benefit,
        &privateMarket,
    };
    for (auto* row : rows)
    {
        row->mDatetime = "2024-01-15T00:30:00.123456789+01:30";
    }

    const TemporaryCsvFile csvFile{
        {trade, dividend, brokerInterest, bondInterest, corporateAction, benefit, privateMarket}};
    tr::TradeRepublicParser parser;
    const ParseResult parseResult = parser.parse(csvFile.path(), 7);
    const auto& statement = parseResult.mStatement;

    const auto* tradeTransaction = findTradeTransaction(statement, trade.mTransactionId);
    const auto* dividendTransaction = findDividendTransaction(statement, dividend.mTransactionId);
    const auto* brokerInterestTransaction =
        findInterestTransaction(statement, brokerInterest.mTransactionId);
    const auto* bondInterestTransaction =
        findInterestTransaction(statement, bondInterest.mTransactionId);
    const auto* action = findCorporateAction(statement, corporateAction.mTransactionId);
    const auto* benefitEvent = findBenefitEvent(statement, benefit.mTransactionId);
    const auto* privateMarketEvent =
        findPrivateMarketEvent(statement, privateMarket.mTransactionId);

    ASSERT_NE(tradeTransaction, nullptr);
    ASSERT_NE(dividendTransaction, nullptr);
    ASSERT_NE(brokerInterestTransaction, nullptr);
    ASSERT_NE(bondInterestTransaction, nullptr);
    ASSERT_NE(action, nullptr);
    ASSERT_NE(benefitEvent, nullptr);
    ASSERT_NE(privateMarketEvent, nullptr);

    expectSourceMetadata(tradeTransaction->mMetadata,
                         csvFile.path(),
                         2,
                         trade.mTransactionId,
                         7,
                         0);
    expectSourceMetadata(dividendTransaction->mMetadata,
                         csvFile.path(),
                         3,
                         dividend.mTransactionId,
                         7,
                         1);
    expectSourceMetadata(brokerInterestTransaction->mMetadata,
                         csvFile.path(),
                         4,
                         brokerInterest.mTransactionId,
                         7,
                         2);
    expectSourceMetadata(bondInterestTransaction->mMetadata,
                         csvFile.path(),
                         5,
                         bondInterest.mTransactionId,
                         7,
                         3);
    expectSourceMetadata(action->mMetadata,
                         csvFile.path(),
                         6,
                         corporateAction.mTransactionId,
                         7,
                         4);
    expectSourceMetadata(benefitEvent->mMetadata, csvFile.path(), 7, benefit.mTransactionId, 7, 5);
    expectSourceMetadata(privateMarketEvent->mMetadata,
                         csvFile.path(),
                         8,
                         privateMarket.mTransactionId,
                         7,
                         6);

    EXPECT_TRUE(parseResult.mDiagnostics.empty());
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
    ASSERT_EQ(parseResult.mDiagnostics.size(), 1U);
    const auto& diagnostic = parseResult.mDiagnostics.front();
    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::UnknownRowType);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::UnknownRowType);
    EXPECT_EQ(diagnostic.mMessageParams,
              (std::vector<MessageParam>{textParam("type", "SYNTHETIC_UNKNOWN_EVENT"),
                                         textParam("category", "SYNTHETIC_CATEGORY")}));
    EXPECT_EQ(diagnostic.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(diagnostic.mRowIndex, 2U);
    ASSERT_TRUE(diagnostic.mTransactionId.has_value());
    EXPECT_EQ(*diagnostic.mTransactionId, row.mTransactionId);
    EXPECT_NE(diagnostic.mMessage.find(row.mType), std::string::npos);
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
    EXPECT_EQ(transaction->mMetadata.mTaxDate, makeDate(2024, 2, 29));
    EXPECT_FALSE(transaction->mMetadata.mSourceTimestamp.has_value());
    EXPECT_EQ(transaction->mUnitPrice, 300'000'000);
    EXPECT_EQ(transaction->mUnits, 200'000'000);
    ASSERT_TRUE(transaction->mAmount.has_value());
    EXPECT_EQ(*transaction->mAmount, 990'000);
    ASSERT_EQ(parseResult.mDiagnostics.size(), 1U);
    EXPECT_EQ(parseResult.mDiagnostics.front().mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(parseResult.mDiagnostics.front().mField, "datetime");
}

TEST(TradeRepublicParserTest, UsesDefaultHourWhenSourceDateIsEarlierThanReportingDate) {
    SyntheticCsvRow row;
    row.mDate = "2024-01-15";
    row.mDatetime = "2024-01-14T23:00:00.000Z";

    const auto result = parseRows({row});
    const auto* transaction = findTradeTransaction(result.mStatement, row.mTransactionId);
    const auto originalTimestamp =
        SourceTimestamp{makeDate(2024, 1, 14).time_since_epoch() + std::chrono::hours{23}};
    const auto defaultOrderingTimestamp =
        SourceTimestamp{makeDate(2024, 1, 15).time_since_epoch() + std::chrono::hours{9}};

    ASSERT_NE(transaction, nullptr);
    EXPECT_EQ(transaction->mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transaction->mMetadata.mSourceTimestamp, originalTimestamp);
    EXPECT_EQ(transaction->mMetadata.mOrderingTimestamp, defaultOrderingTimestamp);
    EXPECT_EQ(transaction->mUnitPrice, 1'000'000'000);
    EXPECT_EQ(transaction->mUnits, UNITS_SCALE);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    const auto& warning = result.mDiagnostics.front();

    EXPECT_EQ(warning.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(warning.mCode, DiagnosticCode::InconsistentValue);
    EXPECT_EQ(warning.mMessageKey, MessageKey::InconsistentValueDatetimeDate);
    EXPECT_EQ(warning.mMessageParams, (std::vector<MessageParam>{numberParam("defaultHour", 9U)}));
    EXPECT_EQ(warning.mField, "datetime");
    EXPECT_EQ(warning.mTransactionId, row.mTransactionId);
    EXPECT_EQ(warning.mRowIndex, 2U);
    EXPECT_NE(warning.mMessage.find("default hour 9:00:00 UTC"), std::string::npos);
    EXPECT_NE(warning.mMessage.find("FIFO or corporate-action order"), std::string::npos);
}

TEST(TradeRepublicParserTest, UsesDefaultHourWhenSourceDateIsLaterThanReportingDate) {
    SyntheticCsvRow row;
    row.mDate = "2024-01-15";
    row.mDatetime = "2024-01-16T01:00:00.000Z";

    const auto result = parseRows({row});
    const auto* transaction = findTradeTransaction(result.mStatement, row.mTransactionId);
    const auto originalTimestamp =
        SourceTimestamp{makeDate(2024, 1, 16).time_since_epoch() + std::chrono::hours{1}};
    const auto defaultOrderingTimestamp =
        SourceTimestamp{makeDate(2024, 1, 15).time_since_epoch() + std::chrono::hours{9}};

    ASSERT_NE(transaction, nullptr);
    EXPECT_EQ(transaction->mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transaction->mMetadata.mSourceTimestamp, originalTimestamp);
    EXPECT_EQ(transaction->mMetadata.mOrderingTimestamp, defaultOrderingTimestamp);
    EXPECT_EQ(transaction->mUnitPrice, 1'000'000'000);
    EXPECT_EQ(transaction->mUnits, UNITS_SCALE);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    const auto& warning = result.mDiagnostics.front();

    EXPECT_EQ(warning.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(warning.mCode, DiagnosticCode::InconsistentValue);
    EXPECT_EQ(warning.mField, "datetime");
    EXPECT_EQ(warning.mTransactionId, row.mTransactionId);
    EXPECT_EQ(warning.mRowIndex, 2U);
    EXPECT_NE(warning.mMessage.find("default hour 9:00:00 UTC"), std::string::npos);
    EXPECT_NE(warning.mMessage.find("FIFO or corporate-action order"), std::string::npos);
}

TEST(TradeRepublicParserTest, DoesNotWarnWhenOnlyUtcNormalizationChangesTheCalendarDate) {
    SyntheticCsvRow row;
    row.mDatetime = "2024-01-15T00:30:00.000+02:00";
    const auto result = parseRows({row});
    const auto* transaction = findTradeTransaction(result.mStatement, row.mTransactionId);
    const auto expectedUtc = SourceTimestamp{makeDate(2024, 1, 14).time_since_epoch() +
                                             std::chrono::hours{22} + std::chrono::minutes{30}};

    ASSERT_NE(transaction, nullptr);
    EXPECT_EQ(transaction->mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transaction->mMetadata.mSourceTimestamp, expectedUtc);
    EXPECT_FALSE(transaction->mMetadata.mOrderingTimestamp.has_value());
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(TradeRepublicParserTest, ContinuesParsingAfterAnInvalidRow) {
    SyntheticCsvRow invalidRow;
    invalidRow.mPrice = "not-a-number";
    invalidRow.mTransactionId = "synthetic-invalid-transaction";

    SyntheticCsvRow validRow;
    validRow.mDate = "2024-01-16";
    validRow.mDatetime = "2024-01-16T10:00:00.000Z";
    validRow.mTransactionId = "synthetic-valid-after-invalid";

    const ParseResult parseResult = parseRows({invalidRow, validRow});

    EXPECT_EQ(tradeTransactionCount(parseResult.mStatement), 1U);
    EXPECT_EQ(findTradeTransaction(parseResult.mStatement, invalidRow.mTransactionId), nullptr);
    const auto* validTransaction =
        findTradeTransaction(parseResult.mStatement, validRow.mTransactionId);
    ASSERT_NE(validTransaction, nullptr);
    EXPECT_EQ(validTransaction->mMetadata.mTaxDate, makeDate(2024, 1, 16));

    ASSERT_EQ(parseResult.mDiagnostics.size(), 1U);
    const auto& diagnostic = parseResult.mDiagnostics.front();
    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(diagnostic.mField, "price");
    EXPECT_EQ(diagnostic.mSourceFile,
              "taxbroker_TradeRepublicParserTest_ContinuesParsingAfterAnInvalidRow.csv");
    EXPECT_EQ(diagnostic.mRowIndex, 2U);
    EXPECT_EQ(diagnostic.mTransactionId, invalidRow.mTransactionId);
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
    EXPECT_TRUE(parseResult.mDiagnostics.empty());
}

TEST(TradeRepublicParserTest, RetainsTradeClassificationConflictForTheSameIsin) {
    const SyntheticCsvRow stock{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const SyntheticCsvRow fund{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "FUND",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-conflicting-asset",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{stock, fund};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mTradeInstruments[0];

    EXPECT_EQ(instrument.mAssetClass, AssetClass::Unknown);
    ASSERT_EQ(instrument.mTransactions.size(), 2U);

    const auto& purchase = instrument.mTransactions[0];
    const auto& other = instrument.mTransactions[1];

    EXPECT_EQ(purchase.mUnits, 100'000'000);       // Source 1 unit at eight decimal places.
    EXPECT_EQ(purchase.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.
    EXPECT_EQ(primarySource(purchase.mMetadata).mTransactionId, "synthetic-test-transaction");
    ASSERT_EQ(purchase.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mSource.mSourceRow, 2U);
    ASSERT_EQ(other.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(other.mInstrumentEvidence[0].mAssetClass, AssetClass::Fund);
    EXPECT_EQ(other.mInstrumentEvidence[0].mAssetClassLabel, "FUND");
    EXPECT_EQ(other.mInstrumentEvidence[0].mSource.mSourceRow, 3U);
    EXPECT_EQ(primarySource(other.mMetadata).mTransactionId, "synthetic-conflicting-asset");
    EXPECT_EQ(other.mUnits, 100'000'000);
    EXPECT_EQ(other.mUnitPrice, 1'000'000'000);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics[0].mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(result.mDiagnostics[0].mCode, DiagnosticCode::InconsistentValue);
    EXPECT_EQ(result.mDiagnostics[0].mField, "asset_class");
    EXPECT_EQ(result.mDiagnostics[0].mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(result.mDiagnostics[0].mRowIndex, 3U);
    EXPECT_EQ(result.mDiagnostics[0].mTransactionId, fund.mTransactionId);
}

TEST(TradeRepublicParserTest, WarnsAboutUnsupportedAssetsOnlyAfterAValidTradeIsPreserved) {
    SyntheticCsvRow malformedCrypto;
    malformedCrypto.mAssetClass = "CRYPTO";
    malformedCrypto.mPrice = "not-a-number";

    const ParseResult parseResult = parseRows({malformedCrypto});
    expectNoParsedRecords(parseResult);
    EXPECT_EQ(countDiagnostics(parseResult, DiagnosticCode::UnsupportedAssetClass), 0U);
}

struct InvalidRowCase {
    const char* mName;
    void (*mMutate)(SyntheticCsvRow&);
};

TEST(TradeRepublicParserTest, RejectsTradeWithMissingIdentity) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(accepted.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "",
        .mSymbol = "",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_FALSE(error.mField.has_value());
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsTradeWithMissingIsin) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(accepted.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mField, "symbol");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsTradeWithShortDate) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(accepted.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-1-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "date");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsTradeWithWrongDateSeparators) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(accepted.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024/01/15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "date");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsTradeWithNonNumericDate) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(accepted.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "202A-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "date");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsTradeWithImpossibleDate) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(accepted.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2023-02-29",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "date");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsTradeWithMissingPrice) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(accepted.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mMessageKey, MessageKey::MissingFieldRequiredField);
    EXPECT_EQ(error.mMessageParams,
              (std::vector<MessageParam>{textParam("field", "price"),
                                         termParam("rowKind", term::kRowKindTrade)}));
    EXPECT_EQ(error.mField, "price");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsTradeWithMalformedPrice) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(accepted.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "not-a-number",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "price");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsTradeWithZeroPrice) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(accepted.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "0",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "price");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsTradeWithNegativePrice) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(accepted.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "-0.01",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "price");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsTradeWithOverflowingPrice) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(accepted.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "92233720368.54775808", // One step above the largest UnitPrice.
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "price");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsTradeWithMissingUnits) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(accepted.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mField, "shares");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsTradeWithMalformedUnits) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(accepted.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "not-a-number",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "shares");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsTradeWithOverflowingUnits) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(accepted.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "92233720368.54775808",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "shares");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsTradeWithZeroUnits) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(accepted.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "0",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InconsistentValue);
    EXPECT_EQ(error.mField, "shares");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsTradeWithNegativeBuyUnits) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(accepted.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "-1.00",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InconsistentValue);
    EXPECT_EQ(error.mMessageKey, MessageKey::InconsistentValueTradeSide);
    EXPECT_EQ(error.mMessageParams, (std::vector<MessageParam>{textParam("field", "shares")}));
    EXPECT_EQ(error.mField, "shares");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsTradeWithPositiveSellUnits) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "SELL",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "-1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(accepted.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "SELL",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.00",
        .mPrice = "10.000000",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InconsistentValue);
    EXPECT_EQ(error.mField, "shares");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsTradeWithMinimumSignedUnits) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "SELL",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "-1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(accepted.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "SELL",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "-92233720368.54775808",
        .mPrice = "10.000000",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InconsistentValue);
    EXPECT_EQ(error.mField, "shares");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsTradeWithMalformedAmount) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(accepted.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "not-a-number",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "amount");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsTradeWithOverflowingAmount) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(accepted.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-922337203685477.5809",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "amount");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsTradeWithZeroAmount) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(accepted.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "0",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InconsistentValue);
    EXPECT_EQ(error.mField, "amount");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsTradeWithPositiveBuyAmount) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(accepted.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InconsistentValue);
    EXPECT_EQ(error.mField, "amount");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsTradeWithNegativeSellAmount) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "SELL",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "-1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(accepted.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "SELL",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "-1.00",
        .mPrice = "10.000000",
        .mAmount = "-10.00",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InconsistentValue);
    EXPECT_EQ(error.mField, "amount");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsTradeWithMinimumSignedAmount) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(accepted.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-922337203685477.5808",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InconsistentValue);
    EXPECT_EQ(error.mField, "amount");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsTradeWithMalformedFee) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(accepted.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "not-a-number",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fee");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsTradeWithPositiveFee) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(accepted.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "0.01",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fee");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsTradeWithMinimumSignedFee) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(accepted.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "-922337203685477.5808",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fee");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsTradeWithMissingCurrency) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(accepted.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mField, "currency");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsTradeWithUnsupportedCurrency) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(accepted.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "CAD",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "currency");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsDividendWithInvalidDate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "DIVIDEND",
                        .mAssetClass = "STOCK",
                        .mName = "Synthetic Validation Share",
                        .mSymbol = "XX9000000001",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-dividend-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mDate = "2024-02-30";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "date");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDividendWithInvalidDate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-dividend-validation");
}

TEST(TradeRepublicParserTest, RejectsDividendWithMalformedTax) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "DIVIDEND",
                        .mAssetClass = "STOCK",
                        .mName = "Synthetic Validation Share",
                        .mSymbol = "XX9000000001",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-dividend-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mTax = "not-a-number";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "tax");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDividendWithMalformedTax.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-dividend-validation");
}

TEST(TradeRepublicParserTest, RejectsDividendWithPositiveTax) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "DIVIDEND",
                        .mAssetClass = "STOCK",
                        .mName = "Synthetic Validation Share",
                        .mSymbol = "XX9000000001",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-dividend-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mTax = "1.00";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "tax");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDividendWithPositiveTax.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-dividend-validation");
}

TEST(TradeRepublicParserTest, RejectsDividendWithMinimumSignedTax) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "DIVIDEND",
                        .mAssetClass = "STOCK",
                        .mName = "Synthetic Validation Share",
                        .mSymbol = "XX9000000001",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-dividend-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mTax = "-922337203685477.5808";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "tax");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDividendWithMinimumSignedTax.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-dividend-validation");
}

TEST(TradeRepublicParserTest, RejectsDividendWithOverflowingTax) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "DIVIDEND",
                        .mAssetClass = "STOCK",
                        .mName = "Synthetic Validation Share",
                        .mSymbol = "XX9000000001",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-dividend-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mTax = "-922337203685477.5809";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "tax");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDividendWithOverflowingTax.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-dividend-validation");
}

TEST(TradeRepublicParserTest, RejectsDividendWithMissingTaxCurrency) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "DIVIDEND",
                        .mAssetClass = "STOCK",
                        .mName = "Synthetic Validation Share",
                        .mSymbol = "XX9000000001",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-dividend-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mCurrency = "";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mField, "currency");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDividendWithMissingTaxCurrency.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-dividend-validation");
}

TEST(TradeRepublicParserTest, RejectsDividendWithUnsupportedTaxCurrency) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "DIVIDEND",
                        .mAssetClass = "STOCK",
                        .mName = "Synthetic Validation Share",
                        .mSymbol = "XX9000000001",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-dividend-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mCurrency = "CAD";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "currency");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDividendWithUnsupportedTaxCurrency.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-dividend-validation");
}

TEST(TradeRepublicParserTest, RejectsDividendWithMissingGrossAmount) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "DIVIDEND",
                        .mAssetClass = "STOCK",
                        .mName = "Synthetic Validation Share",
                        .mSymbol = "XX9000000001",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-dividend-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mAmount = "";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mField, "amount");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDividendWithMissingGrossAmount.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-dividend-validation");
}

TEST(TradeRepublicParserTest, RejectsDividendWithMalformedGrossAmount) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "DIVIDEND",
                        .mAssetClass = "STOCK",
                        .mName = "Synthetic Validation Share",
                        .mSymbol = "XX9000000001",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-dividend-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mAmount = "not-a-number";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "amount");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDividendWithMalformedGrossAmount.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-dividend-validation");
}

TEST(TradeRepublicParserTest, RejectsDividendWithOverflowingGrossAmount) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "DIVIDEND",
                        .mAssetClass = "STOCK",
                        .mName = "Synthetic Validation Share",
                        .mSymbol = "XX9000000001",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-dividend-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mAmount = "922337203685477.5808";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "amount");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDividendWithOverflowingGrossAmount.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-dividend-validation");
}

TEST(TradeRepublicParserTest, RejectsDividendWithUnsupportedOriginalCurrency) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "DIVIDEND",
                        .mAssetClass = "STOCK",
                        .mName = "Synthetic Validation Share",
                        .mSymbol = "XX9000000001",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-dividend-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mOriginalCurrency = "CAD";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "original_currency");
    EXPECT_EQ(
        error.mSourceFile,
        "taxbroker_TradeRepublicParserTest_RejectsDividendWithUnsupportedOriginalCurrency.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-dividend-validation");
}

TEST(TradeRepublicParserTest, RejectsDividendWithMissingOriginalAmount) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "DIVIDEND",
                        .mAssetClass = "STOCK",
                        .mName = "Synthetic Validation Share",
                        .mSymbol = "XX9000000001",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-dividend-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mOriginalAmount = "";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mField, "original_amount");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDividendWithMissingOriginalAmount.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-dividend-validation");
}

TEST(TradeRepublicParserTest, RejectsDividendWithMalformedOriginalAmount) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "DIVIDEND",
                        .mAssetClass = "STOCK",
                        .mName = "Synthetic Validation Share",
                        .mSymbol = "XX9000000001",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-dividend-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mOriginalAmount = "not-a-number";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "original_amount");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDividendWithMalformedOriginalAmount.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-dividend-validation");
}

TEST(TradeRepublicParserTest, RejectsDividendWithZeroExchangeRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "DIVIDEND",
                        .mAssetClass = "STOCK",
                        .mName = "Synthetic Validation Share",
                        .mSymbol = "XX9000000001",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-dividend-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mFxRate = "0";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fx_rate");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDividendWithZeroExchangeRate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-dividend-validation");
}

TEST(TradeRepublicParserTest, RejectsDividendWithNegativeExchangeRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "DIVIDEND",
                        .mAssetClass = "STOCK",
                        .mName = "Synthetic Validation Share",
                        .mSymbol = "XX9000000001",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-dividend-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mFxRate = "-0.90";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fx_rate");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDividendWithNegativeExchangeRate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-dividend-validation");
}

TEST(TradeRepublicParserTest, RejectsDividendWithMalformedExchangeRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "DIVIDEND",
                        .mAssetClass = "STOCK",
                        .mName = "Synthetic Validation Share",
                        .mSymbol = "XX9000000001",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-dividend-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mFxRate = "not-a-number";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fx_rate");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDividendWithMalformedExchangeRate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-dividend-validation");
}

TEST(TradeRepublicParserTest, RejectsDividendWithOverflowingExchangeRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "DIVIDEND",
                        .mAssetClass = "STOCK",
                        .mName = "Synthetic Validation Share",
                        .mSymbol = "XX9000000001",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-dividend-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mFxRate = "92233720368.54775808";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fx_rate");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDividendWithOverflowingExchangeRate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-dividend-validation");
}

TEST(TradeRepublicParserTest, RejectsDividendWithMissingDateWithoutBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "DIVIDEND",
                        .mAssetClass = "STOCK",
                        .mName = "Synthetic Validation Share",
                        .mSymbol = "XX9000000001",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-dividend-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, std::nullopt);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mDate = "";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mField, "date");
    EXPECT_EQ(
        error.mSourceFile,
        "taxbroker_TradeRepublicParserTest_RejectsDividendWithMissingDateWithoutBrokerRate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-dividend-validation");
}

TEST(TradeRepublicParserTest, RejectsDividendWithMissingDatetimeWithoutBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "DIVIDEND",
                        .mAssetClass = "STOCK",
                        .mName = "Synthetic Validation Share",
                        .mSymbol = "XX9000000001",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-dividend-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, std::nullopt);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mDatetime = "";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mField, "datetime");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_"
              "RejectsDividendWithMissingDatetimeWithoutBrokerRate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-dividend-validation");
}

TEST(TradeRepublicParserTest, RejectsDividendWithTinyBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "DIVIDEND",
                        .mAssetClass = "STOCK",
                        .mName = "Synthetic Validation Share",
                        .mSymbol = "XX9000000001",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-dividend-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mFxRate = "0.000000004";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fx_rate");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDividendWithTinyBrokerRate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-dividend-validation");
}

TEST(TradeRepublicParserTest, RejectsDividendWithWhitespaceBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "DIVIDEND",
                        .mAssetClass = "STOCK",
                        .mName = "Synthetic Validation Share",
                        .mSymbol = "XX9000000001",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-dividend-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mFxRate = "   ";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fx_rate");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDividendWithWhitespaceBrokerRate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-dividend-validation");
}

TEST(TradeRepublicParserTest, RejectsDividendWithMalformedDirectForeignBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "DIVIDEND",
                        .mAssetClass = "STOCK",
                        .mName = "Synthetic Validation Share",
                        .mSymbol = "XX9000000001",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "USD",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-dividend-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::USD);

    row.mFxRate = "not-a-number";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fx_rate");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_"
              "RejectsDividendWithMalformedDirectForeignBrokerRate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-dividend-validation");
}

TEST(TradeRepublicParserTest, RejectsDividendWithMalformedEurBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "DIVIDEND",
                        .mAssetClass = "STOCK",
                        .mName = "Synthetic Validation Share",
                        .mSymbol = "XX9000000001",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-dividend-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mFxRate = "not-a-number";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fx_rate");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDividendWithMalformedEurBrokerRate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-dividend-validation");
}

TEST(TradeRepublicParserTest, RejectsDepositInterestWithInvalidDate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "",
                        .mName = "",
                        .mSymbol = "",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-deposit-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mDate = "2024-02-30";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "date");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDepositInterestWithInvalidDate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-deposit-validation");
}

TEST(TradeRepublicParserTest, RejectsDepositInterestWithMalformedTax) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "",
                        .mName = "",
                        .mSymbol = "",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-deposit-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mTax = "not-a-number";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "tax");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDepositInterestWithMalformedTax.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-deposit-validation");
}

TEST(TradeRepublicParserTest, RejectsDepositInterestWithPositiveTax) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "",
                        .mName = "",
                        .mSymbol = "",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-deposit-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mTax = "1.00";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "tax");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDepositInterestWithPositiveTax.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-deposit-validation");
}

TEST(TradeRepublicParserTest, RejectsDepositInterestWithMinimumSignedTax) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "",
                        .mName = "",
                        .mSymbol = "",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-deposit-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mTax = "-922337203685477.5808";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "tax");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDepositInterestWithMinimumSignedTax.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-deposit-validation");
}

TEST(TradeRepublicParserTest, RejectsDepositInterestWithOverflowingTax) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "",
                        .mName = "",
                        .mSymbol = "",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-deposit-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mTax = "-922337203685477.5809";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "tax");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDepositInterestWithOverflowingTax.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-deposit-validation");
}

TEST(TradeRepublicParserTest, RejectsDepositInterestWithMissingTaxCurrency) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "",
                        .mName = "",
                        .mSymbol = "",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-deposit-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mCurrency = "";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mField, "currency");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDepositInterestWithMissingTaxCurrency.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-deposit-validation");
}

TEST(TradeRepublicParserTest, RejectsDepositInterestWithUnsupportedTaxCurrency) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "",
                        .mName = "",
                        .mSymbol = "",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-deposit-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mCurrency = "CAD";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "currency");
    EXPECT_EQ(
        error.mSourceFile,
        "taxbroker_TradeRepublicParserTest_RejectsDepositInterestWithUnsupportedTaxCurrency.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-deposit-validation");
}

TEST(TradeRepublicParserTest, RejectsDepositInterestWithMissingGrossAmount) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "",
                        .mName = "",
                        .mSymbol = "",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-deposit-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mAmount = "";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mField, "amount");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDepositInterestWithMissingGrossAmount.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-deposit-validation");
}

TEST(TradeRepublicParserTest, RejectsDepositInterestWithMalformedGrossAmount) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "",
                        .mName = "",
                        .mSymbol = "",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-deposit-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mAmount = "not-a-number";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "amount");
    EXPECT_EQ(
        error.mSourceFile,
        "taxbroker_TradeRepublicParserTest_RejectsDepositInterestWithMalformedGrossAmount.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-deposit-validation");
}

TEST(TradeRepublicParserTest, RejectsDepositInterestWithOverflowingGrossAmount) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "",
                        .mName = "",
                        .mSymbol = "",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-deposit-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mAmount = "922337203685477.5808";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "amount");
    EXPECT_EQ(
        error.mSourceFile,
        "taxbroker_TradeRepublicParserTest_RejectsDepositInterestWithOverflowingGrossAmount.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-deposit-validation");
}

TEST(TradeRepublicParserTest, RejectsDepositInterestWithUnsupportedOriginalCurrency) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "",
                        .mName = "",
                        .mSymbol = "",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-deposit-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mOriginalCurrency = "CAD";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "original_currency");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_"
              "RejectsDepositInterestWithUnsupportedOriginalCurrency.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-deposit-validation");
}

TEST(TradeRepublicParserTest, RejectsDepositInterestWithMissingOriginalAmount) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "",
                        .mName = "",
                        .mSymbol = "",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-deposit-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mOriginalAmount = "";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mField, "original_amount");
    EXPECT_EQ(
        error.mSourceFile,
        "taxbroker_TradeRepublicParserTest_RejectsDepositInterestWithMissingOriginalAmount.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-deposit-validation");
}

TEST(TradeRepublicParserTest, RejectsDepositInterestWithMalformedOriginalAmount) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "",
                        .mName = "",
                        .mSymbol = "",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-deposit-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mOriginalAmount = "not-a-number";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "original_amount");
    EXPECT_EQ(
        error.mSourceFile,
        "taxbroker_TradeRepublicParserTest_RejectsDepositInterestWithMalformedOriginalAmount.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-deposit-validation");
}

TEST(TradeRepublicParserTest, RejectsDepositInterestWithZeroExchangeRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "",
                        .mName = "",
                        .mSymbol = "",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-deposit-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mFxRate = "0";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fx_rate");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDepositInterestWithZeroExchangeRate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-deposit-validation");
}

TEST(TradeRepublicParserTest, RejectsDepositInterestWithNegativeExchangeRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "",
                        .mName = "",
                        .mSymbol = "",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-deposit-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mFxRate = "-0.90";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fx_rate");
    EXPECT_EQ(
        error.mSourceFile,
        "taxbroker_TradeRepublicParserTest_RejectsDepositInterestWithNegativeExchangeRate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-deposit-validation");
}

TEST(TradeRepublicParserTest, RejectsDepositInterestWithMalformedExchangeRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "",
                        .mName = "",
                        .mSymbol = "",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-deposit-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mFxRate = "not-a-number";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fx_rate");
    EXPECT_EQ(
        error.mSourceFile,
        "taxbroker_TradeRepublicParserTest_RejectsDepositInterestWithMalformedExchangeRate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-deposit-validation");
}

TEST(TradeRepublicParserTest, RejectsDepositInterestWithOverflowingExchangeRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "",
                        .mName = "",
                        .mSymbol = "",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-deposit-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mFxRate = "92233720368.54775808";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fx_rate");
    EXPECT_EQ(
        error.mSourceFile,
        "taxbroker_TradeRepublicParserTest_RejectsDepositInterestWithOverflowingExchangeRate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-deposit-validation");
}

TEST(TradeRepublicParserTest, RejectsDepositInterestWithMissingDateWithoutBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "",
                        .mName = "",
                        .mSymbol = "",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-deposit-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, std::nullopt);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mDate = "";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mField, "date");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_"
              "RejectsDepositInterestWithMissingDateWithoutBrokerRate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-deposit-validation");
}

TEST(TradeRepublicParserTest, RejectsDepositInterestWithMissingDatetimeWithoutBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "",
                        .mName = "",
                        .mSymbol = "",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-deposit-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, std::nullopt);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mDatetime = "";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mField, "datetime");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_"
              "RejectsDepositInterestWithMissingDatetimeWithoutBrokerRate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-deposit-validation");
}

TEST(TradeRepublicParserTest, RejectsDepositInterestWithTinyBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "",
                        .mName = "",
                        .mSymbol = "",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-deposit-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mFxRate = "0.000000004";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fx_rate");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDepositInterestWithTinyBrokerRate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-deposit-validation");
}

TEST(TradeRepublicParserTest, RejectsDepositInterestWithWhitespaceBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "",
                        .mName = "",
                        .mSymbol = "",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-deposit-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mFxRate = "   ";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fx_rate");
    EXPECT_EQ(
        error.mSourceFile,
        "taxbroker_TradeRepublicParserTest_RejectsDepositInterestWithWhitespaceBrokerRate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-deposit-validation");
}

TEST(TradeRepublicParserTest, RejectsDepositInterestWithMalformedDirectForeignBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "",
                        .mName = "",
                        .mSymbol = "",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "USD",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-deposit-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::USD);

    row.mFxRate = "not-a-number";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fx_rate");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_"
              "RejectsDepositInterestWithMalformedDirectForeignBrokerRate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-deposit-validation");
}

TEST(TradeRepublicParserTest, RejectsDepositInterestWithMalformedEurBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "",
                        .mName = "",
                        .mSymbol = "",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-deposit-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mFxRate = "not-a-number";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fx_rate");
    EXPECT_EQ(
        error.mSourceFile,
        "taxbroker_TradeRepublicParserTest_RejectsDepositInterestWithMalformedEurBrokerRate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-deposit-validation");
}

TEST(TradeRepublicParserTest, RejectsBondInterestWithInvalidDate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "BOND",
                        .mName = "Synthetic Validation Bond",
                        .mSymbol = "XX9000000002",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-bond-interest-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mDate = "2024-02-30";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "date");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsBondInterestWithInvalidDate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-bond-interest-validation");
}

TEST(TradeRepublicParserTest, RejectsBondInterestWithMalformedTax) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "BOND",
                        .mName = "Synthetic Validation Bond",
                        .mSymbol = "XX9000000002",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-bond-interest-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mTax = "not-a-number";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "tax");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsBondInterestWithMalformedTax.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-bond-interest-validation");
}

TEST(TradeRepublicParserTest, RejectsBondInterestWithPositiveTax) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "BOND",
                        .mName = "Synthetic Validation Bond",
                        .mSymbol = "XX9000000002",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-bond-interest-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mTax = "1.00";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "tax");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsBondInterestWithPositiveTax.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-bond-interest-validation");
}

TEST(TradeRepublicParserTest, RejectsBondInterestWithMinimumSignedTax) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "BOND",
                        .mName = "Synthetic Validation Bond",
                        .mSymbol = "XX9000000002",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-bond-interest-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mTax = "-922337203685477.5808";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "tax");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsBondInterestWithMinimumSignedTax.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-bond-interest-validation");
}

TEST(TradeRepublicParserTest, RejectsBondInterestWithOverflowingTax) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "BOND",
                        .mName = "Synthetic Validation Bond",
                        .mSymbol = "XX9000000002",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-bond-interest-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mTax = "-922337203685477.5809";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "tax");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsBondInterestWithOverflowingTax.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-bond-interest-validation");
}

TEST(TradeRepublicParserTest, RejectsBondInterestWithMissingTaxCurrency) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "BOND",
                        .mName = "Synthetic Validation Bond",
                        .mSymbol = "XX9000000002",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-bond-interest-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mCurrency = "";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mField, "currency");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsBondInterestWithMissingTaxCurrency.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-bond-interest-validation");
}

TEST(TradeRepublicParserTest, RejectsBondInterestWithUnsupportedTaxCurrency) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "BOND",
                        .mName = "Synthetic Validation Bond",
                        .mSymbol = "XX9000000002",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-bond-interest-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mCurrency = "CAD";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "currency");
    EXPECT_EQ(
        error.mSourceFile,
        "taxbroker_TradeRepublicParserTest_RejectsBondInterestWithUnsupportedTaxCurrency.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-bond-interest-validation");
}

TEST(TradeRepublicParserTest, RejectsBondInterestWithMissingGrossAmount) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "BOND",
                        .mName = "Synthetic Validation Bond",
                        .mSymbol = "XX9000000002",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-bond-interest-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mAmount = "";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mField, "amount");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsBondInterestWithMissingGrossAmount.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-bond-interest-validation");
}

TEST(TradeRepublicParserTest, RejectsBondInterestWithMalformedGrossAmount) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "BOND",
                        .mName = "Synthetic Validation Bond",
                        .mSymbol = "XX9000000002",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-bond-interest-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mAmount = "not-a-number";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "amount");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsBondInterestWithMalformedGrossAmount.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-bond-interest-validation");
}

TEST(TradeRepublicParserTest, RejectsBondInterestWithOverflowingGrossAmount) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "BOND",
                        .mName = "Synthetic Validation Bond",
                        .mSymbol = "XX9000000002",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-bond-interest-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mAmount = "922337203685477.5808";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "amount");
    EXPECT_EQ(
        error.mSourceFile,
        "taxbroker_TradeRepublicParserTest_RejectsBondInterestWithOverflowingGrossAmount.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-bond-interest-validation");
}

TEST(TradeRepublicParserTest, RejectsBondInterestWithUnsupportedOriginalCurrency) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "BOND",
                        .mName = "Synthetic Validation Bond",
                        .mSymbol = "XX9000000002",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-bond-interest-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mOriginalCurrency = "CAD";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "original_currency");
    EXPECT_EQ(
        error.mSourceFile,
        "taxbroker_TradeRepublicParserTest_RejectsBondInterestWithUnsupportedOriginalCurrency.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-bond-interest-validation");
}

TEST(TradeRepublicParserTest, RejectsBondInterestWithMissingOriginalAmount) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "BOND",
                        .mName = "Synthetic Validation Bond",
                        .mSymbol = "XX9000000002",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-bond-interest-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mOriginalAmount = "";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mField, "original_amount");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsBondInterestWithMissingOriginalAmount.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-bond-interest-validation");
}

TEST(TradeRepublicParserTest, RejectsBondInterestWithMalformedOriginalAmount) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "BOND",
                        .mName = "Synthetic Validation Bond",
                        .mSymbol = "XX9000000002",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-bond-interest-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mOriginalAmount = "not-a-number";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "original_amount");
    EXPECT_EQ(
        error.mSourceFile,
        "taxbroker_TradeRepublicParserTest_RejectsBondInterestWithMalformedOriginalAmount.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-bond-interest-validation");
}

TEST(TradeRepublicParserTest, RejectsBondInterestWithZeroExchangeRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "BOND",
                        .mName = "Synthetic Validation Bond",
                        .mSymbol = "XX9000000002",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-bond-interest-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mFxRate = "0";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fx_rate");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsBondInterestWithZeroExchangeRate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-bond-interest-validation");
}

TEST(TradeRepublicParserTest, RejectsBondInterestWithNegativeExchangeRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "BOND",
                        .mName = "Synthetic Validation Bond",
                        .mSymbol = "XX9000000002",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-bond-interest-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mFxRate = "-0.90";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fx_rate");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsBondInterestWithNegativeExchangeRate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-bond-interest-validation");
}

TEST(TradeRepublicParserTest, RejectsBondInterestWithMalformedExchangeRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "BOND",
                        .mName = "Synthetic Validation Bond",
                        .mSymbol = "XX9000000002",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-bond-interest-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mFxRate = "not-a-number";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fx_rate");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsBondInterestWithMalformedExchangeRate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-bond-interest-validation");
}

TEST(TradeRepublicParserTest, RejectsBondInterestWithOverflowingExchangeRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "BOND",
                        .mName = "Synthetic Validation Bond",
                        .mSymbol = "XX9000000002",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-bond-interest-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mFxRate = "92233720368.54775808";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fx_rate");
    EXPECT_EQ(
        error.mSourceFile,
        "taxbroker_TradeRepublicParserTest_RejectsBondInterestWithOverflowingExchangeRate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-bond-interest-validation");
}

TEST(TradeRepublicParserTest, RejectsBondInterestWithMissingDateWithoutBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "BOND",
                        .mName = "Synthetic Validation Bond",
                        .mSymbol = "XX9000000002",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-bond-interest-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, std::nullopt);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mDate = "";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mField, "date");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_"
              "RejectsBondInterestWithMissingDateWithoutBrokerRate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-bond-interest-validation");
}

TEST(TradeRepublicParserTest, RejectsBondInterestWithMissingDatetimeWithoutBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "BOND",
                        .mName = "Synthetic Validation Bond",
                        .mSymbol = "XX9000000002",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-bond-interest-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, std::nullopt);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mDatetime = "";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mField, "datetime");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_"
              "RejectsBondInterestWithMissingDatetimeWithoutBrokerRate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-bond-interest-validation");
}

TEST(TradeRepublicParserTest, RejectsBondInterestWithTinyBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "BOND",
                        .mName = "Synthetic Validation Bond",
                        .mSymbol = "XX9000000002",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-bond-interest-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mFxRate = "0.000000004";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fx_rate");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsBondInterestWithTinyBrokerRate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-bond-interest-validation");
}

TEST(TradeRepublicParserTest, RejectsBondInterestWithWhitespaceBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "BOND",
                        .mName = "Synthetic Validation Bond",
                        .mSymbol = "XX9000000002",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-bond-interest-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mFxRate = "   ";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fx_rate");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsBondInterestWithWhitespaceBrokerRate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-bond-interest-validation");
}

TEST(TradeRepublicParserTest, RejectsBondInterestWithMalformedDirectForeignBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "BOND",
                        .mName = "Synthetic Validation Bond",
                        .mSymbol = "XX9000000002",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "USD",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-bond-interest-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::USD);

    row.mFxRate = "not-a-number";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fx_rate");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_"
              "RejectsBondInterestWithMalformedDirectForeignBrokerRate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-bond-interest-validation");
}

TEST(TradeRepublicParserTest, RejectsBondInterestWithMalformedEurBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "BOND",
                        .mName = "Synthetic Validation Bond",
                        .mSymbol = "XX9000000002",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-bond-interest-validation"};

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);

    row.mFxRate = "not-a-number";

    const auto rejected = parseRows({row});

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fx_rate");
    EXPECT_EQ(
        error.mSourceFile,
        "taxbroker_TradeRepublicParserTest_RejectsBondInterestWithMalformedEurBrokerRate.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-bond-interest-validation");
}

TEST(TradeRepublicParserTest, PreservesGrossDividendWithBlankTax) {
    auto row = makeDividendRow();
    row.mAmount = "12.34";
    row.mTax = "";

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);
    const auto& income = result.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 123'400);
    EXPECT_FALSE(income.mTaxPaid.has_value());
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);
    EXPECT_EQ(income.mExchangeRate, EXCHANGE_RATE_SCALE);
}

TEST(TradeRepublicParserTest, PreservesGrossDividendWithZeroTax) {
    auto row = makeDividendRow();
    row.mAmount = "12.34";
    row.mTax = "0.00";

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);
    const auto& income = result.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 123'400);
    EXPECT_EQ(income.mTaxPaid, 0);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);
    EXPECT_EQ(income.mExchangeRate, EXCHANGE_RATE_SCALE);
}

TEST(TradeRepublicParserTest, PreservesGrossDividendWithWithholding) {
    auto row = makeDividendRow();
    row.mAmount = "12.34";
    row.mTax = "-2.34";

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);
    const auto& income = result.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 123'400);
    EXPECT_EQ(income.mTaxPaid, 23'400);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);
    EXPECT_EQ(income.mExchangeRate, EXCHANGE_RATE_SCALE);
}

TEST(TradeRepublicParserTest, RetainsForeignGrossDividendWithoutBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "DIVIDEND",
                        .mAssetClass = "STOCK",
                        .mName = "Synthetic Validation Share",
                        .mSymbol = "XX9000000001",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "9.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "10.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-dividend-validation"};

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& income = result.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, std::nullopt);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);
    EXPECT_EQ(income.mMetadata.mSourceTimestamp,
              SourceTimestamp{makeDate(2024, 1, 15).time_since_epoch() + std::chrono::hours{10}});
    EXPECT_FALSE(income.mMetadata.mOrderingTimestamp.has_value());
    EXPECT_EQ(
        income.mMetadata.mSources,
        (std::vector<SourceReference>{{.mBroker = Broker::TradeRepublic,
                                       .mFilename = SourceFilename::fromPath(
                                           "taxbroker_TradeRepublicParserTest_"
                                           "RetainsForeignGrossDividendWithoutBrokerRate.csv"),
                                       .mSourceRow = 2,
                                       .mTransactionId = "synthetic-dividend-validation",
                                       .mInputSequence = {0, 0}}}));
}

TEST(TradeRepublicParserTest, PreservesGrossDepositInterestWithBlankTax) {
    auto row = makeBrokerInterestRow();
    row.mAmount = "12.34";
    row.mTax = "";

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);
    const auto& income = result.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 123'400);
    EXPECT_FALSE(income.mTaxPaid.has_value());
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);
    EXPECT_EQ(income.mExchangeRate, EXCHANGE_RATE_SCALE);
}

TEST(TradeRepublicParserTest, PreservesGrossDepositInterestWithZeroTax) {
    auto row = makeBrokerInterestRow();
    row.mAmount = "12.34";
    row.mTax = "0.00";

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);
    const auto& income = result.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 123'400);
    EXPECT_EQ(income.mTaxPaid, 0);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);
    EXPECT_EQ(income.mExchangeRate, EXCHANGE_RATE_SCALE);
}

TEST(TradeRepublicParserTest, PreservesGrossDepositInterestWithWithholding) {
    auto row = makeBrokerInterestRow();
    row.mAmount = "12.34";
    row.mTax = "-2.34";

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);
    const auto& income = result.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 123'400);
    EXPECT_EQ(income.mTaxPaid, 23'400);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);
    EXPECT_EQ(income.mExchangeRate, EXCHANGE_RATE_SCALE);
}

TEST(TradeRepublicParserTest, RetainsForeignGrossDepositInterestWithoutBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "",
                        .mName = "",
                        .mSymbol = "",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "9.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "10.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-deposit-validation"};

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = result.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, std::nullopt);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);
    EXPECT_EQ(income.mMetadata.mSourceTimestamp,
              SourceTimestamp{makeDate(2024, 1, 15).time_since_epoch() + std::chrono::hours{10}});
    EXPECT_FALSE(income.mMetadata.mOrderingTimestamp.has_value());
    EXPECT_EQ(income.mMetadata.mSources,
              (std::vector<SourceReference>{
                  {.mBroker = Broker::TradeRepublic,
                   .mFilename = SourceFilename::fromPath(
                       "taxbroker_TradeRepublicParserTest_"
                       "RetainsForeignGrossDepositInterestWithoutBrokerRate.csv"),
                   .mSourceRow = 2,
                   .mTransactionId = "synthetic-deposit-validation",
                   .mInputSequence = {0, 0}}}));
}

TEST(TradeRepublicParserTest, PreservesGrossBondInterestWithBlankTax) {
    auto row = makeBondInterestRow();
    row.mAmount = "12.34";
    row.mTax = "";

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);
    const auto& income = result.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 123'400);
    EXPECT_FALSE(income.mTaxPaid.has_value());
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);
    EXPECT_EQ(income.mExchangeRate, EXCHANGE_RATE_SCALE);
}

TEST(TradeRepublicParserTest, PreservesGrossBondInterestWithZeroTax) {
    auto row = makeBondInterestRow();
    row.mAmount = "12.34";
    row.mTax = "0.00";

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);
    const auto& income = result.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 123'400);
    EXPECT_EQ(income.mTaxPaid, 0);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);
    EXPECT_EQ(income.mExchangeRate, EXCHANGE_RATE_SCALE);
}

TEST(TradeRepublicParserTest, PreservesGrossBondInterestWithWithholding) {
    auto row = makeBondInterestRow();
    row.mAmount = "12.34";
    row.mTax = "-2.34";

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);
    const auto& income = result.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 123'400);
    EXPECT_EQ(income.mTaxPaid, 23'400);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);
    EXPECT_EQ(income.mExchangeRate, EXCHANGE_RATE_SCALE);
}

TEST(TradeRepublicParserTest, RetainsForeignGrossBondInterestWithoutBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "BOND",
                        .mName = "Synthetic Validation Bond",
                        .mSymbol = "XX9000000002",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "9.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "10.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-bond-interest-validation"};

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = result.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, std::nullopt);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);
    EXPECT_EQ(income.mMetadata.mSourceTimestamp,
              SourceTimestamp{makeDate(2024, 1, 15).time_since_epoch() + std::chrono::hours{10}});
    EXPECT_FALSE(income.mMetadata.mOrderingTimestamp.has_value());
    EXPECT_EQ(
        income.mMetadata.mSources,
        (std::vector<SourceReference>{{.mBroker = Broker::TradeRepublic,
                                       .mFilename = SourceFilename::fromPath(
                                           "taxbroker_TradeRepublicParserTest_"
                                           "RetainsForeignGrossBondInterestWithoutBrokerRate.csv"),
                                       .mSourceRow = 2,
                                       .mTransactionId = "synthetic-bond-interest-validation",
                                       .mInputSequence = {0, 0}}}));
}

TEST(TradeRepublicParserTest, PreservesNativeCouponIdentitySeparatelyFromCashInterest) {
    const auto bond = makeBondInterestRow();
    const auto cash = makeBrokerInterestRow();
    const auto result = parseRows({bond, cash});
    const auto* instrument = findInterestInstrument(result.mStatement, InterestType::BondInterest);

    ASSERT_NE(instrument, nullptr);
    EXPECT_EQ(instrument->mName, bond.mName);
    EXPECT_EQ(instrument->mIsin, bond.mSymbol);
    ASSERT_EQ(instrument->mTransactions.size(), 1U);
    EXPECT_EQ(instrument->mTransactions[0].mGrossAmount, 100'000);
    EXPECT_EQ(instrument->mTransactions[0].mTaxPaid, 10'000);
    EXPECT_EQ(transactionId(instrument->mTransactions[0].mMetadata), bond.mTransactionId);
    ASSERT_EQ(result.mStatement.mInterestInstruments.size(), 2U);
    const auto* deposit = findInterestInstrument(result.mStatement, InterestType::BrokerInterest);

    ASSERT_NE(deposit, nullptr);
    EXPECT_EQ(deposit->mName, "Trade Republic");
    EXPECT_FALSE(deposit->mIsin.has_value());
    ASSERT_EQ(deposit->mTransactions.size(), 1U);
    EXPECT_EQ(transactionId(deposit->mTransactions[0].mMetadata), cash.mTransactionId);
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(TradeRepublicParserTest, DoesNotTreatUnverifiedInstrumentInterestAsBankInterest) {
    SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "BOND",
        .mName = "Synthetic Validation Bond",
        .mSymbol = "XX9000000002",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-validation",
    };

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& transaction =
        accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transactionId(transaction.mMetadata), "synthetic-validation");
    EXPECT_EQ(transaction.mGrossAmount, 100'000);
    EXPECT_EQ(transaction.mTaxPaid, 10'000);
    EXPECT_EQ(transaction.mCurrency, Currency::EUR);
    EXPECT_EQ(transaction.mTaxCurrency, Currency::EUR);

    row.mAssetClass = "STOCK";
    const TemporaryCsvFile csvFile{{row}};
    tr::TradeRepublicParser parser;

    const auto rejected = parser.parse(csvFile.path());

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::UnsupportedAssetClass);
    EXPECT_EQ(error.mMessageKey, MessageKey::UnsupportedAssetClassInterestMapping);
    EXPECT_TRUE(error.mMessageParams.empty());
    EXPECT_EQ(error.mField, "asset_class");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-validation");
}

TEST(TradeRepublicParserTest, RejectsTradeAndReportsErrorWhenDatetimeIsMissing) {
    SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.00",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-validation",
    };

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mTradeInstruments.front().mTransactions.size(), 1U);

    const auto& transaction = accepted.mStatement.mTradeInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transactionId(transaction.mMetadata), "synthetic-validation");
    EXPECT_EQ(transaction.mUnits, 100'000'000);
    EXPECT_EQ(transaction.mUnitPrice, 1'000'000'000);
    EXPECT_EQ(transaction.mAmount, 100'000);
    EXPECT_EQ(transaction.mCurrency, Currency::EUR);

    row.mDatetime.clear();
    const TemporaryCsvFile csvFile{{row}};
    tr::TradeRepublicParser parser;

    const auto rejected = parser.parse(csvFile.path());

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mField, "datetime");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-validation");
}

TEST(TradeRepublicParserTest, RejectsDividendAndReportsErrorWhenDatetimeIsMissing) {
    SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "CASH",
        .mType = "DIVIDEND",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-validation",
    };

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& transaction =
        accepted.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transactionId(transaction.mMetadata), "synthetic-validation");
    EXPECT_EQ(transaction.mGrossAmount, 100'000);
    EXPECT_EQ(transaction.mTaxPaid, 10'000);
    EXPECT_EQ(transaction.mCurrency, Currency::EUR);
    EXPECT_EQ(transaction.mTaxCurrency, Currency::EUR);

    row.mDatetime.clear();
    const TemporaryCsvFile csvFile{{row}};
    tr::TradeRepublicParser parser;

    const auto rejected = parser.parse(csvFile.path());

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mField, "datetime");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-validation");
}

TEST(TradeRepublicParserTest, RejectsDepositInterestAndReportsErrorWhenDatetimeIsMissing) {
    SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "",
        .mName = "",
        .mSymbol = "",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-validation",
    };

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& transaction =
        accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transactionId(transaction.mMetadata), "synthetic-validation");
    EXPECT_EQ(transaction.mGrossAmount, 100'000);
    EXPECT_EQ(transaction.mTaxPaid, 10'000);
    EXPECT_EQ(transaction.mCurrency, Currency::EUR);
    EXPECT_EQ(transaction.mTaxCurrency, Currency::EUR);

    row.mDatetime.clear();
    const TemporaryCsvFile csvFile{{row}};
    tr::TradeRepublicParser parser;

    const auto rejected = parser.parse(csvFile.path());

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mField, "datetime");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-validation");
}

TEST(TradeRepublicParserTest, RejectsBondInterestAndReportsErrorWhenDatetimeIsMissing) {
    SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "BOND",
        .mName = "Synthetic Validation Bond",
        .mSymbol = "XX9000000002",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-validation",
    };

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& transaction =
        accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transactionId(transaction.mMetadata), "synthetic-validation");
    EXPECT_EQ(transaction.mGrossAmount, 100'000);
    EXPECT_EQ(transaction.mTaxPaid, 10'000);
    EXPECT_EQ(transaction.mCurrency, Currency::EUR);
    EXPECT_EQ(transaction.mTaxCurrency, Currency::EUR);

    row.mDatetime.clear();
    const TemporaryCsvFile csvFile{{row}};
    tr::TradeRepublicParser parser;

    const auto rejected = parser.parse(csvFile.path());

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mField, "datetime");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-validation");
}

TEST(TradeRepublicParserTest, RejectsCorporateActionAndReportsErrorWhenDatetimeIsMissing) {
    auto row = makeCorporateActionRow();
    row.mDatetime.clear();

    const auto result = parseRows({row});

    EXPECT_EQ(result.mStatement.mTradeInstruments.size(), 0U);
    EXPECT_EQ(result.mStatement.mDividendInstruments.size(), 0U);
    EXPECT_EQ(result.mStatement.mInterestInstruments.size(), 0U);
    EXPECT_EQ(result.mStatement.mBenefitEvents.size(), 0U);
    EXPECT_EQ(result.mStatement.mPrivateMarketEvents.size(), 0U);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    const auto& error = result.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mField, "datetime");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, row.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsBenefitAndReportsErrorWhenDatetimeIsMissing) {
    auto row = makeBenefitRow();
    row.mDatetime.clear();

    const auto result = parseRows({row});

    EXPECT_EQ(result.mStatement.mTradeInstruments.size(), 0U);
    EXPECT_EQ(result.mStatement.mDividendInstruments.size(), 0U);
    EXPECT_EQ(result.mStatement.mInterestInstruments.size(), 0U);
    EXPECT_EQ(result.mStatement.mBenefitEvents.size(), 0U);
    EXPECT_EQ(result.mStatement.mPrivateMarketEvents.size(), 0U);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    const auto& error = result.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mMessageKey, MessageKey::MissingFieldDatetime);
    EXPECT_TRUE(error.mMessageParams.empty());
    EXPECT_EQ(error.mField, "datetime");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, row.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsPrivateMarketAndReportsErrorWhenDatetimeIsMissing) {
    auto row = makePrivateMarketRow();
    row.mDatetime.clear();

    const auto result = parseRows({row});

    EXPECT_EQ(result.mStatement.mTradeInstruments.size(), 0U);
    EXPECT_EQ(result.mStatement.mDividendInstruments.size(), 0U);
    EXPECT_EQ(result.mStatement.mInterestInstruments.size(), 0U);
    EXPECT_EQ(result.mStatement.mBenefitEvents.size(), 0U);
    EXPECT_EQ(result.mStatement.mPrivateMarketEvents.size(), 0U);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    const auto& error = result.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mField, "datetime");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, row.mTransactionId);
}

TEST(TradeRepublicParserTest, ReportsUnverifiedCreditInsteadOfSilentlyIgnoringIt) {
    SyntheticCsvRow credit;
    credit.mCategory = "CASH";
    credit.mType = "CREDIT";
    credit.mTransactionId = "synthetic-unverified-credit";
    const SyntheticCsvRow healthy;
    const auto result = parseRows({credit, healthy});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics[0].mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(result.mDiagnostics[0].mCode, DiagnosticCode::UnknownRowType);
    EXPECT_EQ(result.mDiagnostics[0].mField, "type");
    EXPECT_EQ(result.mDiagnostics[0].mTransactionId, credit.mTransactionId);
    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);
    EXPECT_EQ(transactionId(result.mStatement.mTradeInstruments[0].mTransactions[0].mMetadata),
              healthy.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsDividendWithMissingIdentity) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "DIVIDEND",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-dividend-validation",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mDividendInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mDividendInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mGrossAmount, 100'000); // EUR 10 at four decimal places.
    EXPECT_EQ(accepted.mTaxPaid, 10'000);      // EUR 1 withholding at four decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "DIVIDEND",
        .mAssetClass = "STOCK",
        .mName = "",
        .mSymbol = "",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-dividend-validation",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_FALSE(error.mField.has_value());
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsDividendWithMissingIsin) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "DIVIDEND",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-dividend-validation",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mDividendInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mDividendInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mGrossAmount, 100'000); // EUR 10 at four decimal places.
    EXPECT_EQ(accepted.mTaxPaid, 10'000);      // EUR 1 withholding at four decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "DIVIDEND",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-dividend-validation",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mField, "symbol");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsBondInterestWithMissingIdentity) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "BOND",
        .mName = "Synthetic Validation Bond",
        .mSymbol = "XX9000000002",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-bond-interest-validation",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mInterestInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mInterestInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mGrossAmount, 100'000); // EUR 10 at four decimal places.
    EXPECT_EQ(accepted.mTaxPaid, 10'000);      // EUR 1 withholding at four decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "BOND",
        .mName = "",
        .mSymbol = "",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-bond-interest-validation",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_FALSE(error.mField.has_value());
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsBondInterestWithMissingIsin) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "BOND",
        .mName = "Synthetic Validation Bond",
        .mSymbol = "XX9000000002",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-bond-interest-validation",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mInterestInstruments[0].mTransactions.size(), 1U);

    const auto& accepted = baseline.mStatement.mInterestInstruments[0].mTransactions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mGrossAmount, 100'000); // EUR 10 at four decimal places.
    EXPECT_EQ(accepted.mTaxPaid, 10'000);      // EUR 1 withholding at four decimal places.

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "BOND",
        .mName = "Synthetic Validation Bond",
        .mSymbol = "",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-bond-interest-validation",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mField, "symbol");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsActionWithMissingIdentity) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.00",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-validation",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mCorporateActions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mCorporateActions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mType, CorporateActionType::UnresolvedSplit);
    EXPECT_EQ(accepted.mUnitsDelta, 100'000'000); // Source 1 unit, not a verified split change.
    EXPECT_FALSE(accepted.mRatio.has_value());

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "",
        .mSymbol = "",
        .mShares = "1.00",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-validation",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mMessageKey, MessageKey::MissingFieldSymbolAndName);
    EXPECT_EQ(error.mMessageParams,
              (std::vector<MessageParam>{termParam("rowKind", term::kRowKindCorporateAction)}));
    EXPECT_FALSE(error.mField.has_value());
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsActionWithMissingIsin) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.00",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-validation",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mCorporateActions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mCorporateActions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mType, CorporateActionType::UnresolvedSplit);
    EXPECT_EQ(accepted.mUnitsDelta, 100'000'000); // Source 1 unit, not a verified split change.
    EXPECT_FALSE(accepted.mRatio.has_value());

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "",
        .mShares = "1.00",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-validation",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mField, "symbol");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsActionWithInvalidDate) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.00",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-validation",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mCorporateActions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mCorporateActions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mType, CorporateActionType::UnresolvedSplit);
    EXPECT_EQ(accepted.mUnitsDelta, 100'000'000); // Source 1 unit, not a verified split change.
    EXPECT_FALSE(accepted.mRatio.has_value());

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-13-01",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.00",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-validation",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "date");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsActionWithMissingUnits) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.00",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-validation",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mCorporateActions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mCorporateActions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mType, CorporateActionType::UnresolvedSplit);
    EXPECT_EQ(accepted.mUnitsDelta, 100'000'000); // Source 1 unit, not a verified split change.
    EXPECT_FALSE(accepted.mRatio.has_value());

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-validation",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mField, "shares");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsActionWithMalformedUnits) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.00",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-validation",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mCorporateActions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mCorporateActions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mType, CorporateActionType::UnresolvedSplit);
    EXPECT_EQ(accepted.mUnitsDelta, 100'000'000); // Source 1 unit, not a verified split change.
    EXPECT_FALSE(accepted.mRatio.has_value());

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "not-a-number",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-validation",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "shares");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsActionWithOverflowingUnits) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.00",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-validation",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mCorporateActions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mCorporateActions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mType, CorporateActionType::UnresolvedSplit);
    EXPECT_EQ(accepted.mUnitsDelta, 100'000'000); // Source 1 unit, not a verified split change.
    EXPECT_FALSE(accepted.mRatio.has_value());

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "92233720368.54775808",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-validation",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "shares");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RejectsActionWithZeroUnits) {
    const SyntheticCsvRow validRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.00",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-validation",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const auto baseline = parseRows({validRow});

    ASSERT_TRUE(baseline.mDiagnostics.empty());
    ASSERT_EQ(baseline.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(baseline.mStatement.mTradeInstruments[0].mCorporateActions.size(), 1U);

    const auto& accepted = baseline.mStatement.mTradeInstruments[0].mCorporateActions[0];

    EXPECT_EQ(accepted.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(primarySource(accepted.mMetadata).mTransactionId, validRow.mTransactionId);
    EXPECT_EQ(accepted.mType, CorporateActionType::UnresolvedSplit);
    EXPECT_EQ(accepted.mUnitsDelta, 100'000'000); // Source 1 unit, not a verified split change.
    EXPECT_FALSE(accepted.mRatio.has_value());

    const SyntheticCsvRow invalidRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "0",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-validation",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{invalidRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics[0];

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "shares");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, invalidRow.mTransactionId);
}

TEST(TradeRepublicParserTest, RetainsActionClassificationConflictForTheSameIsin) {
    const SyntheticCsvRow stock{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const SyntheticCsvRow fund{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "FUND",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.00",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-validation",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{stock, fund};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mTradeInstruments[0];

    EXPECT_EQ(instrument.mAssetClass, AssetClass::Unknown);
    ASSERT_EQ(instrument.mTransactions.size(), 1U);
    ASSERT_EQ(instrument.mCorporateActions.size(), 1U);

    const auto& purchase = instrument.mTransactions[0];
    const auto& other = instrument.mCorporateActions[0];

    EXPECT_EQ(purchase.mUnits, 100'000'000);       // Source 1 unit at eight decimal places.
    EXPECT_EQ(purchase.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.
    EXPECT_EQ(primarySource(purchase.mMetadata).mTransactionId, "synthetic-test-transaction");
    ASSERT_EQ(purchase.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mSource.mSourceRow, 2U);
    ASSERT_EQ(other.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(other.mInstrumentEvidence[0].mAssetClass, AssetClass::Fund);
    EXPECT_EQ(other.mInstrumentEvidence[0].mAssetClassLabel, "FUND");
    EXPECT_EQ(other.mInstrumentEvidence[0].mSource.mSourceRow, 3U);
    EXPECT_EQ(primarySource(other.mMetadata).mTransactionId, "synthetic-split-validation");
    EXPECT_EQ(other.mUnitsDelta, 100'000'000);
    EXPECT_EQ(other.mType, CorporateActionType::UnresolvedSplit);
    EXPECT_FALSE(other.mRatio.has_value());

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics[0].mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(result.mDiagnostics[0].mCode, DiagnosticCode::InconsistentValue);
    EXPECT_EQ(result.mDiagnostics[0].mField, "asset_class");
    EXPECT_EQ(result.mDiagnostics[0].mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(result.mDiagnostics[0].mRowIndex, 3U);
    EXPECT_EQ(result.mDiagnostics[0].mTransactionId, fund.mTransactionId);
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
        ASSERT_EQ(parseResult.mDiagnostics.size(), 1U);
        EXPECT_EQ(parseResult.mDiagnostics.front().mSeverity, DiagnosticSeverity::Error);
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
        ASSERT_EQ(parseResult.mDiagnostics.size(), 1U);
        EXPECT_EQ(parseResult.mDiagnostics.front().mSeverity, DiagnosticSeverity::Error);
    }
}

TEST(TradeRepublicParserTest, IgnoresTheConfiguredCardAndTransferTransactions) {
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
        EXPECT_TRUE(parseResult.mDiagnostics.empty());
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
        ASSERT_EQ(parseResult.mDiagnostics.size(), 1U);
        EXPECT_EQ(parseResult.mDiagnostics.front().mSeverity, DiagnosticSeverity::Error);
        EXPECT_EQ(parseResult.mDiagnostics.front().mCode, DiagnosticCode::UnsupportedRowType);
        EXPECT_EQ(parseResult.mDiagnostics.front().mMessageKey,
                  MessageKey::UnsupportedRowTypeRecognized);
        EXPECT_EQ(parseResult.mDiagnostics.front().mMessageParams,
                  (std::vector<MessageParam>{
                      textParam("type", std::string{unsupportedType.mType}),
                      textParam("category", std::string{unsupportedType.mCategory})}));
        EXPECT_EQ(parseResult.mDiagnostics.front().mRowIndex, 2U);
        EXPECT_NE(parseResult.mDiagnostics.front().mMessage.find(unsupportedType.mType),
                  std::string::npos);
        ASSERT_TRUE(parseResult.mDiagnostics.front().mTransactionId.has_value());
        EXPECT_EQ(*parseResult.mDiagnostics.front().mTransactionId, row.mTransactionId);
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
        ASSERT_EQ(parseResult.mDiagnostics.size(), 1U);
        EXPECT_EQ(parseResult.mDiagnostics.front().mSeverity, DiagnosticSeverity::Error);
        EXPECT_EQ(parseResult.mDiagnostics.front().mCode, DiagnosticCode::UnknownRowType);
        EXPECT_EQ(parseResult.mDiagnostics.front().mRowIndex, 2U);
        EXPECT_NE(parseResult.mDiagnostics.front().mMessage.find(type), std::string::npos);
        ASSERT_TRUE(parseResult.mDiagnostics.front().mTransactionId.has_value());
        EXPECT_EQ(*parseResult.mDiagnostics.front().mTransactionId, row.mTransactionId);
    }
}

TEST(TradeRepublicParserTest, KeepsSignedSourceUnitsAndDiscardedDigitsForReconciliation) {
    const SyntheticCsvRow buy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "0.12345678400",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mCurrency = "EUR",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
    };
    const SyntheticCsvRow sell{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "SELL",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "-0.123456785",
        .mPrice = "10.000000",
        .mAmount = "1.00",
        .mCurrency = "EUR",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-evidence-sell",
    };
    const SyntheticCsvRow split{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "0.123456785",
        .mPrice = "",
        .mAmount = "",
        .mCurrency = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-evidence-action",
    };

    const auto result = parseRows({buy, sell, split});

    EXPECT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mTradeInstruments.front();

    ASSERT_EQ(instrument.mTransactions.size(), 2U);

    const auto& purchase = instrument.mTransactions[0];

    ASSERT_EQ(purchase.mUnitEvidence.size(), 1U);

    const auto& buyEvidence = purchase.mUnitEvidence.front();

    EXPECT_EQ(buyEvidence.mSourceText, "0.12345678400");
    EXPECT_EQ(buyEvidence.mDiscardedDigits, "400");
    EXPECT_FALSE(buyEvidence.mRoundedAwayFromZero);

    const auto& sale = instrument.mTransactions[1];

    ASSERT_EQ(sale.mUnitEvidence.size(), 1U);

    const auto& sellEvidence = sale.mUnitEvidence.front();

    EXPECT_EQ(sellEvidence.mSourceText, "-0.123456785");
    EXPECT_EQ(sellEvidence.mCanonicalValue, "-0.123456785");
    EXPECT_EQ(sellEvidence.mDiscardedDigits, "5");
    EXPECT_TRUE(sellEvidence.mRoundedAwayFromZero);
    // The signed source rounds to -0.12345679; a sale stores its positive magnitude.
    EXPECT_EQ(sale.mUnits, 12'345'679);
    EXPECT_EQ(sellEvidence.mSource, sale.mMetadata.mSources.at(0));

    ASSERT_EQ(instrument.mCorporateActions.size(), 1U);

    const auto& action = instrument.mCorporateActions.front();

    ASSERT_EQ(action.mUnitEvidence.size(), 1U);
    EXPECT_EQ(action.mUnitEvidence.front().mSourceText, "0.123456785");
    EXPECT_FALSE(action.mRatio.has_value());
}

TEST(TradeRepublicParserTest, RejectsTradePriceBelowHalfStep) {
    const SyntheticCsvRow acceptedRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.00",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-validation",
    };

    const auto accepted = parseRows({acceptedRow});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mTradeInstruments.front().mTransactions.size(), 1U);

    const auto& transaction = accepted.mStatement.mTradeInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transactionId(transaction.mMetadata), "synthetic-validation");
    EXPECT_EQ(transaction.mUnits, 100'000'000);
    EXPECT_EQ(transaction.mUnitPrice, 1'000'000'000);
    EXPECT_EQ(transaction.mAmount, 100'000);
    EXPECT_EQ(transaction.mCurrency, Currency::EUR);

    const SyntheticCsvRow rejectedRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "0.000000004", // Below half of the eight-decimal step 0.00000001.
        .mAmount = "-10.00",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-validation",
    };
    const TemporaryCsvFile csvFile{{rejectedRow}};
    tr::TradeRepublicParser parser;

    const auto rejected = parser.parse(csvFile.path());

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "price");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-validation");
    EXPECT_EQ(error.mMessage.find("0.000"), std::string::npos);
}

TEST(TradeRepublicParserTest, ImportsTradePriceWithEightDecimals) {
    // The old four-decimal price would be 10.0001.
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.00005",
        .mAmount = "-10.00",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-price-5-decimals",
    };

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mTradeInstruments.front().mTransactions.size(), 1U);

    const auto& transaction = result.mStatement.mTradeInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mUnitPrice, 1'000'005'000); // EUR 10.00005 at eight decimal places.
    EXPECT_EQ(transaction.mUnits, 100'000'000);
}

TEST(TradeRepublicParserTest, ImportsTradePriceWithAllEightDecimals) {
    // Every one of the eight decimals is kept.
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "12.34567891",
        .mAmount = "-12.35",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-price-8-decimals",
    };

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mTradeInstruments.front().mTransactions.size(), 1U);

    const auto& transaction = result.mStatement.mTradeInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mUnitPrice, 1'234'567'891); // EUR 12.34567891 at eight decimal places.
    EXPECT_EQ(transaction.mUnits, 100'000'000);
}

TEST(TradeRepublicParserTest, RoundsTradePriceHalfUpAtTheNinthDecimal) {
    // Exactly half of the eight-decimal step: rounded away from zero.
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.0000000050",
        .mAmount = "-10.00",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-price-half-step",
    };

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mTradeInstruments.front().mTransactions.size(), 1U);

    const auto& transaction = result.mStatement.mTradeInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mUnitPrice, 1'000'000'001); // EUR 10.00000001 at eight decimal places.
    EXPECT_EQ(transaction.mUnits, 100'000'000);
}

TEST(TradeRepublicParserTest, RoundsTradePriceDownBelowHalfAtTheNinthDecimal) {
    // Below half of the eight-decimal step: rounded down.
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.0000000049",
        .mAmount = "-10.00",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-price-below-half",
    };

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mTradeInstruments.front().mTransactions.size(), 1U);

    const auto& transaction = result.mStatement.mTradeInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.
    EXPECT_EQ(transaction.mUnits, 100'000'000);
}

TEST(TradeRepublicParserTest, AcceptsTradePriceBelowTheOldFourDecimalStep) {
    // The old four-decimal import rejected this price as zero.
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "0.00004",
        .mAmount = "", // The export may omit the amount; -0.00004 would round to zero.
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-price-small",
    };

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mTradeInstruments.front().mTransactions.size(), 1U);

    const auto& transaction = result.mStatement.mTradeInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mUnitPrice, 4'000); // EUR 0.00004 at eight decimal places.
    EXPECT_EQ(transaction.mUnits, 100'000'000);
}

TEST(TradeRepublicParserTest, AcceptsLargestTradePrice) {
    // The largest price that fits the eight-decimal storage.
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "92233720368.54775807",
        .mAmount = "-92233720368.5478",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-price-largest",
    };

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mTradeInstruments.front().mTransactions.size(), 1U);

    const auto& transaction = result.mStatement.mTradeInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mUnitPrice,
              9'223'372'036'854'775'807); // EUR 92233720368.54775807 at eight decimal places.
    EXPECT_EQ(transaction.mUnits, 100'000'000);
}

TEST(TradeRepublicParserTest, AcceptsTheSmallestTradePrice) {
    // Exactly half of the eight-decimal step is the smallest price that rounds to a nonzero value.
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "0.000000005",
        .mAmount = "", // The export may omit the amount; -0.000000005 would round to zero.
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-price-smallest",
    };

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mTradeInstruments.front().mTransactions.size(), 1U);

    const auto& transaction = result.mStatement.mTradeInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mUnitPrice, 1); // EUR 0.00000001 at eight decimal places.
    EXPECT_EQ(transaction.mUnits, 100'000'000);
}

TEST(TradeRepublicParserTest, RoundsTradePriceUpIntoTheNextWholeEuro) {
    // The half step at the ninth decimal carries through all eight decimals.
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.9999999950",
        .mAmount = "-11.00",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-price-carry",
    };

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mTradeInstruments.front().mTransactions.size(), 1U);

    const auto& transaction = result.mStatement.mTradeInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mUnitPrice, 1'100'000'000); // EUR 11 at eight decimal places.
    EXPECT_EQ(transaction.mUnits, 100'000'000);
}

TEST(TradeRepublicParserTest, ImportsTradePriceWithMoreThanTenDecimals) {
    // Extra trailing zeros beyond the ten decimals Trade Republic writes change nothing.
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000050000000",
        .mAmount = "-10.00",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-price-12-decimals",
    };

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mTradeInstruments.front().mTransactions.size(), 1U);

    const auto& transaction = result.mStatement.mTradeInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mUnitPrice, 1'000'005'000); // EUR 10.00005 at eight decimal places.
    EXPECT_EQ(transaction.mUnits, 100'000'000);
}

TEST(TradeRepublicParserTest, AcceptsTradePriceThatRoundsDownToTheLargestValue) {
    // The ninth decimal 4 rounds down, so the price still fits the eight-decimal storage.
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "92233720368.547758074",
        .mAmount = "-92233720368.5478",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-price-rounds-to-largest",
    };

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mTradeInstruments.front().mTransactions.size(), 1U);

    const auto& transaction = result.mStatement.mTradeInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mUnitPrice,
              9'223'372'036'854'775'807); // EUR 92233720368.54775807 at eight decimal places.
    EXPECT_EQ(transaction.mUnits, 100'000'000);
}

TEST(TradeRepublicParserTest, RejectsTradePriceThatRoundsAboveTheLargestValue) {
    // The ninth decimal 5 rounds up one step past the largest UnitPrice.
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "92233720368.547758075",
        .mAmount = "-92233720368.5478",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-price-rounds-over",
    };
    const TemporaryCsvFile csvFile{{row}};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "price");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-price-rounds-over");
    EXPECT_EQ(error.mMessage.find("92233720368"), std::string::npos);
}

TEST(TradeRepublicParserTest, RejectsTradeUnitsBelowHalfStep) {
    SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.00",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-validation",
    };

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mTradeInstruments.front().mTransactions.size(), 1U);

    const auto& transaction = accepted.mStatement.mTradeInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transactionId(transaction.mMetadata), "synthetic-validation");
    EXPECT_EQ(transaction.mUnits, 100'000'000);
    EXPECT_EQ(transaction.mUnitPrice, 1'000'000'000);
    EXPECT_EQ(transaction.mAmount, 100'000);
    EXPECT_EQ(transaction.mCurrency, Currency::EUR);

    row.mShares = "0.000000004";
    const TemporaryCsvFile csvFile{{row}};
    tr::TradeRepublicParser parser;

    const auto rejected = parser.parse(csvFile.path());

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "shares");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-validation");
    EXPECT_EQ(error.mMessage.find("0.000"), std::string::npos);
}

TEST(TradeRepublicParserTest, RejectsTradeAmountBelowHalfStep) {
    SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.00",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-validation",
    };

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mTradeInstruments.front().mTransactions.size(), 1U);

    const auto& transaction = accepted.mStatement.mTradeInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transactionId(transaction.mMetadata), "synthetic-validation");
    EXPECT_EQ(transaction.mUnits, 100'000'000);
    EXPECT_EQ(transaction.mUnitPrice, 1'000'000'000);
    EXPECT_EQ(transaction.mAmount, 100'000);
    EXPECT_EQ(transaction.mCurrency, Currency::EUR);

    row.mAmount = "-0.00004";
    const TemporaryCsvFile csvFile{{row}};
    tr::TradeRepublicParser parser;

    const auto rejected = parser.parse(csvFile.path());

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "amount");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-validation");
    EXPECT_EQ(error.mMessage.find("0.000"), std::string::npos);
}

TEST(TradeRepublicParserTest, RejectsTradeFeeBelowHalfStep) {
    SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.00",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-validation",
    };

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mTradeInstruments.front().mTransactions.size(), 1U);

    const auto& transaction = accepted.mStatement.mTradeInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transactionId(transaction.mMetadata), "synthetic-validation");
    EXPECT_EQ(transaction.mUnits, 100'000'000);
    EXPECT_EQ(transaction.mUnitPrice, 1'000'000'000);
    EXPECT_EQ(transaction.mAmount, 100'000);
    EXPECT_EQ(transaction.mCurrency, Currency::EUR);

    row.mFee = "-0.00004";
    const TemporaryCsvFile csvFile{{row}};
    tr::TradeRepublicParser parser;

    const auto rejected = parser.parse(csvFile.path());

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fee");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-validation");
    EXPECT_EQ(error.mMessage.find("0.000"), std::string::npos);
}

TEST(TradeRepublicParserTest, RejectsDividendNegativeAmount) {
    SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "CASH",
        .mType = "DIVIDEND",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-validation",
    };

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& transaction =
        accepted.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transactionId(transaction.mMetadata), "synthetic-validation");
    EXPECT_EQ(transaction.mGrossAmount, 100'000);
    EXPECT_EQ(transaction.mTaxPaid, 10'000);
    EXPECT_EQ(transaction.mCurrency, Currency::EUR);
    EXPECT_EQ(transaction.mTaxCurrency, Currency::EUR);

    row.mAmount = "-1.00";
    const TemporaryCsvFile csvFile{{row}};
    tr::TradeRepublicParser parser;

    const auto rejected = parser.parse(csvFile.path());

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "amount");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-validation");
}

TEST(TradeRepublicParserTest, RejectsDividendPositiveAmountBelowHalfStep) {
    SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "CASH",
        .mType = "DIVIDEND",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-validation",
    };

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& transaction =
        accepted.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transactionId(transaction.mMetadata), "synthetic-validation");
    EXPECT_EQ(transaction.mGrossAmount, 100'000);
    EXPECT_EQ(transaction.mTaxPaid, 10'000);
    EXPECT_EQ(transaction.mCurrency, Currency::EUR);
    EXPECT_EQ(transaction.mTaxCurrency, Currency::EUR);

    row.mAmount = "0.00004";
    const TemporaryCsvFile csvFile{{row}};
    tr::TradeRepublicParser parser;

    const auto rejected = parser.parse(csvFile.path());

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "amount");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-validation");
}

TEST(TradeRepublicParserTest, RejectsDividendNegativeAmountBelowHalfStep) {
    SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "CASH",
        .mType = "DIVIDEND",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-validation",
    };

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& transaction =
        accepted.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transactionId(transaction.mMetadata), "synthetic-validation");
    EXPECT_EQ(transaction.mGrossAmount, 100'000);
    EXPECT_EQ(transaction.mTaxPaid, 10'000);
    EXPECT_EQ(transaction.mCurrency, Currency::EUR);
    EXPECT_EQ(transaction.mTaxCurrency, Currency::EUR);

    row.mAmount = "-0.00004";
    const TemporaryCsvFile csvFile{{row}};
    tr::TradeRepublicParser parser;

    const auto rejected = parser.parse(csvFile.path());

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "amount");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-validation");
}

TEST(TradeRepublicParserTest, RejectsBondInterestNegativeAmount) {
    SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "BOND",
        .mName = "Synthetic Validation Bond",
        .mSymbol = "XX9000000002",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-validation",
    };

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& transaction =
        accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transactionId(transaction.mMetadata), "synthetic-validation");
    EXPECT_EQ(transaction.mGrossAmount, 100'000);
    EXPECT_EQ(transaction.mTaxPaid, 10'000);
    EXPECT_EQ(transaction.mCurrency, Currency::EUR);
    EXPECT_EQ(transaction.mTaxCurrency, Currency::EUR);

    row.mAmount = "-1.00";
    const TemporaryCsvFile csvFile{{row}};
    tr::TradeRepublicParser parser;

    const auto rejected = parser.parse(csvFile.path());

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "amount");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-validation");
}

TEST(TradeRepublicParserTest, RejectsBondInterestPositiveAmountBelowHalfStep) {
    SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "BOND",
        .mName = "Synthetic Validation Bond",
        .mSymbol = "XX9000000002",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-validation",
    };

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& transaction =
        accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transactionId(transaction.mMetadata), "synthetic-validation");
    EXPECT_EQ(transaction.mGrossAmount, 100'000);
    EXPECT_EQ(transaction.mTaxPaid, 10'000);
    EXPECT_EQ(transaction.mCurrency, Currency::EUR);
    EXPECT_EQ(transaction.mTaxCurrency, Currency::EUR);

    row.mAmount = "0.00004";
    const TemporaryCsvFile csvFile{{row}};
    tr::TradeRepublicParser parser;

    const auto rejected = parser.parse(csvFile.path());

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "amount");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-validation");
}

TEST(TradeRepublicParserTest, RejectsBondInterestNegativeAmountBelowHalfStep) {
    SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "BOND",
        .mName = "Synthetic Validation Bond",
        .mSymbol = "XX9000000002",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-validation",
    };

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& transaction =
        accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transactionId(transaction.mMetadata), "synthetic-validation");
    EXPECT_EQ(transaction.mGrossAmount, 100'000);
    EXPECT_EQ(transaction.mTaxPaid, 10'000);
    EXPECT_EQ(transaction.mCurrency, Currency::EUR);
    EXPECT_EQ(transaction.mTaxCurrency, Currency::EUR);

    row.mAmount = "-0.00004";
    const TemporaryCsvFile csvFile{{row}};
    tr::TradeRepublicParser parser;

    const auto rejected = parser.parse(csvFile.path());

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "amount");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-validation");
}

TEST(TradeRepublicParserTest, RejectsDepositInterestNegativeAmount) {
    SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "",
        .mName = "",
        .mSymbol = "",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-validation",
    };

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& transaction =
        accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transactionId(transaction.mMetadata), "synthetic-validation");
    EXPECT_EQ(transaction.mGrossAmount, 100'000);
    EXPECT_EQ(transaction.mTaxPaid, 10'000);
    EXPECT_EQ(transaction.mCurrency, Currency::EUR);
    EXPECT_EQ(transaction.mTaxCurrency, Currency::EUR);

    row.mAmount = "-1.00";
    const TemporaryCsvFile csvFile{{row}};
    tr::TradeRepublicParser parser;

    const auto rejected = parser.parse(csvFile.path());

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "amount");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-validation");
}

TEST(TradeRepublicParserTest, RejectsDepositInterestPositiveAmountBelowHalfStep) {
    SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "",
        .mName = "",
        .mSymbol = "",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-validation",
    };

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& transaction =
        accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transactionId(transaction.mMetadata), "synthetic-validation");
    EXPECT_EQ(transaction.mGrossAmount, 100'000);
    EXPECT_EQ(transaction.mTaxPaid, 10'000);
    EXPECT_EQ(transaction.mCurrency, Currency::EUR);
    EXPECT_EQ(transaction.mTaxCurrency, Currency::EUR);

    row.mAmount = "0.00004";
    const TemporaryCsvFile csvFile{{row}};
    tr::TradeRepublicParser parser;

    const auto rejected = parser.parse(csvFile.path());

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "amount");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-validation");
}

TEST(TradeRepublicParserTest, RejectsDepositInterestNegativeAmountBelowHalfStep) {
    SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "",
        .mName = "",
        .mSymbol = "",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-validation",
    };

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& transaction =
        accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transactionId(transaction.mMetadata), "synthetic-validation");
    EXPECT_EQ(transaction.mGrossAmount, 100'000);
    EXPECT_EQ(transaction.mTaxPaid, 10'000);
    EXPECT_EQ(transaction.mCurrency, Currency::EUR);
    EXPECT_EQ(transaction.mTaxCurrency, Currency::EUR);

    row.mAmount = "-0.00004";
    const TemporaryCsvFile csvFile{{row}};
    tr::TradeRepublicParser parser;

    const auto rejected = parser.parse(csvFile.path());

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "amount");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-validation");
}

TEST(TradeRepublicParserTest, RejectsTinyWithholdingButAcceptsExplicitZeroTax) {
    SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "CASH",
        .mType = "DIVIDEND",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "",
        .mPrice = "",
        .mAmount = "1.00",
        .mFee = "",
        .mTax = "0.0000",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-validation",
    };

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& transaction =
        accepted.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transactionId(transaction.mMetadata), "synthetic-validation");
    EXPECT_EQ(transaction.mGrossAmount, 10'000);
    EXPECT_EQ(transaction.mTaxPaid, 0);
    EXPECT_EQ(transaction.mCurrency, Currency::EUR);
    EXPECT_EQ(transaction.mTaxCurrency, Currency::EUR);

    row.mTax = "-0.00004";
    const TemporaryCsvFile csvFile{{row}};
    tr::TradeRepublicParser parser;

    const auto rejected = parser.parse(csvFile.path());

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "tax");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-validation");
}

TEST(TradeRepublicParserTest, RoundsForeignDividendAndWithholdingAtTheirImportScales) {
    auto row = makeDividendRow();
    row.mAmount = "11.11";
    row.mOriginalAmount = "12.34565";
    row.mOriginalCurrency = "USD";
    row.mTax = "-2.34565";
    row.mFxRate = "0.900000005";

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mDividendInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mDividendInstruments.front();

    ASSERT_EQ(instrument.mTransactions.size(), 1U);

    const auto& income = instrument.mTransactions.front();

    // Gross income and withholding round separately; the tax never increases gross income.
    EXPECT_EQ(income.mGrossAmount, 123'457);
    EXPECT_EQ(income.mTaxPaid, 23'457);
    EXPECT_EQ(income.mExchangeRate, 90'000'001);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);
}

TEST(TradeRepublicParserTest, RejectsTinyOriginalDividendInsteadOfUsingLocalAmount) {
    SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "CASH",
        .mType = "DIVIDEND",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "12.34",
        .mOriginalCurrency = "USD",
        .mFxRate = "",
        .mTransactionId = "synthetic-validation",
    };

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& transaction =
        accepted.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transactionId(transaction.mMetadata), "synthetic-validation");
    EXPECT_EQ(transaction.mGrossAmount, 123'400);
    EXPECT_EQ(transaction.mTaxPaid, 10'000);
    EXPECT_EQ(transaction.mCurrency, Currency::USD);
    EXPECT_EQ(transaction.mTaxCurrency, Currency::EUR);
    EXPECT_FALSE(transaction.mExchangeRate.has_value());

    row.mOriginalAmount = "0.00004";
    const TemporaryCsvFile csvFile{{row}};
    tr::TradeRepublicParser parser;

    const auto rejected = parser.parse(csvFile.path(), 4);

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "original_amount");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-validation");
    EXPECT_EQ(error.mMessage.find("0.00004"), std::string::npos);
    EXPECT_EQ(error.mMessage.find(csvFile.path().parent_path().string()), std::string::npos);
}

TEST(TradeRepublicParserTest, KeepsDividendTaxYearWhenDatetimeFallsInTheNextYear) {
    auto row = makeDividendRow();
    row.mDate = "2023-12-31";
    row.mDatetime = "2024-01-01T10:00:00Z";

    const auto result = parseRows({row});

    ASSERT_EQ(result.mStatement.mDividendInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mDividendInstruments.front();

    ASSERT_EQ(instrument.mTransactions.size(), 1U);

    const auto& metadata = instrument.mTransactions.front().mMetadata;

    EXPECT_EQ(metadata.mTaxDate, makeDate(2023, 12, 31));
    EXPECT_EQ(metadata.mSourceTimestamp,
              SourceTimestamp{makeDate(2024, 1, 1).time_since_epoch() + std::chrono::hours{10}});
    EXPECT_EQ(metadata.mOrderingTimestamp,
              SourceTimestamp{makeDate(2023, 12, 31).time_since_epoch() + std::chrono::hours{9}});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& warning = result.mDiagnostics.front();

    EXPECT_EQ(warning.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(warning.mCode, DiagnosticCode::InconsistentValue);
    EXPECT_EQ(warning.mField, "datetime");
    EXPECT_EQ(warning.mRowIndex, 2U);
    EXPECT_EQ(warning.mTransactionId, row.mTransactionId);
    EXPECT_NE(warning.mMessage.find("9:00:00 UTC"), std::string::npos);
    EXPECT_NE(warning.mMessage.find("order may be incorrect"), std::string::npos);
}

TEST(TradeRepublicParserTest, RoundsForeignDepositInterestAndWithholdingAtTheirImportScales) {
    auto row = makeBrokerInterestRow();
    row.mAmount = "11.11";
    row.mOriginalAmount = "12.34565";
    row.mOriginalCurrency = "USD";
    row.mTax = "-2.34565";
    row.mFxRate = "0.900000005";

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mInterestInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mInterestInstruments.front();

    ASSERT_EQ(instrument.mTransactions.size(), 1U);

    const auto& income = instrument.mTransactions.front();

    // Gross income and withholding round separately; the tax never increases gross income.
    EXPECT_EQ(income.mGrossAmount, 123'457);
    EXPECT_EQ(income.mTaxPaid, 23'457);
    EXPECT_EQ(income.mExchangeRate, 90'000'001);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);
}

TEST(TradeRepublicParserTest, RejectsTinyOriginalDepositInterestInsteadOfUsingLocalAmount) {
    SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "",
        .mName = "",
        .mSymbol = "",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "12.34",
        .mOriginalCurrency = "USD",
        .mFxRate = "",
        .mTransactionId = "synthetic-validation",
    };

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& transaction =
        accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transactionId(transaction.mMetadata), "synthetic-validation");
    EXPECT_EQ(transaction.mGrossAmount, 123'400);
    EXPECT_EQ(transaction.mTaxPaid, 10'000);
    EXPECT_EQ(transaction.mCurrency, Currency::USD);
    EXPECT_EQ(transaction.mTaxCurrency, Currency::EUR);
    EXPECT_FALSE(transaction.mExchangeRate.has_value());

    row.mOriginalAmount = "0.00004";
    const TemporaryCsvFile csvFile{{row}};
    tr::TradeRepublicParser parser;

    const auto rejected = parser.parse(csvFile.path(), 4);

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "original_amount");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-validation");
    EXPECT_EQ(error.mMessage.find("0.00004"), std::string::npos);
    EXPECT_EQ(error.mMessage.find(csvFile.path().parent_path().string()), std::string::npos);
}

TEST(TradeRepublicParserTest, KeepsDepositInterestTaxYearWhenDatetimeFallsInTheNextYear) {
    auto row = makeBrokerInterestRow();
    row.mDate = "2023-12-31";
    row.mDatetime = "2024-01-01T10:00:00Z";

    const auto result = parseRows({row});

    ASSERT_EQ(result.mStatement.mInterestInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mInterestInstruments.front();

    ASSERT_EQ(instrument.mTransactions.size(), 1U);

    const auto& metadata = instrument.mTransactions.front().mMetadata;

    EXPECT_EQ(metadata.mTaxDate, makeDate(2023, 12, 31));
    EXPECT_EQ(metadata.mSourceTimestamp,
              SourceTimestamp{makeDate(2024, 1, 1).time_since_epoch() + std::chrono::hours{10}});
    EXPECT_EQ(metadata.mOrderingTimestamp,
              SourceTimestamp{makeDate(2023, 12, 31).time_since_epoch() + std::chrono::hours{9}});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& warning = result.mDiagnostics.front();

    EXPECT_EQ(warning.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(warning.mCode, DiagnosticCode::InconsistentValue);
    EXPECT_EQ(warning.mField, "datetime");
    EXPECT_EQ(warning.mRowIndex, 2U);
    EXPECT_EQ(warning.mTransactionId, row.mTransactionId);
    EXPECT_NE(warning.mMessage.find("9:00:00 UTC"), std::string::npos);
    EXPECT_NE(warning.mMessage.find("order may be incorrect"), std::string::npos);
}

TEST(TradeRepublicParserTest, RoundsForeignBondInterestAndWithholdingAtTheirImportScales) {
    auto row = makeBondInterestRow();
    row.mAmount = "11.11";
    row.mOriginalAmount = "12.34565";
    row.mOriginalCurrency = "USD";
    row.mTax = "-2.34565";
    row.mFxRate = "0.900000005";

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mInterestInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mInterestInstruments.front();

    ASSERT_EQ(instrument.mTransactions.size(), 1U);

    const auto& income = instrument.mTransactions.front();

    // Gross income and withholding round separately; the tax never increases gross income.
    EXPECT_EQ(income.mGrossAmount, 123'457);
    EXPECT_EQ(income.mTaxPaid, 23'457);
    EXPECT_EQ(income.mExchangeRate, 90'000'001);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);
}

TEST(TradeRepublicParserTest, RejectsTinyOriginalBondInterestInsteadOfUsingLocalAmount) {
    SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "BOND",
        .mName = "Synthetic Validation Bond",
        .mSymbol = "XX9000000002",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "12.34",
        .mOriginalCurrency = "USD",
        .mFxRate = "",
        .mTransactionId = "synthetic-validation",
    };

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& transaction =
        accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transactionId(transaction.mMetadata), "synthetic-validation");
    EXPECT_EQ(transaction.mGrossAmount, 123'400);
    EXPECT_EQ(transaction.mTaxPaid, 10'000);
    EXPECT_EQ(transaction.mCurrency, Currency::USD);
    EXPECT_EQ(transaction.mTaxCurrency, Currency::EUR);
    EXPECT_FALSE(transaction.mExchangeRate.has_value());

    row.mOriginalAmount = "0.00004";
    const TemporaryCsvFile csvFile{{row}};
    tr::TradeRepublicParser parser;

    const auto rejected = parser.parse(csvFile.path(), 4);

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "original_amount");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-validation");
    EXPECT_EQ(error.mMessage.find("0.00004"), std::string::npos);
    EXPECT_EQ(error.mMessage.find(csvFile.path().parent_path().string()), std::string::npos);
}

TEST(TradeRepublicParserTest, KeepsBondInterestTaxYearWhenDatetimeFallsInTheNextYear) {
    auto row = makeBondInterestRow();
    row.mDate = "2023-12-31";
    row.mDatetime = "2024-01-01T10:00:00Z";

    const auto result = parseRows({row});

    ASSERT_EQ(result.mStatement.mInterestInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mInterestInstruments.front();

    ASSERT_EQ(instrument.mTransactions.size(), 1U);

    const auto& metadata = instrument.mTransactions.front().mMetadata;

    EXPECT_EQ(metadata.mTaxDate, makeDate(2023, 12, 31));
    EXPECT_EQ(metadata.mSourceTimestamp,
              SourceTimestamp{makeDate(2024, 1, 1).time_since_epoch() + std::chrono::hours{10}});
    EXPECT_EQ(metadata.mOrderingTimestamp,
              SourceTimestamp{makeDate(2023, 12, 31).time_since_epoch() + std::chrono::hours{9}});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& warning = result.mDiagnostics.front();

    EXPECT_EQ(warning.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(warning.mCode, DiagnosticCode::InconsistentValue);
    EXPECT_EQ(warning.mField, "datetime");
    EXPECT_EQ(warning.mRowIndex, 2U);
    EXPECT_EQ(warning.mTransactionId, row.mTransactionId);
    EXPECT_NE(warning.mMessage.find("9:00:00 UTC"), std::string::npos);
    EXPECT_NE(warning.mMessage.find("order may be incorrect"), std::string::npos);
}

TEST(TradeRepublicParserTest, RetainsDividendWithInvalidNonEmptyDatetime) {
    auto row = makeDividendRow();
    row.mDatetime = "invalid-time";

    const auto result = parseRows({row});

    ASSERT_EQ(result.mStatement.mDividendInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mDividendInstruments.front();

    ASSERT_EQ(instrument.mTransactions.size(), 1U);

    const auto& metadata = instrument.mTransactions.front().mMetadata;

    EXPECT_EQ(metadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_FALSE(metadata.mSourceTimestamp.has_value());
    EXPECT_FALSE(metadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(metadata.mSources.size(), 1U);
    EXPECT_EQ(metadata.mSources.front().mSourceRow, 2U);
    EXPECT_EQ(metadata.mSources.front().mTransactionId, row.mTransactionId);
    EXPECT_EQ(metadata.mSources.front().mInputSequence, (StableInputSequence{0, 0}));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& warning = result.mDiagnostics.front();

    EXPECT_EQ(warning.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(warning.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(warning.mMessageKey, MessageKey::InvalidValueDatetime);
    EXPECT_TRUE(warning.mMessageParams.empty());
    EXPECT_EQ(warning.mField, "datetime");
    EXPECT_EQ(warning.mRowIndex, 2U);
    EXPECT_EQ(warning.mTransactionId, row.mTransactionId);
}

TEST(TradeRepublicParserTest, RetainsDepositInterestWithInvalidNonEmptyDatetime) {
    auto row = makeBrokerInterestRow();
    row.mDatetime = "invalid-time";

    const auto result = parseRows({row});

    ASSERT_EQ(result.mStatement.mInterestInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mInterestInstruments.front();

    ASSERT_EQ(instrument.mTransactions.size(), 1U);

    const auto& metadata = instrument.mTransactions.front().mMetadata;

    EXPECT_EQ(metadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_FALSE(metadata.mSourceTimestamp.has_value());
    EXPECT_FALSE(metadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(metadata.mSources.size(), 1U);
    EXPECT_EQ(metadata.mSources.front().mSourceRow, 2U);
    EXPECT_EQ(metadata.mSources.front().mTransactionId, row.mTransactionId);
    EXPECT_EQ(metadata.mSources.front().mInputSequence, (StableInputSequence{0, 0}));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& warning = result.mDiagnostics.front();

    EXPECT_EQ(warning.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(warning.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(warning.mField, "datetime");
    EXPECT_EQ(warning.mRowIndex, 2U);
    EXPECT_EQ(warning.mTransactionId, row.mTransactionId);
}

TEST(TradeRepublicParserTest, RetainsBondInterestWithInvalidNonEmptyDatetime) {
    auto row = makeBondInterestRow();
    row.mDatetime = "invalid-time";

    const auto result = parseRows({row});

    ASSERT_EQ(result.mStatement.mInterestInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mInterestInstruments.front();

    ASSERT_EQ(instrument.mTransactions.size(), 1U);

    const auto& metadata = instrument.mTransactions.front().mMetadata;

    EXPECT_EQ(metadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_FALSE(metadata.mSourceTimestamp.has_value());
    EXPECT_FALSE(metadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(metadata.mSources.size(), 1U);
    EXPECT_EQ(metadata.mSources.front().mSourceRow, 2U);
    EXPECT_EQ(metadata.mSources.front().mTransactionId, row.mTransactionId);
    EXPECT_EQ(metadata.mSources.front().mInputSequence, (StableInputSequence{0, 0}));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& warning = result.mDiagnostics.front();

    EXPECT_EQ(warning.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(warning.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(warning.mField, "datetime");
    EXPECT_EQ(warning.mRowIndex, 2U);
    EXPECT_EQ(warning.mTransactionId, row.mTransactionId);
}

TEST(TradeRepublicParserTest, RetainsUnresolvedSplitWithInvalidNonEmptyDatetime) {
    auto row = makeCorporateActionRow();
    row.mDatetime = "invalid-time";

    const auto result = parseRows({row});

    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mTradeInstruments.front();

    ASSERT_EQ(instrument.mCorporateActions.size(), 1U);

    const auto& metadata = instrument.mCorporateActions.front().mMetadata;

    EXPECT_EQ(metadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_FALSE(metadata.mSourceTimestamp.has_value());
    EXPECT_FALSE(metadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(metadata.mSources.size(), 1U);
    EXPECT_EQ(metadata.mSources.front().mSourceRow, 2U);
    EXPECT_EQ(metadata.mSources.front().mTransactionId, row.mTransactionId);
    EXPECT_EQ(metadata.mSources.front().mInputSequence, (StableInputSequence{0, 0}));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& warning = result.mDiagnostics.front();

    EXPECT_EQ(warning.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(warning.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(warning.mField, "datetime");
    EXPECT_EQ(warning.mRowIndex, 2U);
    EXPECT_EQ(warning.mTransactionId, row.mTransactionId);
}

TEST(TradeRepublicParserTest, DoesNotTreatInterestWithOnlyASymbolAsCashInterest) {
    SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "",
        .mName = "",
        .mSymbol = "",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-validation",
    };

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& transaction =
        accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transactionId(transaction.mMetadata), "synthetic-validation");
    EXPECT_EQ(transaction.mGrossAmount, 100'000);
    EXPECT_EQ(transaction.mTaxPaid, 10'000);
    EXPECT_EQ(transaction.mCurrency, Currency::EUR);
    EXPECT_EQ(transaction.mTaxCurrency, Currency::EUR);

    row.mSymbol = "XX9000000002";
    const TemporaryCsvFile csvFile{{row}};
    tr::TradeRepublicParser parser;

    const auto rejected = parser.parse(csvFile.path());

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::UnsupportedAssetClass);
    EXPECT_EQ(error.mField, "asset_class");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-validation");
}

TEST(TradeRepublicParserTest, DoesNotTreatInterestWithOnlyANameAsCashInterest) {
    SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "",
        .mName = "",
        .mSymbol = "",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-validation",
    };

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& transaction =
        accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transactionId(transaction.mMetadata), "synthetic-validation");
    EXPECT_EQ(transaction.mGrossAmount, 100'000);
    EXPECT_EQ(transaction.mTaxPaid, 10'000);
    EXPECT_EQ(transaction.mCurrency, Currency::EUR);
    EXPECT_EQ(transaction.mTaxCurrency, Currency::EUR);

    row.mName = "Synthetic Unclassified Payer";
    const TemporaryCsvFile csvFile{{row}};
    tr::TradeRepublicParser parser;

    const auto rejected = parser.parse(csvFile.path());

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::UnsupportedAssetClass);
    EXPECT_EQ(error.mField, "asset_class");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-validation");
}

TEST(TradeRepublicParserTest, RejectsUnverifiedBondInterestLabelWithoutCreatingACoupon) {
    SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "BOND",
        .mName = "Synthetic Validation Bond",
        .mSymbol = "XX9000000002",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-validation",
    };

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& transaction =
        accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transactionId(transaction.mMetadata), "synthetic-validation");
    EXPECT_EQ(transaction.mGrossAmount, 100'000);
    EXPECT_EQ(transaction.mTaxPaid, 10'000);
    EXPECT_EQ(transaction.mCurrency, Currency::EUR);
    EXPECT_EQ(transaction.mTaxCurrency, Currency::EUR);

    row.mType = "BOND_INTEREST";
    const TemporaryCsvFile csvFile{{row}};
    tr::TradeRepublicParser parser;

    const auto rejected = parser.parse(csvFile.path());

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::UnknownRowType);
    EXPECT_EQ(error.mField, "type");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-validation");
}

TEST(TradeRepublicParserTest, IgnoredCashMovementsDoNotRequireDatetime) {
    SyntheticCsvRow row;
    row.mCategory = "CASH";
    row.mType = "CUSTOMER_INBOUND";
    row.mDatetime.clear();

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(TradeRepublicParserTest, CreditWithoutDatetimeOrIdStillReportsItsUnknownType) {
    SyntheticCsvRow row;
    row.mCategory = "CASH";
    row.mType = "CREDIT";
    row.mDatetime.clear();
    row.mTransactionId.clear();

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::UnknownRowType);
    EXPECT_EQ(error.mField, "type");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_FALSE(error.mTransactionId.has_value());
}

TEST(TradeRepublicParserTest, RejectsNegativeOriginalBondIncomeInsteadOfUsingLocalAmount) {
    SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "BOND",
        .mName = "Synthetic Validation Bond",
        .mSymbol = "XX9000000002",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "12.34",
        .mOriginalCurrency = "USD",
        .mFxRate = "",
        .mTransactionId = "synthetic-validation",
    };

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& transaction =
        accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transactionId(transaction.mMetadata), "synthetic-validation");
    EXPECT_EQ(transaction.mGrossAmount, 123'400);
    EXPECT_EQ(transaction.mTaxPaid, 10'000);
    EXPECT_EQ(transaction.mCurrency, Currency::USD);
    EXPECT_EQ(transaction.mTaxCurrency, Currency::EUR);
    EXPECT_FALSE(transaction.mExchangeRate.has_value());

    row.mOriginalAmount = "-12.34";
    const TemporaryCsvFile csvFile{{row}};
    tr::TradeRepublicParser parser;

    const auto rejected = parser.parse(csvFile.path());

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "original_amount");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-validation");
}

TEST(TradeRepublicParserTest, RejectsTinyDepositInterestWithholdingInsteadOfLosingIt) {
    SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "",
        .mName = "",
        .mSymbol = "",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-validation",
    };

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& transaction =
        accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transactionId(transaction.mMetadata), "synthetic-validation");
    EXPECT_EQ(transaction.mGrossAmount, 100'000);
    EXPECT_EQ(transaction.mTaxPaid, 10'000);
    EXPECT_EQ(transaction.mCurrency, Currency::EUR);
    EXPECT_EQ(transaction.mTaxCurrency, Currency::EUR);

    row.mTax = "-0.00004";
    const TemporaryCsvFile csvFile{{row}};
    tr::TradeRepublicParser parser;

    const auto rejected = parser.parse(csvFile.path());

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "tax");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-validation");
}

TEST(TradeRepublicParserTest, RejectsTinyBondInterestWithholdingInsteadOfLosingIt) {
    SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "BOND",
        .mName = "Synthetic Validation Bond",
        .mSymbol = "XX9000000002",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mTransactionId = "synthetic-validation",
    };

    const auto accepted = parseRows({row});

    ASSERT_TRUE(accepted.mDiagnostics.empty());
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(accepted.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& transaction =
        accepted.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(transaction.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(transactionId(transaction.mMetadata), "synthetic-validation");
    EXPECT_EQ(transaction.mGrossAmount, 100'000);
    EXPECT_EQ(transaction.mTaxPaid, 10'000);
    EXPECT_EQ(transaction.mCurrency, Currency::EUR);
    EXPECT_EQ(transaction.mTaxCurrency, Currency::EUR);

    row.mTax = "-0.00004";
    const TemporaryCsvFile csvFile{{row}};
    tr::TradeRepublicParser parser;

    const auto rejected = parser.parse(csvFile.path());

    EXPECT_TRUE(rejected.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mDividendInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mInterestInstruments.empty());
    EXPECT_TRUE(rejected.mStatement.mBenefitEvents.empty());
    EXPECT_TRUE(rejected.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(rejected.mDiagnostics.size(), 1U);

    const auto& error = rejected.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "tax");
    EXPECT_EQ(error.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-validation");
}

TEST(TradeRepublicParserTest, MissingDatetimeDoesNotCreateAnInstrumentBeforeALaterValidTrade) {
    SyntheticCsvRow missingTime;
    missingTime.mDatetime.clear();
    missingTime.mName = "Synthetic Rejected Share";
    SyntheticCsvRow validBond;
    validBond.mAssetClass = "BOND";
    validBond.mName = "Synthetic Valid Bond";
    validBond.mTransactionId = "synthetic-valid-bond-trade";

    const auto result = parseRows({missingTime, validBond});

    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mTradeInstruments.front();

    EXPECT_EQ(instrument.mName, "Synthetic Valid Bond");
    EXPECT_EQ(instrument.mIsin, "XX9000000001");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Bond);
    ASSERT_EQ(instrument.mTransactions.size(), 1U);

    const auto& trade = instrument.mTransactions.front();

    EXPECT_EQ(trade.mUnitPrice, 1'000'000'000);
    EXPECT_EQ(trade.mUnits, 100'000'000);
    EXPECT_EQ(trade.mAmount, 100'000);
    ASSERT_EQ(trade.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(trade.mMetadata.mSources.front().mSourceRow, 3U);
    EXPECT_EQ(trade.mMetadata.mSources.front().mTransactionId, "synthetic-valid-bond-trade");
    EXPECT_EQ(trade.mMetadata.mSources.front().mInputSequence, (StableInputSequence{0, 1}));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mField, "datetime");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_"
              "MissingDatetimeDoesNotCreateAnInstrumentBeforeALaterValidTrade.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-test-transaction");
}

TEST(TradeRepublicParserTest, InvalidIncomeDoesNotSuppressALaterDividendForTheSameIsin) {
    auto invalid = makeDividendRow();
    invalid.mTax = "not-a-number";
    auto valid = makeDividendRow();
    valid.mAmount = "5.00";
    valid.mTax = "0.00";
    valid.mTransactionId = "synthetic-valid-dividend";

    const auto result = parseRows({invalid, valid});

    ASSERT_EQ(result.mStatement.mDividendInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mDividendInstruments.front();

    EXPECT_EQ(instrument.mIsin, "XX9000000001");
    ASSERT_EQ(instrument.mTransactions.size(), 1U);

    const auto& dividend = instrument.mTransactions.front();

    EXPECT_EQ(dividend.mGrossAmount, 50'000);
    EXPECT_EQ(dividend.mTaxPaid, 0);
    EXPECT_EQ(dividend.mExchangeRate, 100'000'000);
    ASSERT_EQ(dividend.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(dividend.mMetadata.mSources.front().mSourceRow, 3U);
    EXPECT_EQ(dividend.mMetadata.mSources.front().mTransactionId, "synthetic-valid-dividend");
    EXPECT_EQ(dividend.mMetadata.mSources.front().mInputSequence, (StableInputSequence{0, 1}));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = result.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "tax");
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_"
              "InvalidIncomeDoesNotSuppressALaterDividendForTheSameIsin.csv");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-dividend-validation");
}

TEST(TradeRepublicParserTest, RetainsDividendDirectForeignIncomeWithoutBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "DIVIDEND",
                        .mAssetClass = "STOCK",
                        .mName = "Synthetic Validation Share",
                        .mSymbol = "XX9000000001",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "USD",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-dividend-validation"};

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& income = result.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, std::nullopt);
    EXPECT_EQ(income.mTaxCurrency, Currency::USD);
    EXPECT_EQ(income.mMetadata.mSourceTimestamp,
              SourceTimestamp{makeDate(2024, 1, 15).time_since_epoch() + std::chrono::hours{10}});
    EXPECT_EQ(income.mMetadata.mSources,
              (std::vector<SourceReference>{
                  {.mBroker = Broker::TradeRepublic,
                   .mFilename = SourceFilename::fromPath(
                       "taxbroker_TradeRepublicParserTest_"
                       "RetainsDividendDirectForeignIncomeWithoutBrokerRate.csv"),
                   .mSourceRow = 2,
                   .mTransactionId = "synthetic-dividend-validation",
                   .mInputSequence = {0, 0}}}));
}

TEST(TradeRepublicParserTest, RetainsDividendDirectForeignIncomeWithBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "DIVIDEND",
                        .mAssetClass = "STOCK",
                        .mName = "Synthetic Validation Share",
                        .mSymbol = "XX9000000001",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "USD",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-dividend-validation"};

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& income = result.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::USD);
    EXPECT_EQ(income.mMetadata.mSourceTimestamp,
              SourceTimestamp{makeDate(2024, 1, 15).time_since_epoch() + std::chrono::hours{10}});
    EXPECT_EQ(
        income.mMetadata.mSources,
        (std::vector<SourceReference>{{.mBroker = Broker::TradeRepublic,
                                       .mFilename = SourceFilename::fromPath(
                                           "taxbroker_TradeRepublicParserTest_"
                                           "RetainsDividendDirectForeignIncomeWithBrokerRate.csv"),
                                       .mSourceRow = 2,
                                       .mTransactionId = "synthetic-dividend-validation",
                                       .mInputSequence = {0, 0}}}));
}

TEST(TradeRepublicParserTest, RetainsDividendOriginalIncomeWithoutLocalAmount) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "DIVIDEND",
                        .mAssetClass = "STOCK",
                        .mName = "Synthetic Validation Share",
                        .mSymbol = "XX9000000001",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-dividend-validation"};

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& income = result.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, std::nullopt);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);
    EXPECT_EQ(income.mMetadata.mSourceTimestamp,
              SourceTimestamp{makeDate(2024, 1, 15).time_since_epoch() + std::chrono::hours{10}});
    EXPECT_EQ(
        income.mMetadata.mSources,
        (std::vector<SourceReference>{{.mBroker = Broker::TradeRepublic,
                                       .mFilename = SourceFilename::fromPath(
                                           "taxbroker_TradeRepublicParserTest_"
                                           "RetainsDividendOriginalIncomeWithoutLocalAmount.csv"),
                                       .mSourceRow = 2,
                                       .mTransactionId = "synthetic-dividend-validation",
                                       .mInputSequence = {0, 0}}}));
}

TEST(TradeRepublicParserTest, RetainsDividendOriginalIncomeWithMalformedLocalAmount) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "DIVIDEND",
                        .mAssetClass = "STOCK",
                        .mName = "Synthetic Validation Share",
                        .mSymbol = "XX9000000001",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "not-a-number",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-dividend-validation"};

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& income = result.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, std::nullopt);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);
    EXPECT_EQ(income.mMetadata.mSourceTimestamp,
              SourceTimestamp{makeDate(2024, 1, 15).time_since_epoch() + std::chrono::hours{10}});
    EXPECT_EQ(income.mMetadata.mSources,
              (std::vector<SourceReference>{
                  {.mBroker = Broker::TradeRepublic,
                   .mFilename = SourceFilename::fromPath(
                       "taxbroker_TradeRepublicParserTest_"
                       "RetainsDividendOriginalIncomeWithMalformedLocalAmount.csv"),
                   .mSourceRow = 2,
                   .mTransactionId = "synthetic-dividend-validation",
                   .mInputSequence = {0, 0}}}));
}

TEST(TradeRepublicParserTest, RetainsDividendSmallestRoundedBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "DIVIDEND",
                        .mAssetClass = "STOCK",
                        .mName = "Synthetic Validation Share",
                        .mSymbol = "XX9000000001",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "0.000000005",
                        .mTransactionId = "synthetic-dividend-validation"};

    const auto result = parseRows({row});

    // 0.000000005 rounds half away from zero to one eight-decimal rate tick.
    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& income = result.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 1);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);
    EXPECT_EQ(income.mMetadata.mSourceTimestamp,
              SourceTimestamp{makeDate(2024, 1, 15).time_since_epoch() + std::chrono::hours{10}});
    EXPECT_EQ(
        income.mMetadata.mSources,
        (std::vector<SourceReference>{
            {.mBroker = Broker::TradeRepublic,
             .mFilename = SourceFilename::fromPath(
                 "taxbroker_TradeRepublicParserTest_RetainsDividendSmallestRoundedBrokerRate.csv"),
             .mSourceRow = 2,
             .mTransactionId = "synthetic-dividend-validation",
             .mInputSequence = {0, 0}}}));
}

TEST(TradeRepublicParserTest, RetainsDividendEurIncomeWithSuppliedBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "DIVIDEND",
                        .mAssetClass = "STOCK",
                        .mName = "Synthetic Validation Share",
                        .mSymbol = "XX9000000001",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "1.00",
                        .mTransactionId = "synthetic-dividend-validation"};

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mDividendInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mDividendInstruments.front().mTransactions.size(), 1U);

    const auto& income = result.mStatement.mDividendInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);
    EXPECT_EQ(income.mMetadata.mSourceTimestamp,
              SourceTimestamp{makeDate(2024, 1, 15).time_since_epoch() + std::chrono::hours{10}});
    EXPECT_EQ(
        income.mMetadata.mSources,
        (std::vector<SourceReference>{{.mBroker = Broker::TradeRepublic,
                                       .mFilename = SourceFilename::fromPath(
                                           "taxbroker_TradeRepublicParserTest_"
                                           "RetainsDividendEurIncomeWithSuppliedBrokerRate.csv"),
                                       .mSourceRow = 2,
                                       .mTransactionId = "synthetic-dividend-validation",
                                       .mInputSequence = {0, 0}}}));
}

TEST(TradeRepublicParserTest, RetainsDepositInterestDirectForeignIncomeWithoutBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "",
                        .mName = "",
                        .mSymbol = "",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "USD",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-deposit-validation"};

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = result.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, std::nullopt);
    EXPECT_EQ(income.mTaxCurrency, Currency::USD);
    EXPECT_EQ(income.mMetadata.mSourceTimestamp,
              SourceTimestamp{makeDate(2024, 1, 15).time_since_epoch() + std::chrono::hours{10}});
    EXPECT_EQ(income.mMetadata.mSources,
              (std::vector<SourceReference>{
                  {.mBroker = Broker::TradeRepublic,
                   .mFilename = SourceFilename::fromPath(
                       "taxbroker_TradeRepublicParserTest_"
                       "RetainsDepositInterestDirectForeignIncomeWithoutBrokerRate.csv"),
                   .mSourceRow = 2,
                   .mTransactionId = "synthetic-deposit-validation",
                   .mInputSequence = {0, 0}}}));
}

TEST(TradeRepublicParserTest, RetainsDepositInterestDirectForeignIncomeWithBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "",
                        .mName = "",
                        .mSymbol = "",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "USD",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-deposit-validation"};

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = result.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::USD);
    EXPECT_EQ(income.mMetadata.mSourceTimestamp,
              SourceTimestamp{makeDate(2024, 1, 15).time_since_epoch() + std::chrono::hours{10}});
    EXPECT_EQ(income.mMetadata.mSources,
              (std::vector<SourceReference>{
                  {.mBroker = Broker::TradeRepublic,
                   .mFilename = SourceFilename::fromPath(
                       "taxbroker_TradeRepublicParserTest_"
                       "RetainsDepositInterestDirectForeignIncomeWithBrokerRate.csv"),
                   .mSourceRow = 2,
                   .mTransactionId = "synthetic-deposit-validation",
                   .mInputSequence = {0, 0}}}));
}

TEST(TradeRepublicParserTest, RetainsDepositInterestOriginalIncomeWithoutLocalAmount) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "",
                        .mName = "",
                        .mSymbol = "",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-deposit-validation"};

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = result.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, std::nullopt);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);
    EXPECT_EQ(income.mMetadata.mSourceTimestamp,
              SourceTimestamp{makeDate(2024, 1, 15).time_since_epoch() + std::chrono::hours{10}});
    EXPECT_EQ(income.mMetadata.mSources,
              (std::vector<SourceReference>{
                  {.mBroker = Broker::TradeRepublic,
                   .mFilename = SourceFilename::fromPath(
                       "taxbroker_TradeRepublicParserTest_"
                       "RetainsDepositInterestOriginalIncomeWithoutLocalAmount.csv"),
                   .mSourceRow = 2,
                   .mTransactionId = "synthetic-deposit-validation",
                   .mInputSequence = {0, 0}}}));
}

TEST(TradeRepublicParserTest, RetainsDepositInterestOriginalIncomeWithMalformedLocalAmount) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "",
                        .mName = "",
                        .mSymbol = "",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "not-a-number",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-deposit-validation"};

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = result.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, std::nullopt);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);
    EXPECT_EQ(income.mMetadata.mSourceTimestamp,
              SourceTimestamp{makeDate(2024, 1, 15).time_since_epoch() + std::chrono::hours{10}});
    EXPECT_EQ(income.mMetadata.mSources,
              (std::vector<SourceReference>{
                  {.mBroker = Broker::TradeRepublic,
                   .mFilename = SourceFilename::fromPath(
                       "taxbroker_TradeRepublicParserTest_"
                       "RetainsDepositInterestOriginalIncomeWithMalformedLocalAmount.csv"),
                   .mSourceRow = 2,
                   .mTransactionId = "synthetic-deposit-validation",
                   .mInputSequence = {0, 0}}}));
}

TEST(TradeRepublicParserTest, RetainsDepositInterestSmallestRoundedBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "",
                        .mName = "",
                        .mSymbol = "",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "0.000000005",
                        .mTransactionId = "synthetic-deposit-validation"};

    const auto result = parseRows({row});

    // 0.000000005 rounds half away from zero to one eight-decimal rate tick.
    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = result.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 1);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);
    EXPECT_EQ(income.mMetadata.mSourceTimestamp,
              SourceTimestamp{makeDate(2024, 1, 15).time_since_epoch() + std::chrono::hours{10}});
    EXPECT_EQ(
        income.mMetadata.mSources,
        (std::vector<SourceReference>{{.mBroker = Broker::TradeRepublic,
                                       .mFilename = SourceFilename::fromPath(
                                           "taxbroker_TradeRepublicParserTest_"
                                           "RetainsDepositInterestSmallestRoundedBrokerRate.csv"),
                                       .mSourceRow = 2,
                                       .mTransactionId = "synthetic-deposit-validation",
                                       .mInputSequence = {0, 0}}}));
}

TEST(TradeRepublicParserTest, RetainsDepositInterestEurIncomeWithSuppliedBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "",
                        .mName = "",
                        .mSymbol = "",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "1.00",
                        .mTransactionId = "synthetic-deposit-validation"};

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = result.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);
    EXPECT_EQ(income.mMetadata.mSourceTimestamp,
              SourceTimestamp{makeDate(2024, 1, 15).time_since_epoch() + std::chrono::hours{10}});
    EXPECT_EQ(income.mMetadata.mSources,
              (std::vector<SourceReference>{
                  {.mBroker = Broker::TradeRepublic,
                   .mFilename = SourceFilename::fromPath(
                       "taxbroker_TradeRepublicParserTest_"
                       "RetainsDepositInterestEurIncomeWithSuppliedBrokerRate.csv"),
                   .mSourceRow = 2,
                   .mTransactionId = "synthetic-deposit-validation",
                   .mInputSequence = {0, 0}}}));
}

TEST(TradeRepublicParserTest, RetainsBondInterestDirectForeignIncomeWithoutBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "BOND",
                        .mName = "Synthetic Validation Bond",
                        .mSymbol = "XX9000000002",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "USD",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-bond-interest-validation"};

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = result.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, std::nullopt);
    EXPECT_EQ(income.mTaxCurrency, Currency::USD);
    EXPECT_EQ(income.mMetadata.mSourceTimestamp,
              SourceTimestamp{makeDate(2024, 1, 15).time_since_epoch() + std::chrono::hours{10}});
    EXPECT_EQ(income.mMetadata.mSources,
              (std::vector<SourceReference>{
                  {.mBroker = Broker::TradeRepublic,
                   .mFilename = SourceFilename::fromPath(
                       "taxbroker_TradeRepublicParserTest_"
                       "RetainsBondInterestDirectForeignIncomeWithoutBrokerRate.csv"),
                   .mSourceRow = 2,
                   .mTransactionId = "synthetic-bond-interest-validation",
                   .mInputSequence = {0, 0}}}));
}

TEST(TradeRepublicParserTest, RetainsBondInterestDirectForeignIncomeWithBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "BOND",
                        .mName = "Synthetic Validation Bond",
                        .mSymbol = "XX9000000002",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "USD",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "0.90",
                        .mTransactionId = "synthetic-bond-interest-validation"};

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = result.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 90'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::USD);
    EXPECT_EQ(income.mMetadata.mSourceTimestamp,
              SourceTimestamp{makeDate(2024, 1, 15).time_since_epoch() + std::chrono::hours{10}});
    EXPECT_EQ(income.mMetadata.mSources,
              (std::vector<SourceReference>{
                  {.mBroker = Broker::TradeRepublic,
                   .mFilename = SourceFilename::fromPath(
                       "taxbroker_TradeRepublicParserTest_"
                       "RetainsBondInterestDirectForeignIncomeWithBrokerRate.csv"),
                   .mSourceRow = 2,
                   .mTransactionId = "synthetic-bond-interest-validation",
                   .mInputSequence = {0, 0}}}));
}

TEST(TradeRepublicParserTest, RetainsBondInterestOriginalIncomeWithoutLocalAmount) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "BOND",
                        .mName = "Synthetic Validation Bond",
                        .mSymbol = "XX9000000002",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-bond-interest-validation"};

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = result.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, std::nullopt);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);
    EXPECT_EQ(income.mMetadata.mSourceTimestamp,
              SourceTimestamp{makeDate(2024, 1, 15).time_since_epoch() + std::chrono::hours{10}});
    EXPECT_EQ(income.mMetadata.mSources,
              (std::vector<SourceReference>{
                  {.mBroker = Broker::TradeRepublic,
                   .mFilename = SourceFilename::fromPath(
                       "taxbroker_TradeRepublicParserTest_"
                       "RetainsBondInterestOriginalIncomeWithoutLocalAmount.csv"),
                   .mSourceRow = 2,
                   .mTransactionId = "synthetic-bond-interest-validation",
                   .mInputSequence = {0, 0}}}));
}

TEST(TradeRepublicParserTest, RetainsBondInterestOriginalIncomeWithMalformedLocalAmount) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "BOND",
                        .mName = "Synthetic Validation Bond",
                        .mSymbol = "XX9000000002",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "not-a-number",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "",
                        .mTransactionId = "synthetic-bond-interest-validation"};

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = result.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, std::nullopt);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);
    EXPECT_EQ(income.mMetadata.mSourceTimestamp,
              SourceTimestamp{makeDate(2024, 1, 15).time_since_epoch() + std::chrono::hours{10}});
    EXPECT_EQ(income.mMetadata.mSources,
              (std::vector<SourceReference>{
                  {.mBroker = Broker::TradeRepublic,
                   .mFilename = SourceFilename::fromPath(
                       "taxbroker_TradeRepublicParserTest_"
                       "RetainsBondInterestOriginalIncomeWithMalformedLocalAmount.csv"),
                   .mSourceRow = 2,
                   .mTransactionId = "synthetic-bond-interest-validation",
                   .mInputSequence = {0, 0}}}));
}

TEST(TradeRepublicParserTest, RetainsBondInterestSmallestRoundedBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "BOND",
                        .mName = "Synthetic Validation Bond",
                        .mSymbol = "XX9000000002",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "11.00",
                        .mOriginalCurrency = "USD",
                        .mFxRate = "0.000000005",
                        .mTransactionId = "synthetic-bond-interest-validation"};

    const auto result = parseRows({row});

    // 0.000000005 rounds half away from zero to one eight-decimal rate tick.
    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = result.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 110'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 1);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);
    EXPECT_EQ(income.mMetadata.mSourceTimestamp,
              SourceTimestamp{makeDate(2024, 1, 15).time_since_epoch() + std::chrono::hours{10}});
    EXPECT_EQ(
        income.mMetadata.mSources,
        (std::vector<SourceReference>{{.mBroker = Broker::TradeRepublic,
                                       .mFilename = SourceFilename::fromPath(
                                           "taxbroker_TradeRepublicParserTest_"
                                           "RetainsBondInterestSmallestRoundedBrokerRate.csv"),
                                       .mSourceRow = 2,
                                       .mTransactionId = "synthetic-bond-interest-validation",
                                       .mInputSequence = {0, 0}}}));
}

TEST(TradeRepublicParserTest, RetainsBondInterestEurIncomeWithSuppliedBrokerRate) {
    SyntheticCsvRow row{.mDatetime = "2024-01-15T10:00:00.000Z",
                        .mDate = "2024-01-15",
                        .mCategory = "CASH",
                        .mType = "INTEREST_PAYMENT",
                        .mAssetClass = "BOND",
                        .mName = "Synthetic Validation Bond",
                        .mSymbol = "XX9000000002",
                        .mShares = "",
                        .mPrice = "",
                        .mAmount = "10.00",
                        .mTax = "-1.00",
                        .mCurrency = "EUR",
                        .mOriginalAmount = "",
                        .mOriginalCurrency = "",
                        .mFxRate = "1.00",
                        .mTransactionId = "synthetic-bond-interest-validation"};

    const auto result = parseRows({row});

    ASSERT_TRUE(result.mDiagnostics.empty());
    ASSERT_EQ(result.mStatement.mInterestInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mInterestInstruments.front().mTransactions.size(), 1U);

    const auto& income = result.mStatement.mInterestInstruments.front().mTransactions.front();

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::EUR);
    EXPECT_EQ(income.mTaxPaid, 10'000);
    EXPECT_EQ(income.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    EXPECT_EQ(income.mExchangeRate, 100'000'000);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);
    EXPECT_EQ(income.mMetadata.mSourceTimestamp,
              SourceTimestamp{makeDate(2024, 1, 15).time_since_epoch() + std::chrono::hours{10}});
    EXPECT_EQ(income.mMetadata.mSources,
              (std::vector<SourceReference>{
                  {.mBroker = Broker::TradeRepublic,
                   .mFilename = SourceFilename::fromPath(
                       "taxbroker_TradeRepublicParserTest_"
                       "RetainsBondInterestEurIncomeWithSuppliedBrokerRate.csv"),
                   .mSourceRow = 2,
                   .mTransactionId = "synthetic-bond-interest-validation",
                   .mInputSequence = {0, 0}}}));
}

TEST(TradeRepublicParserTest, RetainsTradeWithMissingName) {
    const SyntheticCsvRow sourceRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{sourceRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000001");
    EXPECT_EQ(instrument.mName, "");
    ASSERT_EQ(instrument.mTransactions.size(), 1U);

    const auto& event = instrument.mTransactions[0];

    EXPECT_EQ(event.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    ASSERT_EQ(event.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(event.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(event.mMetadata.mSources[0].mFilename.value(), csvFile.path().filename().string());
    EXPECT_EQ(event.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(event.mMetadata.mSources[0].mTransactionId, "synthetic-test-transaction");
    EXPECT_EQ(event.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(event.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(event.mInstrumentEvidence[0].mSource, event.mMetadata.mSources[0]);
    EXPECT_EQ(event.mInstrumentEvidence[0].mName, "");
    EXPECT_EQ(event.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Stock);
    EXPECT_EQ(event.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(event.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(event.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.
    EXPECT_EQ(event.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(event.mAmount, 100'000);
    EXPECT_EQ(event.mFeePaid, 0);
    EXPECT_EQ(event.mCurrency, Currency::EUR);
    ASSERT_EQ(event.mUnitEvidence.size(), 1U);
    EXPECT_EQ(event.mUnitEvidence[0].mSourceText, "1.0000000000");

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::MissingFieldName);
    EXPECT_TRUE(diagnostic.mMessageParams.empty());
    EXPECT_EQ(diagnostic.mField, "name");
    EXPECT_EQ(diagnostic.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(diagnostic.mRowIndex, 2U);
    EXPECT_EQ(diagnostic.mTransactionId, "synthetic-test-transaction");
}

TEST(TradeRepublicParserTest, RetainsTradeWithMissingClass) {
    const SyntheticCsvRow sourceRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{sourceRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000001");
    EXPECT_EQ(instrument.mName, "Synthetic Test Share");
    ASSERT_EQ(instrument.mTransactions.size(), 1U);

    const auto& event = instrument.mTransactions[0];

    EXPECT_EQ(event.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    ASSERT_EQ(event.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(event.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(event.mMetadata.mSources[0].mFilename.value(), csvFile.path().filename().string());
    EXPECT_EQ(event.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(event.mMetadata.mSources[0].mTransactionId, "synthetic-test-transaction");
    EXPECT_EQ(event.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(event.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(event.mInstrumentEvidence[0].mSource, event.mMetadata.mSources[0]);
    EXPECT_EQ(event.mInstrumentEvidence[0].mName, "Synthetic Test Share");
    EXPECT_EQ(event.mInstrumentEvidence[0].mAssetClassLabel, "");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Unknown);
    EXPECT_EQ(event.mInstrumentEvidence[0].mAssetClass, AssetClass::Unknown);
    EXPECT_EQ(event.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(event.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.
    EXPECT_EQ(event.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(event.mAmount, 100'000);
    EXPECT_EQ(event.mFeePaid, 0);
    EXPECT_EQ(event.mCurrency, Currency::EUR);
    ASSERT_EQ(event.mUnitEvidence.size(), 1U);
    EXPECT_EQ(event.mUnitEvidence[0].mSourceText, "1.0000000000");

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(diagnostic.mField, "asset_class");
    EXPECT_EQ(diagnostic.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(diagnostic.mRowIndex, 2U);
    EXPECT_EQ(diagnostic.mTransactionId, "synthetic-test-transaction");
}

TEST(TradeRepublicParserTest, RetainsTradeWithUnrecognizedClass) {
    const SyntheticCsvRow sourceRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "COMMODITY",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{sourceRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000001");
    EXPECT_EQ(instrument.mName, "Synthetic Test Share");
    ASSERT_EQ(instrument.mTransactions.size(), 1U);

    const auto& event = instrument.mTransactions[0];

    EXPECT_EQ(event.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    ASSERT_EQ(event.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(event.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(event.mMetadata.mSources[0].mFilename.value(), csvFile.path().filename().string());
    EXPECT_EQ(event.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(event.mMetadata.mSources[0].mTransactionId, "synthetic-test-transaction");
    EXPECT_EQ(event.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(event.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(event.mInstrumentEvidence[0].mSource, event.mMetadata.mSources[0]);
    EXPECT_EQ(event.mInstrumentEvidence[0].mName, "Synthetic Test Share");
    EXPECT_EQ(event.mInstrumentEvidence[0].mAssetClassLabel, "COMMODITY");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Unknown);
    EXPECT_EQ(event.mInstrumentEvidence[0].mAssetClass, AssetClass::Unknown);
    EXPECT_EQ(event.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(event.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.
    EXPECT_EQ(event.mUnits, 100'000'000);       // 1 unit at eight decimal places.
    EXPECT_EQ(event.mAmount, 100'000);
    EXPECT_EQ(event.mFeePaid, 0);
    EXPECT_EQ(event.mCurrency, Currency::EUR);
    ASSERT_EQ(event.mUnitEvidence.size(), 1U);
    EXPECT_EQ(event.mUnitEvidence[0].mSourceText, "1.0000000000");

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::UnsupportedAssetClass);
    EXPECT_EQ(diagnostic.mField, "asset_class");
    EXPECT_EQ(diagnostic.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(diagnostic.mRowIndex, 2U);
    EXPECT_EQ(diagnostic.mTransactionId, "synthetic-test-transaction");
}

TEST(TradeRepublicParserTest, RetainsActionWithMissingName) {
    const SyntheticCsvRow sourceRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "",
        .mSymbol = "XX9000000001",
        .mShares = "1.00",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-validation",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{sourceRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000001");
    EXPECT_EQ(instrument.mName, "");
    ASSERT_EQ(instrument.mCorporateActions.size(), 1U);

    const auto& event = instrument.mCorporateActions[0];

    EXPECT_EQ(event.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    ASSERT_EQ(event.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(event.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(event.mMetadata.mSources[0].mFilename.value(), csvFile.path().filename().string());
    EXPECT_EQ(event.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(event.mMetadata.mSources[0].mTransactionId, "synthetic-split-validation");
    EXPECT_EQ(event.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(event.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(event.mInstrumentEvidence[0].mSource, event.mMetadata.mSources[0]);
    EXPECT_EQ(event.mInstrumentEvidence[0].mName, "");
    EXPECT_EQ(event.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Stock);
    EXPECT_EQ(event.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(event.mType, CorporateActionType::UnresolvedSplit);
    EXPECT_EQ(event.mUnitsDelta, 100'000'000); // Source 1 unit, not a verified split change.
    EXPECT_FALSE(event.mRatio.has_value());
    ASSERT_EQ(event.mUnitEvidence.size(), 1U);
    EXPECT_EQ(event.mUnitEvidence[0].mSourceText, "1.00");

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(diagnostic.mField, "name");
    EXPECT_EQ(diagnostic.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(diagnostic.mRowIndex, 2U);
    EXPECT_EQ(diagnostic.mTransactionId, "synthetic-split-validation");
}

TEST(TradeRepublicParserTest, RetainsActionWithMissingClass) {
    const SyntheticCsvRow sourceRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.00",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-validation",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{sourceRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000001");
    EXPECT_EQ(instrument.mName, "Synthetic Test Share");
    ASSERT_EQ(instrument.mCorporateActions.size(), 1U);

    const auto& event = instrument.mCorporateActions[0];

    EXPECT_EQ(event.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    ASSERT_EQ(event.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(event.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(event.mMetadata.mSources[0].mFilename.value(), csvFile.path().filename().string());
    EXPECT_EQ(event.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(event.mMetadata.mSources[0].mTransactionId, "synthetic-split-validation");
    EXPECT_EQ(event.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(event.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(event.mInstrumentEvidence[0].mSource, event.mMetadata.mSources[0]);
    EXPECT_EQ(event.mInstrumentEvidence[0].mName, "Synthetic Test Share");
    EXPECT_EQ(event.mInstrumentEvidence[0].mAssetClassLabel, "");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Unknown);
    EXPECT_EQ(event.mInstrumentEvidence[0].mAssetClass, AssetClass::Unknown);
    EXPECT_EQ(event.mType, CorporateActionType::UnresolvedSplit);
    EXPECT_EQ(event.mUnitsDelta, 100'000'000); // Source 1 unit, not a verified split change.
    EXPECT_FALSE(event.mRatio.has_value());
    ASSERT_EQ(event.mUnitEvidence.size(), 1U);
    EXPECT_EQ(event.mUnitEvidence[0].mSourceText, "1.00");

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(diagnostic.mField, "asset_class");
    EXPECT_EQ(diagnostic.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(diagnostic.mRowIndex, 2U);
    EXPECT_EQ(diagnostic.mTransactionId, "synthetic-split-validation");
}

TEST(TradeRepublicParserTest, RetainsActionWithUnrecognizedClass) {
    const SyntheticCsvRow sourceRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "COMMODITY",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.00",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-validation",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{sourceRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000001");
    EXPECT_EQ(instrument.mName, "Synthetic Test Share");
    ASSERT_EQ(instrument.mCorporateActions.size(), 1U);

    const auto& event = instrument.mCorporateActions[0];

    EXPECT_EQ(event.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    ASSERT_EQ(event.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(event.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(event.mMetadata.mSources[0].mFilename.value(), csvFile.path().filename().string());
    EXPECT_EQ(event.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(event.mMetadata.mSources[0].mTransactionId, "synthetic-split-validation");
    EXPECT_EQ(event.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(event.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(event.mInstrumentEvidence[0].mSource, event.mMetadata.mSources[0]);
    EXPECT_EQ(event.mInstrumentEvidence[0].mName, "Synthetic Test Share");
    EXPECT_EQ(event.mInstrumentEvidence[0].mAssetClassLabel, "COMMODITY");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Unknown);
    EXPECT_EQ(event.mInstrumentEvidence[0].mAssetClass, AssetClass::Unknown);
    EXPECT_EQ(event.mType, CorporateActionType::UnresolvedSplit);
    EXPECT_EQ(event.mUnitsDelta, 100'000'000); // Source 1 unit, not a verified split change.
    EXPECT_FALSE(event.mRatio.has_value());
    ASSERT_EQ(event.mUnitEvidence.size(), 1U);
    EXPECT_EQ(event.mUnitEvidence[0].mSourceText, "1.00");

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::UnsupportedAssetClass);
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::UnsupportedAssetClassUnrecognized);
    EXPECT_TRUE(diagnostic.mMessageParams.empty());
    EXPECT_EQ(diagnostic.mField, "asset_class");
    EXPECT_EQ(diagnostic.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(diagnostic.mRowIndex, 2U);
    EXPECT_EQ(diagnostic.mTransactionId, "synthetic-split-validation");
}

TEST(TradeRepublicParserTest, RetainsDividendWithMissingName) {
    const SyntheticCsvRow sourceRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "DIVIDEND",
        .mAssetClass = "STOCK",
        .mName = "",
        .mSymbol = "XX9000000001",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-dividend-validation",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{sourceRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    ASSERT_EQ(result.mStatement.mDividendInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mDividendInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000001");
    EXPECT_EQ(instrument.mName, "");
    ASSERT_EQ(instrument.mTransactions.size(), 1U);

    const auto& event = instrument.mTransactions[0];

    EXPECT_EQ(event.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    ASSERT_EQ(event.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(event.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(event.mMetadata.mSources[0].mFilename.value(), csvFile.path().filename().string());
    EXPECT_EQ(event.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(event.mMetadata.mSources[0].mTransactionId, "synthetic-dividend-validation");
    EXPECT_EQ(event.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(event.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(event.mInstrumentEvidence[0].mSource, event.mMetadata.mSources[0]);
    EXPECT_EQ(event.mInstrumentEvidence[0].mName, "");
    EXPECT_EQ(event.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(event.mGrossAmount, 100'000); // EUR 10 at four decimal places.
    EXPECT_EQ(event.mTaxPaid, 10'000);      // EUR 1 withholding at four decimal places.
    EXPECT_EQ(event.mCurrency, Currency::EUR);
    EXPECT_EQ(event.mTaxCurrency, Currency::EUR);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(diagnostic.mField, "name");
    EXPECT_EQ(diagnostic.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(diagnostic.mRowIndex, 2U);
    EXPECT_EQ(diagnostic.mTransactionId, "synthetic-dividend-validation");
}

TEST(TradeRepublicParserTest, RetainsBondInterestWithMissingName) {
    const SyntheticCsvRow sourceRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "BOND",
        .mName = "",
        .mSymbol = "XX9000000002",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-bond-interest-validation",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{sourceRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    ASSERT_EQ(result.mStatement.mInterestInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mInterestInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000002");
    EXPECT_EQ(instrument.mName, "");
    ASSERT_EQ(instrument.mTransactions.size(), 1U);

    const auto& event = instrument.mTransactions[0];

    EXPECT_EQ(event.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    ASSERT_EQ(event.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(event.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(event.mMetadata.mSources[0].mFilename.value(), csvFile.path().filename().string());
    EXPECT_EQ(event.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(event.mMetadata.mSources[0].mTransactionId, "synthetic-bond-interest-validation");
    EXPECT_EQ(event.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(event.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(event.mInstrumentEvidence[0].mSource, event.mMetadata.mSources[0]);
    EXPECT_EQ(event.mInstrumentEvidence[0].mName, "");
    EXPECT_EQ(event.mInstrumentEvidence[0].mAssetClassLabel, "BOND");
    EXPECT_EQ(event.mGrossAmount, 100'000); // EUR 10 at four decimal places.
    EXPECT_EQ(event.mTaxPaid, 10'000);      // EUR 1 withholding at four decimal places.
    EXPECT_EQ(event.mCurrency, Currency::EUR);
    EXPECT_EQ(event.mTaxCurrency, Currency::EUR);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(diagnostic.mField, "name");
    EXPECT_EQ(diagnostic.mSourceFile, csvFile.path().filename().string());
    EXPECT_EQ(diagnostic.mRowIndex, 2U);
    EXPECT_EQ(diagnostic.mTransactionId, "synthetic-bond-interest-validation");
}

TEST(TradeRepublicParserTest, MissingClassRemainsUnresolvedWithKnownClassLast) {
    const SyntheticCsvRow unknownRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-unknown",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const SyntheticCsvRow knownRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-known",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{unknownRow, knownRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mTradeInstruments[0];

    EXPECT_EQ(instrument.mAssetClass, AssetClass::Unknown);
    ASSERT_EQ(instrument.mTransactions.size(), 2U);

    const auto& first = instrument.mTransactions[0];
    const auto& second = instrument.mTransactions[1];

    EXPECT_EQ(first.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    ASSERT_EQ(first.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(first.mMetadata.mSources[0].mFilename.value(), csvFile.path().filename().string());
    EXPECT_EQ(first.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(first.mMetadata.mSources[0].mTransactionId, "synthetic-unknown");
    EXPECT_EQ(first.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(first.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(first.mInstrumentEvidence[0].mSource, first.mMetadata.mSources[0]);
    EXPECT_EQ(first.mInstrumentEvidence[0].mAssetClass, AssetClass::Unknown);
    EXPECT_EQ(first.mInstrumentEvidence[0].mAssetClassLabel, "");
    EXPECT_EQ(first.mInstrumentEvidence[0].mName, "Synthetic Test Share");
    EXPECT_EQ(first.mUnits, 100'000'000);       // Source 1 unit at eight decimal places.
    EXPECT_EQ(first.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.
    EXPECT_EQ(first.mAmount, 100'000);
    EXPECT_EQ(first.mCurrency, Currency::EUR);

    EXPECT_EQ(second.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    ASSERT_EQ(second.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(second.mMetadata.mSources[0].mFilename.value(), csvFile.path().filename().string());
    EXPECT_EQ(second.mMetadata.mSources[0].mSourceRow, 3U);
    EXPECT_EQ(second.mMetadata.mSources[0].mTransactionId, "synthetic-known");
    EXPECT_EQ(second.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 1}));
    ASSERT_EQ(second.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(second.mInstrumentEvidence[0].mSource, second.mMetadata.mSources[0]);
    EXPECT_EQ(second.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(second.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(second.mInstrumentEvidence[0].mName, "Synthetic Test Share");
    EXPECT_EQ(second.mUnits, 100'000'000);       // Source 1 unit at eight decimal places.
    EXPECT_EQ(second.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.
    EXPECT_EQ(second.mAmount, 100'000);
    EXPECT_EQ(second.mCurrency, Currency::EUR);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics[0].mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(result.mDiagnostics[0].mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(result.mDiagnostics[0].mMessageKey, MessageKey::MissingFieldAssetClass);
    EXPECT_TRUE(result.mDiagnostics[0].mMessageParams.empty());
    EXPECT_EQ(result.mDiagnostics[0].mField, "asset_class");
    EXPECT_EQ(result.mDiagnostics[0].mRowIndex, 2U);
    EXPECT_EQ(result.mDiagnostics[0].mTransactionId, "synthetic-unknown");
}

TEST(TradeRepublicParserTest, MissingClassRemainsUnresolvedWithKnownClassFirst) {
    const SyntheticCsvRow knownRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-known",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const SyntheticCsvRow unknownRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-unknown",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{knownRow, unknownRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mTradeInstruments[0];

    EXPECT_EQ(instrument.mAssetClass, AssetClass::Unknown);
    ASSERT_EQ(instrument.mTransactions.size(), 2U);

    const auto& first = instrument.mTransactions[0];
    const auto& second = instrument.mTransactions[1];

    EXPECT_EQ(first.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    ASSERT_EQ(first.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(first.mMetadata.mSources[0].mFilename.value(), csvFile.path().filename().string());
    EXPECT_EQ(first.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(first.mMetadata.mSources[0].mTransactionId, "synthetic-known");
    EXPECT_EQ(first.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(first.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(first.mInstrumentEvidence[0].mSource, first.mMetadata.mSources[0]);
    EXPECT_EQ(first.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(first.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(first.mInstrumentEvidence[0].mName, "Synthetic Test Share");
    EXPECT_EQ(first.mUnits, 100'000'000);       // Source 1 unit at eight decimal places.
    EXPECT_EQ(first.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.
    EXPECT_EQ(first.mAmount, 100'000);
    EXPECT_EQ(first.mCurrency, Currency::EUR);

    EXPECT_EQ(second.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    ASSERT_EQ(second.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(second.mMetadata.mSources[0].mFilename.value(), csvFile.path().filename().string());
    EXPECT_EQ(second.mMetadata.mSources[0].mSourceRow, 3U);
    EXPECT_EQ(second.mMetadata.mSources[0].mTransactionId, "synthetic-unknown");
    EXPECT_EQ(second.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 1}));
    ASSERT_EQ(second.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(second.mInstrumentEvidence[0].mSource, second.mMetadata.mSources[0]);
    EXPECT_EQ(second.mInstrumentEvidence[0].mAssetClass, AssetClass::Unknown);
    EXPECT_EQ(second.mInstrumentEvidence[0].mAssetClassLabel, "");
    EXPECT_EQ(second.mInstrumentEvidence[0].mName, "Synthetic Test Share");
    EXPECT_EQ(second.mUnits, 100'000'000);       // Source 1 unit at eight decimal places.
    EXPECT_EQ(second.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.
    EXPECT_EQ(second.mAmount, 100'000);
    EXPECT_EQ(second.mCurrency, Currency::EUR);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics[0].mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(result.mDiagnostics[0].mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(result.mDiagnostics[0].mField, "asset_class");
    EXPECT_EQ(result.mDiagnostics[0].mRowIndex, 3U);
    EXPECT_EQ(result.mDiagnostics[0].mTransactionId, "synthetic-unknown");
}

TEST(TradeRepublicParserTest, UnrecognizedClassRemainsUnresolvedWithKnownClassLast) {
    const SyntheticCsvRow unknownRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "COMMODITY",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-unknown",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const SyntheticCsvRow knownRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-known",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{unknownRow, knownRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mTradeInstruments[0];

    EXPECT_EQ(instrument.mAssetClass, AssetClass::Unknown);
    ASSERT_EQ(instrument.mTransactions.size(), 2U);

    const auto& first = instrument.mTransactions[0];
    const auto& second = instrument.mTransactions[1];

    EXPECT_EQ(first.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    ASSERT_EQ(first.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(first.mMetadata.mSources[0].mFilename.value(), csvFile.path().filename().string());
    EXPECT_EQ(first.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(first.mMetadata.mSources[0].mTransactionId, "synthetic-unknown");
    EXPECT_EQ(first.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(first.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(first.mInstrumentEvidence[0].mSource, first.mMetadata.mSources[0]);
    EXPECT_EQ(first.mInstrumentEvidence[0].mAssetClass, AssetClass::Unknown);
    EXPECT_EQ(first.mInstrumentEvidence[0].mAssetClassLabel, "COMMODITY");
    EXPECT_EQ(first.mInstrumentEvidence[0].mName, "Synthetic Test Share");
    EXPECT_EQ(first.mUnits, 100'000'000);       // Source 1 unit at eight decimal places.
    EXPECT_EQ(first.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.
    EXPECT_EQ(first.mAmount, 100'000);
    EXPECT_EQ(first.mCurrency, Currency::EUR);

    EXPECT_EQ(second.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    ASSERT_EQ(second.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(second.mMetadata.mSources[0].mFilename.value(), csvFile.path().filename().string());
    EXPECT_EQ(second.mMetadata.mSources[0].mSourceRow, 3U);
    EXPECT_EQ(second.mMetadata.mSources[0].mTransactionId, "synthetic-known");
    EXPECT_EQ(second.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 1}));
    ASSERT_EQ(second.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(second.mInstrumentEvidence[0].mSource, second.mMetadata.mSources[0]);
    EXPECT_EQ(second.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(second.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(second.mInstrumentEvidence[0].mName, "Synthetic Test Share");
    EXPECT_EQ(second.mUnits, 100'000'000);       // Source 1 unit at eight decimal places.
    EXPECT_EQ(second.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.
    EXPECT_EQ(second.mAmount, 100'000);
    EXPECT_EQ(second.mCurrency, Currency::EUR);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics[0].mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(result.mDiagnostics[0].mCode, DiagnosticCode::UnsupportedAssetClass);
    EXPECT_EQ(result.mDiagnostics[0].mField, "asset_class");
    EXPECT_EQ(result.mDiagnostics[0].mRowIndex, 2U);
    EXPECT_EQ(result.mDiagnostics[0].mTransactionId, "synthetic-unknown");
}

TEST(TradeRepublicParserTest, UnrecognizedClassRemainsUnresolvedWithKnownClassFirst) {
    const SyntheticCsvRow knownRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-known",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const SyntheticCsvRow unknownRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "COMMODITY",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-unknown",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{knownRow, unknownRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mTradeInstruments[0];

    EXPECT_EQ(instrument.mAssetClass, AssetClass::Unknown);
    ASSERT_EQ(instrument.mTransactions.size(), 2U);

    const auto& first = instrument.mTransactions[0];
    const auto& second = instrument.mTransactions[1];

    EXPECT_EQ(first.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    ASSERT_EQ(first.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(first.mMetadata.mSources[0].mFilename.value(), csvFile.path().filename().string());
    EXPECT_EQ(first.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(first.mMetadata.mSources[0].mTransactionId, "synthetic-known");
    EXPECT_EQ(first.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(first.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(first.mInstrumentEvidence[0].mSource, first.mMetadata.mSources[0]);
    EXPECT_EQ(first.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(first.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(first.mInstrumentEvidence[0].mName, "Synthetic Test Share");
    EXPECT_EQ(first.mUnits, 100'000'000);       // Source 1 unit at eight decimal places.
    EXPECT_EQ(first.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.
    EXPECT_EQ(first.mAmount, 100'000);
    EXPECT_EQ(first.mCurrency, Currency::EUR);

    EXPECT_EQ(second.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    ASSERT_EQ(second.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(second.mMetadata.mSources[0].mFilename.value(), csvFile.path().filename().string());
    EXPECT_EQ(second.mMetadata.mSources[0].mSourceRow, 3U);
    EXPECT_EQ(second.mMetadata.mSources[0].mTransactionId, "synthetic-unknown");
    EXPECT_EQ(second.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 1}));
    ASSERT_EQ(second.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(second.mInstrumentEvidence[0].mSource, second.mMetadata.mSources[0]);
    EXPECT_EQ(second.mInstrumentEvidence[0].mAssetClass, AssetClass::Unknown);
    EXPECT_EQ(second.mInstrumentEvidence[0].mAssetClassLabel, "COMMODITY");
    EXPECT_EQ(second.mInstrumentEvidence[0].mName, "Synthetic Test Share");
    EXPECT_EQ(second.mUnits, 100'000'000);       // Source 1 unit at eight decimal places.
    EXPECT_EQ(second.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.
    EXPECT_EQ(second.mAmount, 100'000);
    EXPECT_EQ(second.mCurrency, Currency::EUR);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics[0].mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(result.mDiagnostics[0].mCode, DiagnosticCode::UnsupportedAssetClass);
    EXPECT_EQ(result.mDiagnostics[0].mField, "asset_class");
    EXPECT_EQ(result.mDiagnostics[0].mRowIndex, 3U);
    EXPECT_EQ(result.mDiagnostics[0].mTransactionId, "synthetic-unknown");
}

TEST(TradeRepublicParserTest, LaterMatchingClassCannotClearAClassConflict) {
    const SyntheticCsvRow firstRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-stock-first",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const SyntheticCsvRow secondRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "FUND",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-fund",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const SyntheticCsvRow thirdRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-stock-last",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{firstRow, secondRow, thirdRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mTradeInstruments[0];

    EXPECT_EQ(instrument.mAssetClass, AssetClass::Unknown);
    ASSERT_EQ(instrument.mTransactions.size(), 3U);

    const auto& first = instrument.mTransactions[0];
    const auto& second = instrument.mTransactions[1];
    const auto& third = instrument.mTransactions[2];

    EXPECT_EQ(first.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    ASSERT_EQ(first.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(first.mMetadata.mSources[0].mFilename.value(), csvFile.path().filename().string());
    EXPECT_EQ(first.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(first.mMetadata.mSources[0].mTransactionId, "synthetic-stock-first");
    EXPECT_EQ(first.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(first.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(first.mInstrumentEvidence[0].mSource, first.mMetadata.mSources[0]);
    EXPECT_EQ(first.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(first.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(first.mInstrumentEvidence[0].mName, "Synthetic Test Share");
    EXPECT_EQ(first.mUnits, 100'000'000);       // Source 1 unit at eight decimal places.
    EXPECT_EQ(first.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.
    EXPECT_EQ(first.mAmount, 100'000);
    EXPECT_EQ(first.mCurrency, Currency::EUR);

    EXPECT_EQ(second.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    ASSERT_EQ(second.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(second.mMetadata.mSources[0].mFilename.value(), csvFile.path().filename().string());
    EXPECT_EQ(second.mMetadata.mSources[0].mSourceRow, 3U);
    EXPECT_EQ(second.mMetadata.mSources[0].mTransactionId, "synthetic-fund");
    EXPECT_EQ(second.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 1}));
    ASSERT_EQ(second.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(second.mInstrumentEvidence[0].mSource, second.mMetadata.mSources[0]);
    EXPECT_EQ(second.mInstrumentEvidence[0].mAssetClass, AssetClass::Fund);
    EXPECT_EQ(second.mInstrumentEvidence[0].mAssetClassLabel, "FUND");
    EXPECT_EQ(second.mInstrumentEvidence[0].mName, "Synthetic Test Share");
    EXPECT_EQ(second.mUnits, 100'000'000);       // Source 1 unit at eight decimal places.
    EXPECT_EQ(second.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.
    EXPECT_EQ(second.mAmount, 100'000);
    EXPECT_EQ(second.mCurrency, Currency::EUR);

    EXPECT_EQ(third.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    ASSERT_EQ(third.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(third.mMetadata.mSources[0].mFilename.value(), csvFile.path().filename().string());
    EXPECT_EQ(third.mMetadata.mSources[0].mSourceRow, 4U);
    EXPECT_EQ(third.mMetadata.mSources[0].mTransactionId, "synthetic-stock-last");
    EXPECT_EQ(third.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 2}));
    ASSERT_EQ(third.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(third.mInstrumentEvidence[0].mSource, third.mMetadata.mSources[0]);
    EXPECT_EQ(third.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(third.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(third.mInstrumentEvidence[0].mName, "Synthetic Test Share");
    EXPECT_EQ(third.mUnits, 100'000'000);       // Source 1 unit at eight decimal places.
    EXPECT_EQ(third.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.
    EXPECT_EQ(third.mAmount, 100'000);
    EXPECT_EQ(third.mCurrency, Currency::EUR);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics[0].mCode, DiagnosticCode::InconsistentValue);
    EXPECT_EQ(result.mDiagnostics[0].mMessageKey, MessageKey::InconsistentValueAssetClass);
    EXPECT_TRUE(result.mDiagnostics[0].mMessageParams.empty());
    EXPECT_EQ(result.mDiagnostics[0].mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(result.mDiagnostics[0].mField, "asset_class");
    EXPECT_EQ(result.mDiagnostics[0].mTransactionId, "synthetic-fund");
}

TEST(TradeRepublicParserTest, RetainsClassConflictWhenActionPrecedesTrade) {
    const SyntheticCsvRow actionRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "FUND",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.00",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-action-first",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const SyntheticCsvRow tradeRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-trade-last",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{actionRow, tradeRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mTradeInstruments[0];

    EXPECT_EQ(instrument.mAssetClass, AssetClass::Unknown);
    ASSERT_EQ(instrument.mTransactions.size(), 1U);
    ASSERT_EQ(instrument.mCorporateActions.size(), 1U);

    const auto& action = instrument.mCorporateActions[0];
    const auto& trade = instrument.mTransactions[0];

    EXPECT_EQ(action.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    ASSERT_EQ(action.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(action.mMetadata.mSources[0].mFilename.value(), csvFile.path().filename().string());
    EXPECT_EQ(action.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(action.mMetadata.mSources[0].mTransactionId, "synthetic-action-first");
    EXPECT_EQ(action.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(action.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(action.mInstrumentEvidence[0].mSource, action.mMetadata.mSources[0]);
    EXPECT_EQ(action.mInstrumentEvidence[0].mAssetClass, AssetClass::Fund);
    EXPECT_EQ(action.mInstrumentEvidence[0].mAssetClassLabel, "FUND");
    EXPECT_EQ(action.mInstrumentEvidence[0].mName, "Synthetic Test Share");
    EXPECT_EQ(action.mType, CorporateActionType::UnresolvedSplit);
    EXPECT_EQ(action.mUnitsDelta, 100'000'000); // Source 1 unit, not a verified split change.
    EXPECT_FALSE(action.mRatio.has_value());

    EXPECT_EQ(trade.mMetadata.mTaxDate, makeDate(2024, 1, 15));
    ASSERT_EQ(trade.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(trade.mMetadata.mSources[0].mFilename.value(), csvFile.path().filename().string());
    EXPECT_EQ(trade.mMetadata.mSources[0].mSourceRow, 3U);
    EXPECT_EQ(trade.mMetadata.mSources[0].mTransactionId, "synthetic-trade-last");
    EXPECT_EQ(trade.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 1}));
    ASSERT_EQ(trade.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(trade.mInstrumentEvidence[0].mSource, trade.mMetadata.mSources[0]);
    EXPECT_EQ(trade.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(trade.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(trade.mInstrumentEvidence[0].mName, "Synthetic Test Share");
    EXPECT_EQ(trade.mUnits, 100'000'000);       // Source 1 unit at eight decimal places.
    EXPECT_EQ(trade.mUnitPrice, 1'000'000'000); // EUR 10 at eight decimal places.
    EXPECT_EQ(trade.mAmount, 100'000);
    EXPECT_EQ(trade.mCurrency, Currency::EUR);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics[0].mCode, DiagnosticCode::InconsistentValue);
    EXPECT_EQ(result.mDiagnostics[0].mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(result.mDiagnostics[0].mTransactionId, "synthetic-trade-last");
}

TEST(TradeRepublicParserTest, CompletesMissingTradeNameWithoutChangingOriginalEvidence) {
    const SyntheticCsvRow unnamedRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-unnamed",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const SyntheticCsvRow namedRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Supplied Name",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-named",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{unnamedRow, namedRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000001");
    EXPECT_EQ(instrument.mName, "Supplied Name");
    ASSERT_EQ(instrument.mTransactions.size(), 2U);
    ASSERT_EQ(instrument.mTransactions[0].mInstrumentEvidence.size(), 1U);
    ASSERT_EQ(instrument.mTransactions[1].mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(instrument.mTransactions[0].mInstrumentEvidence[0].mName, "");
    EXPECT_EQ(instrument.mTransactions[1].mInstrumentEvidence[0].mName, "Supplied Name");
    EXPECT_EQ(instrument.mTransactions[0].mInstrumentEvidence[0].mSource.mSourceRow, 2U);
    EXPECT_EQ(instrument.mTransactions[1].mInstrumentEvidence[0].mSource.mSourceRow, 3U);
    EXPECT_EQ(instrument.mTransactions[0].mUnits, 100'000'000); // 1 unit at eight decimal places.
    EXPECT_EQ(instrument.mTransactions[1].mUnits, 100'000'000);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics[0].mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(result.mDiagnostics[0].mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(result.mDiagnostics[0].mField, "name");
    EXPECT_EQ(result.mDiagnostics[0].mRowIndex, 2U);
    EXPECT_EQ(result.mDiagnostics[0].mTransactionId, "synthetic-unnamed");
}

TEST(TradeRepublicParserTest, CompletesMissingDividendNameWithoutChangingOriginalEvidence) {
    const SyntheticCsvRow unnamedRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "DIVIDEND",
        .mAssetClass = "STOCK",
        .mName = "",
        .mSymbol = "XX9000000001",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-unnamed",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const SyntheticCsvRow namedRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "DIVIDEND",
        .mAssetClass = "STOCK",
        .mName = "Supplied Name",
        .mSymbol = "XX9000000001",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-named",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{unnamedRow, namedRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    ASSERT_EQ(result.mStatement.mDividendInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mDividendInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000001");
    EXPECT_EQ(instrument.mName, "Supplied Name");
    ASSERT_EQ(instrument.mTransactions.size(), 2U);
    ASSERT_EQ(instrument.mTransactions[0].mInstrumentEvidence.size(), 1U);
    ASSERT_EQ(instrument.mTransactions[1].mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(instrument.mTransactions[0].mInstrumentEvidence[0].mName, "");
    EXPECT_EQ(instrument.mTransactions[1].mInstrumentEvidence[0].mName, "Supplied Name");
    EXPECT_EQ(instrument.mTransactions[0].mInstrumentEvidence[0].mSource.mSourceRow, 2U);
    EXPECT_EQ(instrument.mTransactions[1].mInstrumentEvidence[0].mSource.mSourceRow, 3U);
    EXPECT_EQ(instrument.mTransactions[0].mGrossAmount, 100'000); // EUR 10 at four decimal places.
    EXPECT_EQ(instrument.mTransactions[1].mGrossAmount, 100'000);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics[0].mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(result.mDiagnostics[0].mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(result.mDiagnostics[0].mField, "name");
    EXPECT_EQ(result.mDiagnostics[0].mRowIndex, 2U);
    EXPECT_EQ(result.mDiagnostics[0].mTransactionId, "synthetic-unnamed");
}

TEST(TradeRepublicParserTest, CompletesMissingBondInterestNameWithoutChangingOriginalEvidence) {
    const SyntheticCsvRow unnamedRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "BOND",
        .mName = "",
        .mSymbol = "XX9000000001",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-unnamed",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const SyntheticCsvRow namedRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "BOND",
        .mName = "Supplied Name",
        .mSymbol = "XX9000000001",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-named",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const TemporaryCsvFile csvFile{unnamedRow, namedRow};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    ASSERT_EQ(result.mStatement.mInterestInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mInterestInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000001");
    EXPECT_EQ(instrument.mName, "Supplied Name");
    ASSERT_EQ(instrument.mTransactions.size(), 2U);
    ASSERT_EQ(instrument.mTransactions[0].mInstrumentEvidence.size(), 1U);
    ASSERT_EQ(instrument.mTransactions[1].mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(instrument.mTransactions[0].mInstrumentEvidence[0].mName, "");
    EXPECT_EQ(instrument.mTransactions[1].mInstrumentEvidence[0].mName, "Supplied Name");
    EXPECT_EQ(instrument.mTransactions[0].mInstrumentEvidence[0].mSource.mSourceRow, 2U);
    EXPECT_EQ(instrument.mTransactions[1].mInstrumentEvidence[0].mSource.mSourceRow, 3U);
    EXPECT_EQ(instrument.mTransactions[0].mGrossAmount, 100'000); // EUR 10 at four decimal places.
    EXPECT_EQ(instrument.mTransactions[1].mGrossAmount, 100'000);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics[0].mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(result.mDiagnostics[0].mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(result.mDiagnostics[0].mField, "name");
    EXPECT_EQ(result.mDiagnostics[0].mRowIndex, 2U);
    EXPECT_EQ(result.mDiagnostics[0].mTransactionId, "synthetic-unnamed");
}

TEST(TradeRepublicParserTest, RejectsTradePriceWithDecimalComma) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "1,250",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U); // One row is below the file-level hint threshold.

    const auto& error = result.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsTradePriceWithDecimalComma.csv");
    EXPECT_EQ(error.mField, "price");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-test-transaction");
}

TEST(TradeRepublicParserTest, RejectsTradeSharesWithDecimalComma) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "0,125",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U); // One row is below the file-level hint threshold.

    const auto& error = result.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsTradeSharesWithDecimalComma.csv");
    EXPECT_EQ(error.mField, "shares");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-test-transaction");
}

TEST(TradeRepublicParserTest, RejectsTradeAmountWithDecimalComma) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-1,250",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U); // One row is below the file-level hint threshold.

    const auto& error = result.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsTradeAmountWithDecimalComma.csv");
    EXPECT_EQ(error.mField, "amount");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-test-transaction");
}

TEST(TradeRepublicParserTest, RejectsTradeFeeWithDecimalComma) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "-1,000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U); // One row is below the file-level hint threshold.

    const auto& error = result.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsTradeFeeWithDecimalComma.csv");
    EXPECT_EQ(error.mField, "fee");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-test-transaction");
}

TEST(TradeRepublicParserTest, RejectsSplitSharesWithDecimalComma) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "0,125",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-transaction",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U); // One row is below the file-level hint threshold.

    const auto& error = result.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsSplitSharesWithDecimalComma.csv");
    EXPECT_EQ(error.mField, "shares");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-split-transaction");
}

TEST(TradeRepublicParserTest, RejectsDividendAmountWithDecimalComma) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "DIVIDEND",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "",
        .mPrice = "",
        .mAmount = "1,250",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-dividend-transaction",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U); // One row is below the file-level hint threshold.

    const auto& error = result.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDividendAmountWithDecimalComma.csv");
    EXPECT_EQ(error.mField, "amount");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-dividend-transaction");
}

TEST(TradeRepublicParserTest, RejectsDividendOriginalAmountWithDecimalComma) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "DIVIDEND",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "",
        .mPrice = "",
        .mAmount = "1.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "1,250",
        .mOriginalCurrency = "USD",
        .mFxRate = "0.90000000",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-dividend-transaction",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U); // One row is below the file-level hint threshold.

    const auto& error = result.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(
        error.mSourceFile,
        "taxbroker_TradeRepublicParserTest_RejectsDividendOriginalAmountWithDecimalComma.csv");
    EXPECT_EQ(error.mField, "original_amount");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-dividend-transaction");
}

TEST(TradeRepublicParserTest, RejectsDividendTaxWithDecimalComma) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "DIVIDEND",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.00",
        .mFee = "",
        .mTax = "-0,375",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-dividend-transaction",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U); // One row is below the file-level hint threshold.

    const auto& error = result.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDividendTaxWithDecimalComma.csv");
    EXPECT_EQ(error.mField, "tax");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-dividend-transaction");
}

TEST(TradeRepublicParserTest, RejectsDividendFxRateWithDecimalComma) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "DIVIDEND",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "",
        .mPrice = "",
        .mAmount = "1.00",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "1.00",
        .mOriginalCurrency = "USD",
        .mFxRate = "0,920",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-dividend-transaction",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U); // One row is below the file-level hint threshold.

    const auto& error = result.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsDividendFxRateWithDecimalComma.csv");
    EXPECT_EQ(error.mField, "fx_rate");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-dividend-transaction");
}

TEST(TradeRepublicParserTest, RejectsInterestAmountWithDecimalComma) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "",
        .mName = "",
        .mSymbol = "",
        .mShares = "",
        .mPrice = "",
        .mAmount = "1,250",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-interest-transaction",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U); // One row is below the file-level hint threshold.

    const auto& error = result.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsInterestAmountWithDecimalComma.csv");
    EXPECT_EQ(error.mField, "amount");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-interest-transaction");
}

TEST(TradeRepublicParserTest, RejectsBenefitAmountWithDecimalComma) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "BENEFITS_SAVEBACK",
        .mAssetClass = "",
        .mName = "",
        .mSymbol = "",
        .mShares = "",
        .mPrice = "",
        .mAmount = "1,250",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-benefit-transaction",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U); // One row is below the file-level hint threshold.

    const auto& error = result.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsBenefitAmountWithDecimalComma.csv");
    EXPECT_EQ(error.mField, "amount");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-benefit-transaction");
}

TEST(TradeRepublicParserTest, RejectsPrivateMarketAmountWithDecimalComma) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "PRIVATE_MARKET_BUY",
        .mAssetClass = "PRIVATE_FUND",
        .mName = "Synthetic Test Private Fund",
        .mSymbol = "XX9000000003",
        .mShares = "",
        .mPrice = "",
        .mAmount = "-1,250",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-private-market-transaction",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U); // One row is below the file-level hint threshold.

    const auto& error = result.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsPrivateMarketAmountWithDecimalComma.csv");
    EXPECT_EQ(error.mField, "amount");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-private-market-transaction");
}

TEST(TradeRepublicParserTest, RejectsPrivateMarketFeeWithDecimalComma) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "PRIVATE_MARKET_BUY",
        .mAssetClass = "PRIVATE_FUND",
        .mName = "Synthetic Test Private Fund",
        .mSymbol = "XX9000000003",
        .mShares = "",
        .mPrice = "",
        .mAmount = "-100.00",
        .mFee = "-1,000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-private-market-transaction",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U); // One row is below the file-level hint threshold.

    const auto& error = result.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsPrivateMarketFeeWithDecimalComma.csv");
    EXPECT_EQ(error.mField, "fee");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-private-market-transaction");
}

TEST(TradeRepublicParserTest, RejectsTradePriceInParentheses) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "(10.5)",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U); // One row is below the file-level hint threshold.

    const auto& error = result.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsTradePriceInParentheses.csv");
    EXPECT_EQ(error.mField, "price");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-test-transaction");
}

TEST(TradeRepublicParserTest, RejectsTradePriceWithThousandsSeparator) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "1,234.5",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U); // One row is below the file-level hint threshold.

    const auto& error = result.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsTradePriceWithThousandsSeparator.csv");
    EXPECT_EQ(error.mField, "price");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-test-transaction");
}

TEST(TradeRepublicParserTest, RejectsTradePriceWithEuropeanGrouping) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "1.000,50",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U); // One row is below the file-level hint threshold.

    const auto& error = result.mDiagnostics.front();

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mSourceFile,
              "taxbroker_TradeRepublicParserTest_RejectsTradePriceWithEuropeanGrouping.csv");
    EXPECT_EQ(error.mField, "price");
    EXPECT_EQ(error.mRowIndex, 2U);
    EXPECT_EQ(error.mTransactionId, "synthetic-test-transaction");
}

TEST(TradeRepublicParserTest, DoesNotWarnAboutLocalizedNumbersBelowThreeRows) {
    const SyntheticCsvRow validBuy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-valid-buy",
    };
    const SyntheticCsvRow commaBuy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10,5",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-comma-buy",
    };
    const SyntheticCsvRow commaDividend{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "DIVIDEND",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "",
        .mPrice = "",
        .mAmount = "1,250",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-comma-dividend",
    };
    const SyntheticCsvRow validSell{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "SELL",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "-1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-valid-sell",
    };

    const auto result = parseRows({validBuy, commaBuy, commaDividend, validSell});

    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);
    EXPECT_EQ(result.mStatement.mTradeInstruments.front().mTransactions.size(),
              2U); // BUY and SELL.
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 2U); // Two bad rows are below the hint threshold of 3.
    EXPECT_EQ(result.mDiagnostics[0].mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(result.mDiagnostics[0].mField, "price");
    EXPECT_EQ(result.mDiagnostics[0].mRowIndex, 3U);
    EXPECT_EQ(result.mDiagnostics[1].mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(result.mDiagnostics[1].mField, "amount");
    EXPECT_EQ(result.mDiagnostics[1].mRowIndex, 4U);
    EXPECT_EQ(countDiagnostics(result, DiagnosticCode::LocalizedNumberFormat), 0U);
}

TEST(TradeRepublicParserTest, WarnsOnceAboutLocalizedNumbersFromThreeRows) {
    const SyntheticCsvRow validBuy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-valid-buy",
    };
    const SyntheticCsvRow commaBuy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10,5",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-comma-buy",
    };
    const SyntheticCsvRow commaDividend{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "DIVIDEND",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "",
        .mPrice = "",
        .mAmount = "1,250",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-comma-dividend",
    };
    const SyntheticCsvRow parenthesesBuy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "(10.5)",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-parentheses-buy",
    };
    const SyntheticCsvRow validSell{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "SELL",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "-1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-valid-sell",
    };

    const TemporaryCsvFile csvFile{validBuy, commaBuy, commaDividend, parenthesesBuy, validSell};
    tr::TradeRepublicParser parser;

    const auto result = parser.parse(csvFile.path());

    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);
    EXPECT_EQ(result.mStatement.mTradeInstruments.front().mTransactions.size(),
              2U); // BUY and SELL.

    ASSERT_EQ(result.mDiagnostics.size(), 4U); // Three row errors, then one file-level warning.
    EXPECT_EQ(result.mDiagnostics[0].mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(result.mDiagnostics[0].mRowIndex, 3U);
    EXPECT_EQ(result.mDiagnostics[1].mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(result.mDiagnostics[1].mRowIndex, 4U);
    EXPECT_EQ(result.mDiagnostics[2].mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(result.mDiagnostics[2].mRowIndex, 5U);

    const auto& warning = result.mDiagnostics[3];

    EXPECT_EQ(warning.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(warning.mCode, DiagnosticCode::LocalizedNumberFormat);
    EXPECT_EQ(warning.mMessageKey, MessageKey::LocalizedNumberFormat);
    EXPECT_EQ(warning.mMessageParams, (std::vector<MessageParam>{numberParam("rowCount", 3U)}));
    EXPECT_EQ(warning.mSourceFile,
              "taxbroker_TradeRepublicParserTest_WarnsOnceAboutLocalizedNumbersFromThreeRows.csv");
    EXPECT_FALSE(warning.mRowIndex.has_value());
    EXPECT_FALSE(warning.mField.has_value());
    EXPECT_FALSE(warning.mTransactionId.has_value());
    EXPECT_TRUE(hasDiagnosticContaining(result, "3 rows have a number with a comma"));
}

TEST(TradeRepublicParserTest, CountsOnlyLocalizedNumbersTowardsTheWarning) {
    const SyntheticCsvRow commaBuy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10,5",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-comma-buy",
    };
    const SyntheticCsvRow commaDividend{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "DIVIDEND",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "",
        .mPrice = "",
        .mAmount = "1,250",
        .mFee = "",
        .mTax = "-1.00",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-comma-dividend",
    };
    const SyntheticCsvRow textBuy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "not-a-number",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-text-buy",
    };
    const SyntheticCsvRow dateBuy{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "15,01,2024",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-date-buy",
    };

    const auto result = parseRows({commaBuy, commaDividend, textBuy, dateBuy});

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());

    ASSERT_EQ(result.mDiagnostics.size(), 4U); // Four row errors; only two use a comma in a number.
    EXPECT_EQ(countDiagnostics(result, DiagnosticCode::InvalidValue), 4U);
    EXPECT_EQ(countDiagnostics(result, DiagnosticCode::LocalizedNumberFormat), 0U);
}

TEST(TradeRepublicParserTest, ReportsEveryInvalidFieldOfATradeRow) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-13-01",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "0,5",
        .mPrice = "abc",
        .mAmount = "-10.000000",
        .mFee = "1.00",
        .mTax = "",
        .mCurrency = "XXX",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(),
              5U); // Five invalid fields; a positive fee and a month 13 are invalid.

    const auto& dateError = result.mDiagnostics[0];

    EXPECT_EQ(dateError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(dateError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(dateError.mField, "date");
    EXPECT_EQ(dateError.mRowIndex, 2U);
    EXPECT_EQ(dateError.mTransactionId, "synthetic-test-transaction");
    EXPECT_EQ(dateError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_ReportsEveryInvalidFieldOfATradeRow.csv");

    const auto& sharesError = result.mDiagnostics[1];

    EXPECT_EQ(sharesError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(sharesError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(sharesError.mField, "shares");
    EXPECT_EQ(sharesError.mRowIndex, 2U);
    EXPECT_EQ(sharesError.mTransactionId, "synthetic-test-transaction");
    EXPECT_EQ(sharesError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_ReportsEveryInvalidFieldOfATradeRow.csv");

    const auto& priceError = result.mDiagnostics[2];

    EXPECT_EQ(priceError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(priceError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(priceError.mField, "price");
    EXPECT_EQ(priceError.mRowIndex, 2U);
    EXPECT_EQ(priceError.mTransactionId, "synthetic-test-transaction");
    EXPECT_EQ(priceError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_ReportsEveryInvalidFieldOfATradeRow.csv");

    const auto& feeError = result.mDiagnostics[3];

    EXPECT_EQ(feeError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(feeError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(feeError.mField, "fee");
    EXPECT_EQ(feeError.mRowIndex, 2U);
    EXPECT_EQ(feeError.mTransactionId, "synthetic-test-transaction");
    EXPECT_EQ(feeError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_ReportsEveryInvalidFieldOfATradeRow.csv");

    const auto& currencyError = result.mDiagnostics[4];

    EXPECT_EQ(currencyError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(currencyError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(currencyError.mField, "currency");
    EXPECT_EQ(currencyError.mRowIndex, 2U);
    EXPECT_EQ(currencyError.mTransactionId, "synthetic-test-transaction");
    EXPECT_EQ(currencyError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_ReportsEveryInvalidFieldOfATradeRow.csv");
}

TEST(TradeRepublicParserTest, ReportsFieldErrorsAroundAMissingSymbolAndNameOfATradeRow) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-13-01", // Invalid month: checked before the instrument.
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "",
        .mSymbol = "",
        .mShares = "abc",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "0.00",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(),
              3U); // A bad date, missing symbol/name, and invalid shares still get reported.

    const auto& dateError = result.mDiagnostics[0];

    EXPECT_EQ(dateError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(dateError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(dateError.mField, "date");
    EXPECT_EQ(dateError.mRowIndex, 2U);
    EXPECT_EQ(dateError.mTransactionId, "synthetic-test-transaction");
    EXPECT_EQ(dateError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_"
              "ReportsFieldErrorsAroundAMissingSymbolAndNameOfATradeRow.csv");

    const auto& instrumentError = result.mDiagnostics[1];

    EXPECT_EQ(instrumentError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(instrumentError.mCode, DiagnosticCode::MissingField);
    EXPECT_FALSE(instrumentError.mField.has_value()); // No single field; symbol and name both.
    EXPECT_EQ(instrumentError.mRowIndex, 2U);
    EXPECT_EQ(instrumentError.mTransactionId, "synthetic-test-transaction");
    EXPECT_EQ(instrumentError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_"
              "ReportsFieldErrorsAroundAMissingSymbolAndNameOfATradeRow.csv");

    const auto& sharesError = result.mDiagnostics[2];

    EXPECT_EQ(sharesError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(sharesError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(sharesError.mField, "shares");
    EXPECT_EQ(sharesError.mRowIndex, 2U);
    EXPECT_EQ(sharesError.mTransactionId, "synthetic-test-transaction");
    EXPECT_EQ(sharesError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_"
              "ReportsFieldErrorsAroundAMissingSymbolAndNameOfATradeRow.csv");
}

TEST(TradeRepublicParserTest, ReportsFieldErrorsBeforeTheSignCheck) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "SELL",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "10.000000",
        .mFee = "abc",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(),
              2U); // The fee error comes first; a positive share count is wrong for a sale.

    const auto& feeError = result.mDiagnostics[0];

    EXPECT_EQ(feeError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(feeError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(feeError.mField, "fee");
    EXPECT_EQ(feeError.mRowIndex, 2U);
    EXPECT_EQ(feeError.mTransactionId, "synthetic-test-transaction");
    EXPECT_EQ(feeError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_ReportsFieldErrorsBeforeTheSignCheck.csv");

    const auto& signError = result.mDiagnostics[1];

    EXPECT_EQ(signError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(signError.mCode, DiagnosticCode::InconsistentValue);
    EXPECT_EQ(signError.mField, "shares");
    EXPECT_EQ(signError.mRowIndex, 2U);
    EXPECT_EQ(signError.mTransactionId, "synthetic-test-transaction");
    EXPECT_EQ(signError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_ReportsFieldErrorsBeforeTheSignCheck.csv");
}

TEST(TradeRepublicParserTest, SkipsTheSignCheckWhenSharesAreInvalid) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "SELL",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "x",
        .mPrice = "10.000000",
        .mAmount = "10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(),
              1U); // Only the invalid shares; no sign check without a valid share count.

    const auto& sharesError = result.mDiagnostics[0];

    EXPECT_EQ(sharesError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(sharesError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(sharesError.mField, "shares");
    EXPECT_EQ(sharesError.mRowIndex, 2U);
    EXPECT_EQ(sharesError.mTransactionId, "synthetic-test-transaction");
    EXPECT_EQ(sharesError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_SkipsTheSignCheckWhenSharesAreInvalid.csv");
}

TEST(TradeRepublicParserTest, ReportsEveryInvalidFieldOfADividendRow) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "15.01.2024",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "DIVIDEND",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "",
        .mPrice = "",
        .mAmount = "1.00",
        .mFee = "",
        .mTax = "abc",
        .mCurrency = "XXX",
        .mOriginalAmount = "1,25",
        .mOriginalCurrency = "USD",
        .mFxRate = "x",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-dividend-transaction",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(),
              5U); // Five invalid fields in column order; original_currency USD is valid.

    const auto& dateError = result.mDiagnostics[0];

    EXPECT_EQ(dateError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(dateError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(dateError.mField, "date");
    EXPECT_EQ(dateError.mRowIndex, 2U);
    EXPECT_EQ(dateError.mTransactionId, "synthetic-dividend-transaction");
    EXPECT_EQ(dateError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_ReportsEveryInvalidFieldOfADividendRow.csv");

    const auto& taxError = result.mDiagnostics[1];

    EXPECT_EQ(taxError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(taxError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(taxError.mField, "tax");
    EXPECT_EQ(taxError.mRowIndex, 2U);
    EXPECT_EQ(taxError.mTransactionId, "synthetic-dividend-transaction");
    EXPECT_EQ(taxError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_ReportsEveryInvalidFieldOfADividendRow.csv");

    const auto& currencyError = result.mDiagnostics[2];

    EXPECT_EQ(currencyError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(currencyError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(currencyError.mField, "currency");
    EXPECT_EQ(currencyError.mRowIndex, 2U);
    EXPECT_EQ(currencyError.mTransactionId, "synthetic-dividend-transaction");
    EXPECT_EQ(currencyError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_ReportsEveryInvalidFieldOfADividendRow.csv");

    const auto& originalAmountError = result.mDiagnostics[3];

    EXPECT_EQ(originalAmountError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(originalAmountError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(originalAmountError.mField, "original_amount");
    EXPECT_EQ(originalAmountError.mRowIndex, 2U);
    EXPECT_EQ(originalAmountError.mTransactionId, "synthetic-dividend-transaction");
    EXPECT_EQ(originalAmountError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_ReportsEveryInvalidFieldOfADividendRow.csv");

    const auto& rateError = result.mDiagnostics[4];

    EXPECT_EQ(rateError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(rateError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(rateError.mField, "fx_rate");
    EXPECT_EQ(rateError.mRowIndex, 2U);
    EXPECT_EQ(rateError.mTransactionId, "synthetic-dividend-transaction");
    EXPECT_EQ(rateError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_ReportsEveryInvalidFieldOfADividendRow.csv");
}

TEST(TradeRepublicParserTest, ReportsAnUnknownCurrencyOnceForAPlainDividend) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "DIVIDEND",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "",
        .mPrice = "",
        .mAmount = "1.250000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "XXX",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-dividend-transaction",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(),
              1U); // The currency column is also the amount currency, and is reported once.

    const auto& currencyError = result.mDiagnostics[0];

    EXPECT_EQ(currencyError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(currencyError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(currencyError.mField, "currency");
    EXPECT_EQ(currencyError.mRowIndex, 2U);
    EXPECT_EQ(currencyError.mTransactionId, "synthetic-dividend-transaction");
    EXPECT_EQ(
        currencyError.mSourceFile,
        "taxbroker_TradeRepublicParserTest_ReportsAnUnknownCurrencyOnceForAPlainDividend.csv");
}

TEST(TradeRepublicParserTest, ReportsEveryInvalidFieldOfABondInterestRow) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "x",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "BOND",
        .mName = "Synthetic Test Bond",
        .mSymbol = "",
        .mShares = "",
        .mPrice = "",
        .mAmount = "-1.00",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-interest-transaction",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 3U); // A bad date, a missing ISIN and a negative amount.

    const auto& dateError = result.mDiagnostics[0];

    EXPECT_EQ(dateError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(dateError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(dateError.mMessageKey, MessageKey::InvalidValueField);
    EXPECT_EQ(dateError.mMessageParams,
              (std::vector<MessageParam>{textParam("field", "date"),
                                         termParam("rowKind", term::kRowKindBondInterest)}));
    EXPECT_EQ(dateError.mField, "date");
    EXPECT_EQ(dateError.mRowIndex, 2U);
    EXPECT_EQ(dateError.mTransactionId, "synthetic-interest-transaction");
    EXPECT_EQ(dateError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_ReportsEveryInvalidFieldOfABondInterestRow.csv");

    const auto& symbolError = result.mDiagnostics[1];

    EXPECT_EQ(symbolError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(symbolError.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(symbolError.mMessageKey, MessageKey::MissingFieldRequiredField);
    EXPECT_EQ(symbolError.mMessageParams,
              (std::vector<MessageParam>{textParam("field", "symbol"),
                                         termParam("rowKind", term::kRowKindBondInterest)}));
    EXPECT_EQ(symbolError.mField, "symbol");
    EXPECT_EQ(symbolError.mRowIndex, 2U);
    EXPECT_EQ(symbolError.mTransactionId, "synthetic-interest-transaction");
    EXPECT_EQ(symbolError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_ReportsEveryInvalidFieldOfABondInterestRow.csv");

    const auto& amountError = result.mDiagnostics[2];

    EXPECT_EQ(amountError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(amountError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(amountError.mMessageKey, MessageKey::InvalidValueField);
    EXPECT_EQ(amountError.mMessageParams,
              (std::vector<MessageParam>{textParam("field", "amount"),
                                         termParam("rowKind", term::kRowKindBondInterest)}));
    EXPECT_EQ(amountError.mField, "amount");
    EXPECT_EQ(amountError.mRowIndex, 2U);
    EXPECT_EQ(amountError.mTransactionId, "synthetic-interest-transaction");
    EXPECT_EQ(amountError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_ReportsEveryInvalidFieldOfABondInterestRow.csv");
}

TEST(TradeRepublicParserTest, ReportsEveryInvalidFieldOfASplitRow) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "x",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "0",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-split-transaction",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 2U); // A bad date and a share count of zero.

    const auto& dateError = result.mDiagnostics[0];

    EXPECT_EQ(dateError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(dateError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(dateError.mField, "date");
    EXPECT_EQ(dateError.mRowIndex, 2U);
    EXPECT_EQ(dateError.mTransactionId, "synthetic-split-transaction");
    EXPECT_EQ(dateError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_ReportsEveryInvalidFieldOfASplitRow.csv");

    const auto& sharesError = result.mDiagnostics[1];

    EXPECT_EQ(sharesError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(sharesError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(sharesError.mField, "shares");
    EXPECT_EQ(sharesError.mRowIndex, 2U);
    EXPECT_EQ(sharesError.mTransactionId, "synthetic-split-transaction");
    EXPECT_EQ(sharesError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_ReportsEveryInvalidFieldOfASplitRow.csv");
}

TEST(TradeRepublicParserTest, ReportsEveryInvalidFieldOfABenefitRow) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "x",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "BENEFITS_SAVEBACK",
        .mAssetClass = "",
        .mName = "",
        .mSymbol = "",
        .mShares = "",
        .mPrice = "",
        .mAmount = "abc",
        .mFee = "",
        .mTax = "",
        .mCurrency = "XXX",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-benefit-transaction",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mBenefitEvents.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 3U); // A bad date, amount and currency.

    const auto& dateError = result.mDiagnostics[0];

    EXPECT_EQ(dateError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(dateError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(dateError.mField, "date");
    EXPECT_EQ(dateError.mRowIndex, 2U);
    EXPECT_EQ(dateError.mTransactionId, "synthetic-benefit-transaction");
    EXPECT_EQ(dateError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_ReportsEveryInvalidFieldOfABenefitRow.csv");

    const auto& amountError = result.mDiagnostics[1];

    EXPECT_EQ(amountError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(amountError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(amountError.mField, "amount");
    EXPECT_EQ(amountError.mRowIndex, 2U);
    EXPECT_EQ(amountError.mTransactionId, "synthetic-benefit-transaction");
    EXPECT_EQ(amountError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_ReportsEveryInvalidFieldOfABenefitRow.csv");

    const auto& currencyError = result.mDiagnostics[2];

    EXPECT_EQ(currencyError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(currencyError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(currencyError.mField, "currency");
    EXPECT_EQ(currencyError.mRowIndex, 2U);
    EXPECT_EQ(currencyError.mTransactionId, "synthetic-benefit-transaction");
    EXPECT_EQ(currencyError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_ReportsEveryInvalidFieldOfABenefitRow.csv");
}

TEST(TradeRepublicParserTest, ReportsEveryInvalidFieldOfAPrivateMarketRow) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "x",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "PRIVATE_MARKET_BUY",
        .mAssetClass = "PRIVATE_FUND",
        .mName = "Synthetic Test Private Fund",
        .mSymbol = "XX9000000003",
        .mShares = "",
        .mPrice = "",
        .mAmount = "abc",
        .mFee = "1.00",
        .mTax = "",
        .mCurrency = "XXX",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-private-market-transaction",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mPrivateMarketEvents.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 4U); // A bad date, amount, fee and currency.

    const auto& dateError = result.mDiagnostics[0];

    EXPECT_EQ(dateError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(dateError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(dateError.mField, "date");
    EXPECT_EQ(dateError.mRowIndex, 2U);
    EXPECT_EQ(dateError.mTransactionId, "synthetic-private-market-transaction");
    EXPECT_EQ(dateError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_ReportsEveryInvalidFieldOfAPrivateMarketRow.csv");

    const auto& amountError = result.mDiagnostics[1];

    EXPECT_EQ(amountError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(amountError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(amountError.mField, "amount");
    EXPECT_EQ(amountError.mRowIndex, 2U);
    EXPECT_EQ(amountError.mTransactionId, "synthetic-private-market-transaction");
    EXPECT_EQ(amountError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_ReportsEveryInvalidFieldOfAPrivateMarketRow.csv");

    const auto& feeError = result.mDiagnostics[2];

    EXPECT_EQ(feeError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(feeError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(feeError.mField, "fee");
    EXPECT_EQ(feeError.mRowIndex, 2U);
    EXPECT_EQ(feeError.mTransactionId, "synthetic-private-market-transaction");
    EXPECT_EQ(feeError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_ReportsEveryInvalidFieldOfAPrivateMarketRow.csv");

    const auto& currencyError = result.mDiagnostics[3];

    EXPECT_EQ(currencyError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(currencyError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(currencyError.mField, "currency");
    EXPECT_EQ(currencyError.mRowIndex, 2U);
    EXPECT_EQ(currencyError.mTransactionId, "synthetic-private-market-transaction");
    EXPECT_EQ(currencyError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_ReportsEveryInvalidFieldOfAPrivateMarketRow.csv");
}

TEST(TradeRepublicParserTest, ReportsInvalidFieldsAfterAMissingDatetime) {
    const SyntheticCsvRow row{
        .mDatetime = "",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "abc",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(),
              2U); // The missing datetime first, then the invalid price.

    const auto& datetimeError = result.mDiagnostics[0];

    EXPECT_EQ(datetimeError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(datetimeError.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(datetimeError.mField, "datetime");
    EXPECT_EQ(datetimeError.mRowIndex, 2U);
    EXPECT_EQ(datetimeError.mTransactionId, "synthetic-test-transaction");
    EXPECT_EQ(datetimeError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_ReportsInvalidFieldsAfterAMissingDatetime.csv");

    const auto& priceError = result.mDiagnostics[1];

    EXPECT_EQ(priceError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(priceError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(priceError.mField, "price");
    EXPECT_EQ(priceError.mRowIndex, 2U);
    EXPECT_EQ(priceError.mTransactionId, "synthetic-test-transaction");
    EXPECT_EQ(priceError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_ReportsInvalidFieldsAfterAMissingDatetime.csv");
}

TEST(TradeRepublicParserTest, ReportsInvalidFieldsAfterAMissingDatetimeForAnUnknownInterestRow) {
    const SyntheticCsvRow row{
        .mDatetime = "",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "FUND", // Not "BOND" and not empty, so the interest kind is unknown.
        .mName = "",
        .mSymbol = "",
        .mShares = "",
        .mPrice = "",
        .mAmount = "1.00",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-interest-transaction",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mInterestInstruments.empty());
    ASSERT_EQ(result.mDiagnostics.size(),
              2U); // The missing datetime first; the unknown interest kind still stops the row.

    const auto& datetimeError = result.mDiagnostics[0];

    EXPECT_EQ(datetimeError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(datetimeError.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(datetimeError.mField, "datetime");
    EXPECT_EQ(datetimeError.mRowIndex, 2U);
    EXPECT_EQ(datetimeError.mTransactionId, "synthetic-interest-transaction");
    EXPECT_EQ(datetimeError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_"
              "ReportsInvalidFieldsAfterAMissingDatetimeForAnUnknownInterestRow.csv");

    const auto& assetClassError = result.mDiagnostics[1];

    EXPECT_EQ(assetClassError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(assetClassError.mCode, DiagnosticCode::UnsupportedAssetClass);
    EXPECT_EQ(assetClassError.mField, "asset_class");
    EXPECT_EQ(assetClassError.mRowIndex, 2U);
    EXPECT_EQ(assetClassError.mTransactionId, "synthetic-interest-transaction");
    EXPECT_EQ(assetClassError.mSourceFile,
              "taxbroker_TradeRepublicParserTest_"
              "ReportsInvalidFieldsAfterAMissingDatetimeForAnUnknownInterestRow.csv");
}

TEST(TradeRepublicParserTest, CountsRowsNotFieldsForTheLocalizedNumberWarning) {
    const SyntheticCsvRow commaPriceAndAmount{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10,5",
        .mAmount = "-1,250",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-buy-first",
    };

    const SyntheticCsvRow commaShares{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "0,5",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-buy-second",
    };

    const SyntheticCsvRow commaDividend{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "DIVIDEND",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "",
        .mPrice = "",
        .mAmount = "1,250",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-dividend-third",
    };

    const auto result = parseRows({commaPriceAndAmount, commaShares, commaDividend});

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    EXPECT_TRUE(result.mStatement.mDividendInstruments.empty());
    // Row 2 has two comma numbers, rows 3 and 4 one each: 4 errors but 3 rows.
    ASSERT_EQ(result.mDiagnostics.size(), 5U);

    const auto& priceError = result.mDiagnostics[0];

    EXPECT_EQ(priceError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(priceError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(priceError.mField, "price");
    EXPECT_EQ(priceError.mRowIndex, 2U);
    EXPECT_EQ(priceError.mTransactionId, "synthetic-buy-first");
    EXPECT_EQ(
        priceError.mSourceFile,
        "taxbroker_TradeRepublicParserTest_CountsRowsNotFieldsForTheLocalizedNumberWarning.csv");

    const auto& amountError = result.mDiagnostics[1];

    EXPECT_EQ(amountError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(amountError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(amountError.mField, "amount");
    EXPECT_EQ(amountError.mRowIndex, 2U);
    EXPECT_EQ(amountError.mTransactionId, "synthetic-buy-first");
    EXPECT_EQ(
        amountError.mSourceFile,
        "taxbroker_TradeRepublicParserTest_CountsRowsNotFieldsForTheLocalizedNumberWarning.csv");

    const auto& sharesError = result.mDiagnostics[2];

    EXPECT_EQ(sharesError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(sharesError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(sharesError.mField, "shares");
    EXPECT_EQ(sharesError.mRowIndex, 3U);
    EXPECT_EQ(sharesError.mTransactionId, "synthetic-buy-second");
    EXPECT_EQ(
        sharesError.mSourceFile,
        "taxbroker_TradeRepublicParserTest_CountsRowsNotFieldsForTheLocalizedNumberWarning.csv");

    const auto& dividendAmountError = result.mDiagnostics[3];

    EXPECT_EQ(dividendAmountError.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(dividendAmountError.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(dividendAmountError.mField, "amount");
    EXPECT_EQ(dividendAmountError.mRowIndex, 4U);
    EXPECT_EQ(dividendAmountError.mTransactionId, "synthetic-dividend-third");
    EXPECT_EQ(
        dividendAmountError.mSourceFile,
        "taxbroker_TradeRepublicParserTest_CountsRowsNotFieldsForTheLocalizedNumberWarning.csv");

    const auto& warning = result.mDiagnostics[4];

    EXPECT_EQ(warning.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(warning.mCode, DiagnosticCode::LocalizedNumberFormat);
    EXPECT_EQ(
        warning.mSourceFile,
        "taxbroker_TradeRepublicParserTest_CountsRowsNotFieldsForTheLocalizedNumberWarning.csv");
    EXPECT_FALSE(warning.mRowIndex.has_value());
    EXPECT_FALSE(warning.mField.has_value());
    EXPECT_FALSE(warning.mTransactionId.has_value());
    EXPECT_TRUE(hasDiagnosticContaining(result, "3 rows have a number with a comma"));
}

TEST(TradeRepublicParserTest, DoesNotWarnForOneRowWithSeveralCommaNumbers) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "0,5",
        .mPrice = "10,5",
        .mAmount = "-1,250",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
    };

    const auto result = parseRows({row});

    EXPECT_TRUE(result.mStatement.mTradeInstruments.empty());
    // Three comma numbers in one row are still one row, below the threshold of 3.
    ASSERT_EQ(result.mDiagnostics.size(), 3U);
    EXPECT_EQ(countDiagnostics(result, DiagnosticCode::InvalidValue), 3U);
    EXPECT_EQ(countDiagnostics(result, DiagnosticCode::LocalizedNumberFormat), 0U);
}

TEST(TradeRepublicParserTest, KeepsTheIdentityRateForAnEurTrade) {
    // A plain EUR trade without fx_rate keeps the identity rate 1.0.
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const ParseResult result = parseRows({row});

    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& trade = result.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(trade.mExchangeRate, 100'000'000); // 1.0 at eight decimals.
    EXPECT_EQ(trade.mCurrency, Currency::EUR);
    EXPECT_EQ(trade.mFeeCurrency, Currency::EUR);

    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(TradeRepublicParserTest, StoresNoRateForAForeignTradeWithoutFxRate) {
    // The issue case: a USD trade without any rate in the source must not get 1.0.
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "2.0000000000",
        .mPrice = "10.00",
        .mAmount = "-20.00",
        .mFee = "-1.00",
        .mTax = "",
        .mCurrency = "USD",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const ParseResult result = parseRows({row});

    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& trade = result.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_FALSE(trade.mExchangeRate.has_value());
    EXPECT_EQ(trade.mCurrency, Currency::USD);
    EXPECT_EQ(trade.mFeeCurrency, Currency::USD);
    EXPECT_EQ(trade.mUnitPrice, 1'000'000'000); // USD 10.00 at eight decimals.
    EXPECT_EQ(trade.mUnits, 200'000'000);       // 2 units at eight decimals.
    ASSERT_TRUE(trade.mAmount.has_value());
    EXPECT_EQ(*trade.mAmount, 200'000); // USD 20.00, buy sign removed.
    EXPECT_EQ(trade.mFeePaid, 10'000);  // USD 1.00.

    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(TradeRepublicParserTest, KeepsTheForeignFactsAndBrokerRateOfAForeignBuy) {
    // Shaped like a foreign dividend: the amount is in original_currency, the fee in currency.
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "10.0000000000",
        .mPrice = "10.00",
        .mAmount = "-90.00",
        .mFee = "-1.00",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "-100.00",
        .mOriginalCurrency = "USD",
        .mFxRate = "0.9",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const ParseResult result = parseRows({row});

    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& trade = result.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(trade.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(trade.mCurrency, Currency::USD);
    EXPECT_EQ(trade.mFeeCurrency, Currency::EUR);
    ASSERT_TRUE(trade.mAmount.has_value());
    EXPECT_EQ(*trade.mAmount, 1'000'000);       // USD 100.00 from original_amount, not EUR 90.00.
    EXPECT_EQ(trade.mUnitPrice, 1'000'000'000); // 10.00 at eight decimals.
    EXPECT_EQ(trade.mFeePaid, 10'000);          // EUR 1.00.
    EXPECT_EQ(trade.mExchangeRate, 90'000'000); // 0.9 at eight decimals.

    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(TradeRepublicParserTest, StoresNoRateForAForeignBuyWithoutFxRate) {
    // Like a foreign dividend: an empty fx_rate is not an error and invents no rate.
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "10.0000000000",
        .mPrice = "10.00",
        .mAmount = "-90.00",
        .mFee = "-1.00",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "-100.00",
        .mOriginalCurrency = "USD",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const ParseResult result = parseRows({row});

    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& trade = result.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_FALSE(trade.mExchangeRate.has_value());
    EXPECT_EQ(trade.mCurrency, Currency::USD);
    ASSERT_TRUE(trade.mAmount.has_value());
    EXPECT_EQ(*trade.mAmount, 1'000'000); // USD 100.00.

    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(TradeRepublicParserTest, KeepsTheForeignFactsAndBrokerRateOfAForeignSell) {
    // A sell has a positive original_amount.
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "SELL",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "-10.0000000000",
        .mPrice = "10.00",
        .mAmount = "90.00",
        .mFee = "-1.00",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "100.00",
        .mOriginalCurrency = "USD",
        .mFxRate = "0.9",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const ParseResult result = parseRows({row});

    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& trade = result.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(trade.mTradeSide, TradeSide::Sell);
    EXPECT_EQ(trade.mUnits, 1'000'000'000); // 10 units at eight decimals, sell sign removed.
    EXPECT_EQ(trade.mCurrency, Currency::USD);
    ASSERT_TRUE(trade.mAmount.has_value());
    EXPECT_EQ(*trade.mAmount, 1'000'000); // USD 100.00.
    EXPECT_EQ(trade.mExchangeRate, 90'000'000);

    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(TradeRepublicParserTest, KeepsASuppliedRateOnAnEurTrade) {
    // A supplied rate is kept as given, even for EUR.
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "1.0",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const ParseResult result = parseRows({row});

    ASSERT_EQ(result.mStatement.mTradeInstruments.size(), 1U);
    ASSERT_EQ(result.mStatement.mTradeInstruments[0].mTransactions.size(), 1U);

    const auto& trade = result.mStatement.mTradeInstruments[0].mTransactions[0];

    EXPECT_EQ(trade.mExchangeRate, 100'000'000);
    EXPECT_EQ(trade.mCurrency, Currency::EUR);

    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(TradeRepublicParserTest, RejectsATradeWithAZeroFxRate) {
    // fx_rate 0 is not a usable rate.
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "10.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-90.00",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "-100.00",
        .mOriginalCurrency = "USD",
        .mFxRate = "0",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const ParseResult result = parseRows({row});

    expectNoParsedRecords(result);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics[0].mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(result.mDiagnostics[0].mField, "fx_rate");
    EXPECT_EQ(result.mDiagnostics[0].mRowIndex, 2U);
}

TEST(TradeRepublicParserTest, RejectsATradeWithADecimalCommaFxRate) {
    // fx_rate 0,9 is a decimal comma.
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "10.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-90.00",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "-100.00",
        .mOriginalCurrency = "USD",
        .mFxRate = "0,9",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const ParseResult result = parseRows({row});

    expectNoParsedRecords(result);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics[0].mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(result.mDiagnostics[0].mField, "fx_rate");
    EXPECT_EQ(result.mDiagnostics[0].mRowIndex, 2U);
}

TEST(TradeRepublicParserTest, RejectsATradeWithAnUnknownOriginalCurrency) {
    // XYZ is not a known currency.
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "10.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-90.00",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "-100.00",
        .mOriginalCurrency = "XYZ",
        .mFxRate = "0.9",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const ParseResult result = parseRows({row});

    expectNoParsedRecords(result);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics[0].mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(result.mDiagnostics[0].mField, "original_currency");
    EXPECT_EQ(result.mDiagnostics[0].mRowIndex, 2U);
}

TEST(TradeRepublicParserTest, RejectsATradeWithAnInvalidOriginalAmount) {
    // original_amount is not a number.
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "10.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-90.00",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "abc",
        .mOriginalCurrency = "USD",
        .mFxRate = "0.9",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const ParseResult result = parseRows({row});

    expectNoParsedRecords(result);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics[0].mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(result.mDiagnostics[0].mField, "original_amount");
    EXPECT_EQ(result.mDiagnostics[0].mRowIndex, 2U);
}

TEST(TradeRepublicParserTest, RejectsAForeignBuyWithAPositiveOriginalAmount) {
    // A buy needs a negative original_amount.
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "10.0000000000",
        .mPrice = "10.000000",
        .mAmount = "-90.00",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "100.00",
        .mOriginalCurrency = "USD",
        .mFxRate = "0.9",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const ParseResult result = parseRows({row});

    expectNoParsedRecords(result);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics[0].mCode, DiagnosticCode::InconsistentValue);
    EXPECT_EQ(result.mDiagnostics[0].mMessageKey, MessageKey::InconsistentValueTradeSide);
    EXPECT_EQ(result.mDiagnostics[0].mMessageParams,
              (std::vector<MessageParam>{textParam("field", "original_amount")}));
    EXPECT_EQ(result.mDiagnostics[0].mField, "original_amount");
    EXPECT_EQ(result.mDiagnostics[0].mRowIndex, 2U);
}

TEST(TradeRepublicParserTest, RejectsAForeignTradeWithAnEmptyOriginalAmount) {
    // original_currency is set, so original_amount is required.
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "10.0000000000",
        .mPrice = "10.00",
        .mAmount = "-90.00",
        .mFee = "-1.00",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "USD",
        .mFxRate = "0.9",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const ParseResult result = parseRows({row});

    expectNoParsedRecords(result);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics[0].mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(result.mDiagnostics[0].mField, "original_amount");
    EXPECT_EQ(result.mDiagnostics[0].mRowIndex, 2U);
}

TEST(TradeRepublicParserTest, RejectsAForeignTradeWithAnInvalidEurAmount) {
    // amount is not stored for a foreign trade, but it must still be a number.
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "10.0000000000",
        .mPrice = "10.00",
        .mAmount = "abc",
        .mFee = "-1.00",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "-100.00",
        .mOriginalCurrency = "USD",
        .mFxRate = "0.9",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const ParseResult result = parseRows({row});

    expectNoParsedRecords(result);
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(result.mDiagnostics[0].mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(result.mDiagnostics[0].mField, "amount");
    EXPECT_EQ(result.mDiagnostics[0].mRowIndex, 2U);
}

TEST(TradeRepublicParserTest, ReportsAForeignTradeAmountBeforeItsFxRate) {
    // amount is not a number and fx_rate has a decimal comma.
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "10.0000000000",
        .mPrice = "10.00",
        .mAmount = "abc",
        .mFee = "-1.00",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "-100.00",
        .mOriginalCurrency = "USD",
        .mFxRate = "0,9",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const ParseResult result = parseRows({row});

    expectNoParsedRecords(result);
    ASSERT_EQ(result.mDiagnostics.size(), 2U);
    EXPECT_EQ(result.mDiagnostics[0].mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(result.mDiagnostics[0].mField, "amount");
    EXPECT_EQ(result.mDiagnostics[0].mRowIndex, 2U);
    EXPECT_EQ(result.mDiagnostics[1].mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(result.mDiagnostics[1].mField, "fx_rate");
    EXPECT_EQ(result.mDiagnostics[1].mRowIndex, 2U);
}

// The row kind tells the application result which reports a rejected row affects.

TEST(TradeRepublicParserTest, ARejectedTradeRowDiagnosticHasTheTradeRowKind) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "1.0000000000",
        .mPrice = "abc", // not a number
        .mAmount = "-10.000000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const ParseResult result = parseRows({row});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mField, "price");
    EXPECT_EQ(result.mDiagnostics[0].mRowKind, ParsedRowKind::Trade);
}

TEST(TradeRepublicParserTest, ARejectedDividendRowDiagnosticHasTheDividendRowKind) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "DIVIDEND",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "",
        .mPrice = "",
        .mAmount = "abc", // not a number
        .mFee = "",
        .mTax = "0",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const ParseResult result = parseRows({row});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mField, "amount");
    EXPECT_EQ(result.mDiagnostics[0].mRowKind, ParsedRowKind::Dividend);
}

TEST(TradeRepublicParserTest, ARejectedBondInterestRowDiagnosticHasTheBondInterestRowKind) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "BOND",
        .mName = "Synthetic Test Bond",
        .mSymbol = "XX9000000002",
        .mShares = "",
        .mPrice = "",
        .mAmount = "abc", // not a number
        .mFee = "",
        .mTax = "0",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const ParseResult result = parseRows({row});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mField, "amount");
    EXPECT_EQ(result.mDiagnostics[0].mRowKind, ParsedRowKind::BondInterest);
}

TEST(TradeRepublicParserTest, ARejectedBrokerInterestRowDiagnosticHasTheBrokerInterestRowKind) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "", // no asset class, name or symbol: interest of the broker account
        .mName = "",
        .mSymbol = "",
        .mShares = "",
        .mPrice = "",
        .mAmount = "abc", // not a number
        .mFee = "",
        .mTax = "0",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const ParseResult result = parseRows({row});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mField, "amount");
    EXPECT_EQ(result.mDiagnostics[0].mRowKind, ParsedRowKind::BrokerInterest);
}

TEST(TradeRepublicParserTest, ARejectedCorporateActionRowDiagnosticHasTheCorporateActionRowKind) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "abc", // not a number
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const ParseResult result = parseRows({row});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mRowKind, ParsedRowKind::CorporateAction);
}

TEST(TradeRepublicParserTest, ARejectedBenefitRowDiagnosticHasTheBenefitRowKind) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "BENEFITS_SAVEBACK",
        .mAssetClass = "",
        .mName = "",
        .mSymbol = "",
        .mShares = "",
        .mPrice = "",
        .mAmount = "abc", // not a number
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const ParseResult result = parseRows({row});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mRowKind, ParsedRowKind::Benefit);
}

TEST(TradeRepublicParserTest, ARejectedPrivateMarketRowDiagnosticHasThePrivateMarketRowKind) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "PRIVATE_MARKET_BUY",
        .mAssetClass = "PRIVATE_FUND",
        .mName = "Synthetic Test Private Fund",
        .mSymbol = "XX9000000003",
        .mShares = "",
        .mPrice = "",
        .mAmount = "abc", // not a number
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const ParseResult result = parseRows({row});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mRowKind, ParsedRowKind::PrivateMarket);
}

TEST(TradeRepublicParserTest, ARejectedInterestRowOfUnknownSubtypeHasTheUnknownInterestRowKind) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Test Share",
        .mSymbol = "XX9000000001",
        .mShares = "",
        .mPrice = "",
        .mAmount = "abc", // not a number; a share pays no interest
        .mFee = "",
        .mTax = "0",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const ParseResult result = parseRows({row});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mRowKind, ParsedRowKind::UnknownInterest);
}

TEST(TradeRepublicParserTest, ARowOfUnknownTypeHasNoRowKind) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "SYNTHETIC_UNKNOWN",
        .mAssetClass = "",
        .mName = "",
        .mSymbol = "",
        .mShares = "",
        .mPrice = "",
        .mAmount = "5",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const ParseResult result = parseRows({row});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mCode, DiagnosticCode::UnknownRowType);
    EXPECT_FALSE(result.mDiagnostics[0].mRowKind.has_value());
}

TEST(TradeRepublicParserTest, ARecognizedUnsupportedRowHasNoRowKind) {
    const SyntheticCsvRow row{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "FIXED_INCOME", // recognized, but this app produces no XML for it
        .mAssetClass = "",
        .mName = "",
        .mSymbol = "",
        .mShares = "",
        .mPrice = "",
        .mAmount = "5",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "Synthetic test row",
        .mTransactionId = "synthetic-test-transaction",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const ParseResult result = parseRows({row});

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mCode, DiagnosticCode::UnsupportedRowType);
    EXPECT_FALSE(result.mDiagnostics[0].mRowKind.has_value());
}

TEST(TradeRepublicParserTest, ARecordWithTheWrongFieldCountHasNoRowKind) {
    // 22 fields: the last field (mcc_code) is missing, so the row type is never read.
    const std::string shortBuyLine =
        R"("2024-01-15T10:00:00.000Z","2024-01-15","DEFAULT","TRADING","BUY","STOCK",)"
        R"("Synthetic Test Share","XX9000000001","1.0000000000","10.000000","-10.000000",)"
        R"("","","EUR","","","","Synthetic test row","synthetic-test-transaction",)"
        R"("","","")";

    const ParseResult result = parseRawText(std::string{kCsvHeader} + '\n' + shortBuyLine + '\n');

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    EXPECT_EQ(result.mDiagnostics[0].mCode, DiagnosticCode::FieldCountMismatch);
    EXPECT_FALSE(result.mDiagnostics[0].mRowKind.has_value());
}

TEST(TradeRepublicParserTest, AFileLevelDiagnosticHasNoRowKind) {
    // 0 bytes: the reader reports the file as unreadable.
    const ParseResult result = parseRawText("");

    ASSERT_FALSE(result.mDiagnostics.empty());

    EXPECT_FALSE(result.mDiagnostics[0].mRowKind.has_value());
}

} // namespace
