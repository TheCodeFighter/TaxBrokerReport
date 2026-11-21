#include "../support/log_capture.hpp"
#include "taxbroker/api/application_result_json.hpp"
#include "taxbroker/application_request.hpp"
#include "taxbroker/application_run.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace taxbroker;

// All rows are synthetic. The header is the one of the Trade Republic export.
constexpr std::string_view kTradeRepublicHeader =
    "datetime,date,account_type,category,type,asset_class,name,symbol,shares,price,amount,fee,tax,"
    "currency,original_amount,original_currency,fx_rate,description,transaction_id,"
    "counterparty_name,counterparty_iban,payment_reference,mcc_code\n";

// The header of the ECB Data Portal `csvdata` export of the EXR dataset (32 columns).
constexpr std::string_view kEcbHeader =
    "KEY,FREQ,CURRENCY,CURRENCY_DENOM,EXR_TYPE,EXR_SUFFIX,TIME_PERIOD,OBS_VALUE,OBS_STATUS,OBS_"
    "CONF,OBS_PRE_BREAK,OBS_COM,TIME_FORMAT,BREAKS,COLLECTION,COMPILING_ORG,DISS_ORG,DOM_SER_IDS,"
    "PUBL_ECB,PUBL_MU,PUBL_PUBLIC,UNIT_INDEX_BASE,COMPILATION,COVERAGE,DECIMALS,NAT_TITLE,SOURCE_"
    "AGENCY,SOURCE_PUB,TITLE,TITLE_COMPL,UNIT,UNIT_MULT";

TaxpayerMetadata syntheticTaxpayer() {
    return {.mTaxNumber = "12345679",
            .mName = "Synthetic Taxpayer",
            .mAddress = "Synthetic Street 1",
            .mCity = "Synthetic City",
            .mPostCode = "1000",
            .mPostName = "Synthetic City",
            .mBirthDate = *parseCalendarDate("1990-01-31"),
            .mIsResident = true,
            .mEmail = "taxpayer@example.com",
            .mPhoneNumber = "+38600000000"};
}

/// A request for 2024 with one Trade Republic file made of the given rows.
ApplicationRequest requestWithRows(const std::vector<std::string_view>& aRows) {
    std::string content{kTradeRepublicHeader};

    for (const auto row : aRows)
    {
        content += row;
        content += '\n';
    }

    ApplicationRequest request{
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
        .mTaxpayer = syntheticTaxpayer(),
    };

    request.mBrokerFiles.push_back(
        {.mBroker = Broker::TradeRepublic,
         .mFile = {.mName = "synthetic.csv", .mContent = std::move(content)}});

    return request;
}

/// The run, or an empty optional (and a failed test) when the files were not accepted.
std::optional<ApplicationRun> startRun(ApplicationRequest aRequest) {
    auto started = ApplicationRun::start(std::move(aRequest));
    auto* run = std::get_if<ApplicationRun>(&started);

    EXPECT_NE(run, nullptr);

    if (run == nullptr)
    {
        return std::nullopt;
    }

    return std::optional<ApplicationRun>{std::move(*run)};
}

// Two purchases and one sale of XX0000000001, and one EUR dividend of a known payer.
constexpr std::string_view kBuy1 =
    R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)";
constexpr std::string_view kBuy2 =
    R"(2024-02-10T09:00:00Z,2024-02-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,20,20,-400,,,EUR,,,,,synthetic-buy-2,,,,)";
constexpr std::string_view kSell =
    R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-15,30,450,,,EUR,,,,,synthetic-sell,,,,)";
constexpr std::string_view kDividend =
    R"(2024-03-14T09:00:00Z,2024-03-14,DEFAULT,CASH,DIVIDEND,STOCK,Apple Inc,US0378331005,,,10.00,,,EUR,12.50,USD,0.800000,,synthetic-dividend-1,,,,)";
// A sale of 5 units of XX0000000002 that was never bought: the history is incomplete.
constexpr std::string_view kSellWithoutPurchase =
    R"(2024-04-02T09:00:00Z,2024-04-02,DEFAULT,TRADING,SELL,STOCK,Other Share,XX0000000002,-5,10,50,,,EUR,,,,,synthetic-sale-2,,,,)";

TEST(ApplicationRunIntegrationTest, OneRequestGivesAllFourForms) {
    auto run = startRun(requestWithRows({kBuy1, kBuy2, kSell, kDividend}));

    ASSERT_TRUE(run.has_value());

    const auto& result = run->result();

    // Doh-KDVP: 10 units at EUR 10 and 5 units at EUR 20 are EUR 200 of basis against EUR 450 of
    // proceeds, so the sale has no loss question.
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Generated);
    EXPECT_TRUE(result.report(ReportType::CapitalGains).mXml.has_value());

    EXPECT_EQ(result.report(ReportType::Dividends).mStatus, ReportStatus::Generated);
    EXPECT_TRUE(result.report(ReportType::Dividends).mXml.has_value());

    // No interest rows.
    EXPECT_EQ(result.report(ReportType::BankDepositInterest).mStatus, ReportStatus::NoData);
    EXPECT_EQ(result.report(ReportType::OtherInterest).mStatus, ReportStatus::NoData);

    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.mStatus, OverallStatus::Success);
}

// A purchase with a fee of EUR 1.00 and a sale with a fee of EUR 5.00, and a saveback.
constexpr std::string_view kBuyWithFee =
    R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,-1.00,,EUR,,,,,synthetic-buy-fee,,,,)";
constexpr std::string_view kSellWithFee =
    R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,30,300,-5.00,,EUR,,,,,synthetic-sell-fee,,,,)";
constexpr std::string_view kBuyWithoutFee =
    R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-fee,,,,)";
constexpr std::string_view kSellWithoutFee =
    R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,30,300,,,EUR,,,,,synthetic-sell-fee,,,,)";
constexpr std::string_view kSaveback =
    R"(2024-05-02T09:00:00Z,2024-05-02,DEFAULT,CASH,BENEFITS_SAVEBACK,FUND,Synthetic Reward Fund,XX1000000002,,,2.50,,,EUR,,,,,synthetic-saveback,,,,)";
// Fees in the year before and the year after the reporting year.
constexpr std::string_view kBuyWithFeeIn2023 =
    R"(2023-12-31T09:00:00Z,2023-12-31,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,-1.00,,EUR,,,,,synthetic-buy-2023,,,,)";
constexpr std::string_view kBuyWithFeeIn2025 =
    R"(2025-01-02T09:00:00Z,2025-01-02,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,-1.00,,EUR,,,,,synthetic-buy-2025,,,,)";

TEST(ApplicationRunIntegrationTest, FeesAndBenefitsAreSummarisedForTheYear) {
    auto run = startRun(requestWithRows({kBuyWithFee, kSellWithFee, kSaveback}));

    ASSERT_TRUE(run.has_value());

    const auto& result = run->result();

    ASSERT_EQ(result.mFees.mBrokers.size(), 1U);
    EXPECT_EQ(result.mFees.mBrokers.at(0).mBroker, Broker::TradeRepublic);
    EXPECT_EQ(result.mFees.mBrokers.at(0).mByCurrency.at(0).mCount, 2U);
    // EUR 1.00 + EUR 5.00 = EUR 6.00 at four decimals.
    EXPECT_EQ(result.mFees.mBrokers.at(0).mByCurrency.at(0).mTotal, 60'000);
    EXPECT_EQ(result.mFees.mEurTotal, 60'000);

    ASSERT_EQ(result.mBenefits.mBrokers.size(), 1U);
    EXPECT_EQ(result.mBenefits.mBrokers.at(0).mType, BenefitType::Saveback);
    EXPECT_EQ(result.mBenefits.mEurTotal, 25'000); // EUR 2.50

    // A fee never asks for anything.
    EXPECT_TRUE(result.mRequests.empty());
}

TEST(ApplicationRunIntegrationTest, FeesOfOtherYearsAreNotInTheSummary) {
    auto run = startRun(requestWithRows({kBuyWithFeeIn2023, kBuyWithFeeIn2025}));

    ASSERT_TRUE(run.has_value());

    EXPECT_TRUE(run->result().mFees.mBrokers.empty());
    EXPECT_FALSE(run->result().mFees.mEurTotal.has_value());
}

TEST(ApplicationRunIntegrationTest, TheSameExportTwiceCountsAFeeOnce) {
    auto request = requestWithRows({kBuyWithFee, kSellWithFee});
    const auto content = request.mBrokerFiles.at(0).mFile.mContent;

    request.mBrokerFiles.push_back(
        {.mBroker = Broker::TradeRepublic, .mFile = {.mName = "copy.csv", .mContent = content}});

    auto run = startRun(std::move(request));

    ASSERT_TRUE(run.has_value());

    const auto& fees = run->result().mFees;

    // The two files hold the same two transactions, which merge into two events.
    ASSERT_EQ(fees.mBrokers.size(), 1U);
    EXPECT_EQ(fees.mBrokers.at(0).mByCurrency.at(0).mCount, 2U);
    EXPECT_EQ(fees.mEurTotal, 60'000);
}

TEST(ApplicationRunIntegrationTest, FeesDoNotChangeTheCapitalGainsXml) {
    // The same purchase and sale, once with fees and once without. The form uses the unit prices
    // and the units, so the XML is the same.
    auto withFees = startRun(requestWithRows({kBuyWithFee, kSellWithFee}));
    auto withoutFees = startRun(requestWithRows({kBuyWithoutFee, kSellWithoutFee}));

    ASSERT_TRUE(withFees.has_value());
    ASSERT_TRUE(withoutFees.has_value());

    const auto& left = withFees->result().report(ReportType::CapitalGains);
    const auto& right = withoutFees->result().report(ReportType::CapitalGains);

    ASSERT_TRUE(left.mXml.has_value());
    ASSERT_TRUE(right.mXml.has_value());
    EXPECT_EQ(*left.mXml, *right.mXml);
    EXPECT_TRUE(withoutFees->result().mFees.mBrokers.empty());
    EXPECT_EQ(withFees->result().mFees.mEurTotal, 60'000);
}

TEST(ApplicationRunIntegrationTest, ACapitalErrorBlocksOnlyDohKdvp) {
    auto run = startRun(requestWithRows({kBuy1, kBuy2, kSell, kDividend, kSellWithoutPurchase}));

    ASSERT_TRUE(run.has_value());

    const auto& result = run->result();

    // The sale of XX0000000002 has no purchase: one error for capital gains only.
    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(std::get<ProcessingDiagnosticCode>(diagnostic.mCode),
              ProcessingDiagnosticCode::IncompleteHistory);
    EXPECT_EQ(diagnostic.mIsin, "XX0000000002");
    EXPECT_EQ(diagnostic.mAffectedReports, (std::vector{ReportType::CapitalGains}));

    // The user decides what happens to the failed investment.
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::NeedsInput);
    EXPECT_FALSE(result.report(ReportType::CapitalGains).mXml.has_value());

    ASSERT_EQ(result.mRequests.size(), 1U);

    const auto& request = result.mRequests[0];

    EXPECT_EQ(request.mType, RequestType::CapitalGainsFailureResolutionRequired);
    ASSERT_EQ(request.mInvestments.size(), 1U);
    EXPECT_EQ(request.mInvestments[0].mIsin, "XX0000000002");

    // The dividend form is independent.
    EXPECT_EQ(result.report(ReportType::Dividends).mStatus, ReportStatus::Generated);
    EXPECT_TRUE(result.report(ReportType::Dividends).mXml.has_value());

    // An open request makes the whole run needs_input, while the dividend form stays available.
    EXPECT_EQ(result.mStatus, OverallStatus::NeedsInput);
}

TEST(ApplicationRunIntegrationTest,
     AWrongTaxNumberCheckDigitGivesOneWarningAndTheXmlStaysComplete) {
    // 1*8 + 2*7 + 3*6 + 4*5 + 5*4 + 6*3 + 7*2 = 112; 112 mod 11 = 2; the check digit is 9, not 8.
    auto request = requestWithRows({kBuy1, kBuy2, kSell});

    request.mTaxpayer.mTaxNumber = "12345678";

    auto run = startRun(std::move(request));

    ASSERT_TRUE(run.has_value());

    const auto& result = run->result();

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = result.mDiagnostics[0];

    EXPECT_EQ(std::get<ProcessingDiagnosticCode>(diagnostic.mCode),
              ProcessingDiagnosticCode::TaxpayerTaxNumberCheckDigit);
    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_FALSE(diagnostic.mBlocking);
    EXPECT_TRUE(diagnostic.mAffectedReports.empty());
    EXPECT_EQ(diagnostic.mMessageKey, MessageKey::TaxpayerTaxNumberCheckDigit);
    EXPECT_EQ(diagnostic.mMessage.find("12345678"), std::string::npos);

    // The form is still written, with the number as typed; the warning only changes the status.
    EXPECT_EQ(result.mStatus, OverallStatus::CompletedWithWarnings);
    ASSERT_TRUE(result.report(ReportType::CapitalGains).mXml.has_value());
    EXPECT_NE(result.report(ReportType::CapitalGains).mXml->find("<edp:taxNumber>12345678<"),
              std::string::npos);
}

TEST(ApplicationRunIntegrationTest, AnAnswerThroughTheSessionRecalculates) {
    auto run = startRun(requestWithRows({kBuy1, kBuy2, kSell, kDividend, kSellWithoutPurchase}));

    ASSERT_TRUE(run.has_value());
    ASSERT_EQ(run->result().mRequests.size(), 1U);

    const auto startRevision = run->result().mRunRevision;
    const auto requestId = run->result().mRequests[0].mId;
    const auto rejections = run->session().answer(
        {{requestId,
          startRevision,
          FailureResolutionAnswer{FailureResolution::ExcludeFailedInstruments}}});

    EXPECT_TRUE(rejections.empty());

    const auto& result = run->result();

    EXPECT_EQ(result.mRunRevision, startRevision + 1);

    // Doh-KDVP is written for XX0000000001 only.
    const auto& capital = result.report(ReportType::CapitalGains);

    EXPECT_EQ(capital.mStatus, ReportStatus::Generated);
    EXPECT_TRUE(capital.mXml.has_value());
    ASSERT_EQ(capital.mExcludedInvestments.size(), 1U);
    EXPECT_EQ(capital.mExcludedInvestments[0].mIsin, "XX0000000002");

    EXPECT_EQ(result.report(ReportType::Dividends).mStatus, ReportStatus::Generated);
    EXPECT_TRUE(result.mRequests.empty());

    // The user's exclusion keeps the error it resolved, which makes the run a completed one with
    // warnings.
    EXPECT_EQ(result.mStatus, OverallStatus::CompletedWithWarnings);
}

TEST(ApplicationRunIntegrationTest, TheSameRequestGivesTheSameJson) {
    const auto rows =
        std::vector<std::string_view>{kBuy1, kBuy2, kSell, kDividend, kSellWithoutPurchase};
    auto first = startRun(requestWithRows(rows));
    auto second = startRun(requestWithRows(rows));

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());

    EXPECT_EQ(api::serializeApplicationResultJson(first->result()),
              api::serializeApplicationResultJson(second->result()));
}

TEST(ApplicationRunIntegrationTest, AnUnsupportedYearFailsAllFourForms) {
    // 1999 is below the first supported year, 2000.
    auto request = requestWithRows({kBuy1});

    request.mReportingYear = 1999;

    auto run = startRun(std::move(request));

    ASSERT_TRUE(run.has_value());

    const auto& result = run->result();

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(std::get<ProcessingDiagnosticCode>(result.mDiagnostics[0].mCode),
              ProcessingDiagnosticCode::InvalidReportingYear);
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(ReportType::Dividends).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(ReportType::BankDepositInterest).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.report(ReportType::OtherInterest).mStatus, ReportStatus::Failed);
    EXPECT_EQ(result.mStatus, OverallStatus::Failed);
}

TEST(ApplicationRunIntegrationTest, ABrokerWithoutAParserIsAProgrammingError) {
    // A request for an unknown broker has no parser, so it cannot be read.
    auto request = requestWithRows({kBuy1});

    request.mBrokerFiles[0].mBroker = Broker::Unknown;

    EXPECT_THROW(std::ignore = ApplicationRun::start(std::move(request)), std::logic_error);
}

/// A request for 2024 with one Interactive Brokers file with the given XML text.
ApplicationRequest requestWithInteractiveBrokersXml(std::string aXml) {
    ApplicationRequest request{
        .mReportingYear = 2024,
        .mFilingType = FilingType::Original,
        .mTaxpayer = syntheticTaxpayer(),
    };

    request.mBrokerFiles.push_back(
        {.mBroker = Broker::InteractiveBrokers,
         .mFile = {.mName = "synthetic.xml", .mContent = std::move(aXml)}});

    return request;
}

TEST(ApplicationRunIntegrationTest, AnInteractiveBrokersFileIsReadByItsOwnParser) {
    // A buy of 10 shares at EUR 10 and a sale of 4 at EUR 12.50 in 2024: a gain of EUR 10.00.
    auto run = startRun(requestWithInteractiveBrokersXml(
        R"xml(<FlexQueryResponse queryName="synthetic" type="AF">
  <FlexStatements count="1">
    <FlexStatement accountId="U0000000">
      <AccountInformation accountId="U0000000" ibEntity="IB-IE"/>
      <Trades>
        <Trade transactionID="synthetic-1" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="Synthetic Share" isin="XX0000000001" currency="EUR" tradeDate="20240110" dateTime="20240110;040000" buySell="BUY" quantity="10" tradePrice="10" proceeds="-100" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" notes="" multiplier="1"/>
        <Trade transactionID="synthetic-2" levelOfDetail="EXECUTION" transactionType="ExchTrade" assetCategory="STK" subCategory="COMMON" description="Synthetic Share" isin="XX0000000001" currency="EUR" tradeDate="20240310" dateTime="20240310;040000" buySell="SELL" quantity="-4" tradePrice="12.5" proceeds="50" ibCommission="0" ibCommissionCurrency="EUR" taxes="0" notes="" multiplier="1"/>
      </Trades>
    </FlexStatement>
  </FlexStatements>
</FlexQueryResponse>
)xml"));

    ASSERT_TRUE(run.has_value());

    const auto& result = run->result();

    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Generated);
    EXPECT_TRUE(result.mDiagnostics.empty());
}

TEST(ApplicationRunIntegrationTest, AnInteractiveBrokersFileThatIsNotXmlFailsTheCapitalReport) {
    // A Flex Query exported as CSV is not read; the file gets one error that names no form.
    auto run = startRun(requestWithInteractiveBrokersXml(
        "\"ClientAccountID\",\"AssetClass\"\n\"U0000000\",\"STK\"\n"));

    ASSERT_TRUE(run.has_value());

    const auto& result = run->result();

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(std::get<DiagnosticCode>(result.mDiagnostics[0].mCode),
              DiagnosticCode::InvalidHeader);
    EXPECT_EQ(result.mDiagnostics[0].mMessageKey, MessageKey::InvalidHeaderFlexQueryXmlRequired);
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::Failed);
}

TEST(ApplicationRunIntegrationTest, ABadSuppliedRateFileIsAFault) {
    auto request = requestWithRows({kBuy1});

    // The rate in row 2 is abc.
    request.mEcbRateFiles.push_back(
        {.mName = "bad_rates.csv",
         .mContent =
             std::string{kEcbHeader} + "\n" +
             R"csv(EXR.D.USD.EUR.SP00.A,D,USD,EUR,SP00,A,2024-01-02,abc,A,F,,,P1D,,A,,,,,,,99Q1=100,,,4,,4F0,,Synthetic currency/Euro ECB reference exchange rate,"ECB reference exchange rate, Synthetic currency/Euro, 2.15 pm (C.E.T.)",USD,0)csv" +
             "\n"});

    const auto started = ApplicationRun::start(std::move(request));

    ASSERT_TRUE(std::holds_alternative<ApplicationInputFault>(started));

    const auto& fault = std::get<ApplicationInputFault>(started);

    ASSERT_TRUE(std::holds_alternative<RateFileFault>(fault));

    const auto& diagnostic = std::get<RateFileFault>(fault).mDiagnostic;

    EXPECT_EQ(diagnostic.mSourceFile, "bad_rates.csv");
    EXPECT_EQ(diagnostic.mRowIndex, std::optional<std::size_t>{2});
    EXPECT_EQ(diagnostic.mField, std::optional<std::string>{"OBS_VALUE"});
}

TEST(ApplicationRunIntegrationTest, AReferenceFileWithAnUnknownNameIsAFault) {
    auto request = requestWithRows({kBuy1});

    // Only payers.csv, relief_statements.csv and broker_payers.csv are known names.
    request.mReferenceFiles.push_back({.mName = "unknown.csv", .mContent = "isin\n"});

    const auto started = ApplicationRun::start(std::move(request));

    ASSERT_TRUE(std::holds_alternative<ApplicationInputFault>(started));

    const auto& fault = std::get<ApplicationInputFault>(started);

    ASSERT_TRUE(std::holds_alternative<ReferenceFileFault>(fault));
    EXPECT_EQ(std::get<ReferenceFileFault>(fault).mMessage.mKey,
              MessageKey::ReferenceFileUnknownName);
}

TEST(ApplicationRunIntegrationTest, StartWritesOneRunStartedLineAndTheLoadStages) {
    LogCapture log;

    auto run = startRun(requestWithRows({kBuy1, kBuy2, kSell}));

    ASSERT_TRUE(run.has_value());

    EXPECT_EQ(log.count("Run started: year 2024, filing original, developer mode off, brokers "
                        "[Trade Republic x1], ecb rate files 0, reference files 0"),
              1U);
    EXPECT_EQ(log.count("Run restarted"), 0U);
    EXPECT_EQ(log.count("Stage parse and merge: sources 1, events 3,"), 1U);
    EXPECT_EQ(log.count("Calculation done ("), 1U);
}

TEST(ApplicationRunIntegrationTest, RestartWritesOneRunRestartedLine) {
    auto run = startRun(requestWithRows({kBuy1, kBuy2, kSell}));

    ASSERT_TRUE(run.has_value());

    LogCapture log;
    const auto fault = run->restart(requestWithRows({kBuy1, kBuy2, kSell}));

    EXPECT_FALSE(fault.has_value());
    EXPECT_EQ(log.count("Run restarted: year 2024, filing original,"), 1U);
    EXPECT_EQ(log.count("Run started"), 0U);
    EXPECT_EQ(log.count("Calculation done ("), 1U);
}

TEST(ApplicationRunIntegrationTest, AFaultWritesNoRunStartedLine) {
    LogCapture log;
    auto request = requestWithRows({kBuy1});

    request.mReferenceFiles.push_back({.mName = "unknown.csv", .mContent = "isin\n"});

    const auto started = ApplicationRun::start(std::move(request));

    EXPECT_TRUE(std::holds_alternative<ApplicationInputFault>(started));
    EXPECT_EQ(log.count("Run started"), 0U);
}

} // namespace
