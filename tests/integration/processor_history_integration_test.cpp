#include "taxbroker/application_request.hpp"
#include "taxbroker/application_run.hpp"
#include "taxbroker/exact_arithmetic.hpp"
#include "taxbroker/input_file.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace taxbroker;

// All rows are synthetic. Every scenario runs the whole application (`ApplicationRun`) on
// Trade Republic files in EUR, so no rate file is needed. Money is stored with 4 decimals
// (30'000 is EUR 3.00) and units with 8 decimals (100'000'000 is 1 unit). The synthetic ISINs
// use the prefix XX, which is never in the built-in payer file, so no payer is found.
constexpr std::string_view kTradeRepublicHeader =
    "datetime,date,account_type,category,type,asset_class,name,symbol,shares,price,amount,fee,tax,"
    "currency,original_amount,original_currency,fx_rate,description,transaction_id,"
    "counterparty_name,counterparty_iban,payment_reference,mcc_code\n";

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

Date day(std::string_view aDate) {
    return *parseCalendarDate(aDate);
}

/// A Trade Republic file made of the header and the given rows.
BrokerInputFile fileOfRows(std::string_view aName, const std::vector<std::string_view>& aRows) {
    std::string content{kTradeRepublicHeader};

    for (const std::string_view row : aRows)
    {
        content += row;
        content += '\n';
    }

    return {.mBroker = Broker::TradeRepublic,
            .mFile = {.mName = std::string{aName}, .mContent = std::move(content)}};
}

/// A request for the year with the synthetic taxpayer and the given files.
ApplicationRequest requestOf(int aYear, std::vector<BrokerInputFile> aFiles) {
    return {.mReportingYear = aYear,
            .mFilingType = FilingType::Original,
            .mTaxpayer = syntheticTaxpayer(),
            .mBrokerFiles = std::move(aFiles)};
}

/// The run, or an empty optional (and a failed test) when the files were not accepted.
std::optional<ApplicationRun> startRun(ApplicationRequest aRequest) {
    std::variant<ApplicationInputFault, ApplicationRun> started =
        ApplicationRun::start(std::move(aRequest));
    ApplicationRun* run = std::get_if<ApplicationRun>(&started);

    EXPECT_NE(run, nullptr);

    if (run == nullptr)
    {
        return std::nullopt;
    }

    return std::optional<ApplicationRun>{std::move(*run)};
}

/// Answers the request of the given ID at the current revision of the run.
std::vector<RejectedAnswer>
answerRequest(ApplicationRun& aRun, const std::string& aRequestId, AnswerValue aValue) {
    return aRun.session().answer({{aRequestId, aRun.result().mRunRevision, std::move(aValue)}});
}

/// An exact EUR amount: aNumerator / aDenominator.
ExactRational eur(std::int64_t aNumerator, std::int64_t aDenominator) {
    return std::get<ExactRational>(
        ExactRational::create(WideInteger{aNumerator}, WideInteger{aDenominator}));
}

/// The diagnostics of the result with this message key, in result order.
std::vector<const ApplicationDiagnostic*> diagnosticsWithKey(const ApplicationResult& aResult,
                                                             MessageKey aKey) {
    std::vector<const ApplicationDiagnostic*> found;

    for (const ApplicationDiagnostic& diagnostic : aResult.mDiagnostics)
    {
        if (diagnostic.mMessageKey == aKey)
        {
            found.push_back(&diagnostic);
        }
    }

    return found;
}

// Trade Republic interest from 6 December 2023 is deposit interest (Doh-DHO); earlier interest is
// Doh-Obr. The receipt date of each payment alone decides the form.
TEST(ProcessorHistoryIntegrationTest, InterestOfThe5thOfDecember2023IsDohObrAndOfThe6thIsDohDho) {
    std::optional<ApplicationRun> run = startRun(requestOf(
        2023,
        {fileOfRows(
            "history.csv",
            {
                R"(2023-12-05T10:00:00Z,2023-12-05,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,3.00,,,EUR,,,,Synthetic broker interest,synthetic-interest-1,,,,)",
                R"(2023-12-06T10:00:00Z,2023-12-06,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,4.00,,,EUR,,,,Synthetic bank interest,synthetic-interest-2,,,,)",
            })}));

    ASSERT_TRUE(run.has_value());

    // Doh-Obr: the payment of the 5th, EUR 3.00. Doh-DHO: the total of the 6th is EUR 4.00, above
    // 0 and at most EUR 1,000, so one allowance question is open; without an answer the form is
    // made as for yes.
    const ApplicationResult& asked = run->result();

    EXPECT_EQ(asked.report(ReportType::OtherInterest).mStatus, ReportStatus::Generated);
    ASSERT_TRUE(asked.report(ReportType::OtherInterest).mIncome.has_value());
    ASSERT_EQ(asked.report(ReportType::OtherInterest).mIncome->mRows.size(), 1U);
    EXPECT_EQ(asked.report(ReportType::OtherInterest).mIncome->mRows[0].mKind,
              IncomeRowKind::BrokerCashInterest);
    EXPECT_EQ(asked.report(ReportType::OtherInterest).mIncome->mRows[0].mGross, 30'000); // EUR 3.00

    ASSERT_EQ(asked.mRequests.size(), 1U);
    EXPECT_EQ(asked.mRequests[0].mType, RequestType::DepositInterestTotalRequired);
    EXPECT_EQ(asked.report(ReportType::BankDepositInterest).mStatus, ReportStatus::Generated);
    ASSERT_TRUE(asked.report(ReportType::BankDepositInterest).mIncome.has_value());
    ASSERT_EQ(asked.report(ReportType::BankDepositInterest).mIncome->mRows.size(), 1U);
    EXPECT_EQ(asked.report(ReportType::BankDepositInterest).mIncome->mRows[0].mGross,
              40'000); // EUR 4.00

    const std::string requestId = asked.mRequests[0].mId;

    // Yes (other banks bring the year above EUR 1,000): Doh-DHO holds the EUR 4.00; Doh-Obr stays.
    EXPECT_TRUE(
        answerRequest(*run, requestId, DepositInterestTotalAnswer{.mAboveLimit = true}).empty());

    const ApplicationResult& answered = run->result();

    EXPECT_TRUE(answered.mRequests.empty());
    EXPECT_EQ(answered.report(ReportType::BankDepositInterest).mStatus, ReportStatus::Generated);
    ASSERT_EQ(answered.report(ReportType::BankDepositInterest).mIncome->mRows.size(), 1U);
    EXPECT_EQ(answered.report(ReportType::BankDepositInterest).mIncome->mRows[0].mGross,
              40'000); // EUR 4.00
    ASSERT_EQ(answered.report(ReportType::OtherInterest).mIncome->mRows.size(), 1U);
    EXPECT_EQ(answered.report(ReportType::OtherInterest).mIncome->mRows[0].mGross, 30'000);
}

// The allowance is EUR 1,000 over all files of the year; a payment of January of the next year is
// another year.
TEST(ProcessorHistoryIntegrationTest, ExactlyOneThousandFromTwoFilesAsksAndAJanuaryPaymentIsNotIn) {
    std::optional<ApplicationRun> run = startRun(requestOf(
        2024,
        {fileOfRows(
             "a.csv",
             {
                 R"(2024-06-03T10:00:00Z,2024-06-03,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,500.00,,,EUR,,,,Synthetic bank interest,synthetic-interest-1,,,,)",
             }),
         fileOfRows(
             "b.csv",
             {
                 R"(2024-12-02T10:00:00Z,2024-12-02,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,500.00,,,EUR,,,,Synthetic bank interest,synthetic-interest-2,,,,)",
                 R"(2025-01-02T10:00:00Z,2025-01-02,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,0.01,,,EUR,,,,Synthetic bank interest,synthetic-interest-3,,,,)",
             })}));

    ASSERT_TRUE(run.has_value());

    const ApplicationResult& result = run->result();

    // 500.00 + 500.00 = EUR 1,000.00, which is not above the allowance: one question. The EUR 0.01
    // of 2025 is not in the total, so the form made without an answer holds EUR 1,000.00.
    ASSERT_TRUE(result.mDepositInterest.has_value());
    EXPECT_EQ(result.mDepositInterest->mImportedQualifying, 10'000'000); // EUR 1,000.0000
    EXPECT_EQ(result.mDepositInterest->mAllowance, 10'000'000);

    ASSERT_EQ(result.mRequests.size(), 1U);
    EXPECT_EQ(result.mRequests[0].mType, RequestType::DepositInterestTotalRequired);
    ASSERT_TRUE(result.report(ReportType::BankDepositInterest).mIncome.has_value());
    EXPECT_EQ(result.report(ReportType::BankDepositInterest).mIncome->mGross, 10'000'000);
}

TEST(ProcessorHistoryIntegrationTest, OneCentAboveOneThousandFromTwoFilesMakesDohDhoWithoutAsking) {
    std::optional<ApplicationRun> run = startRun(requestOf(
        2024,
        {fileOfRows(
             "a.csv",
             {
                 R"(2024-06-03T10:00:00Z,2024-06-03,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,500.00,,,EUR,,,,Synthetic bank interest,synthetic-interest-1,,,,)",
             }),
         fileOfRows(
             "b.csv",
             {
                 R"(2024-12-02T10:00:00Z,2024-12-02,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,500.01,,,EUR,,,,Synthetic bank interest,synthetic-interest-2,,,,)",
             })}));

    ASSERT_TRUE(run.has_value());

    const ApplicationResult& result = run->result();

    // 500.00 + 500.01 = EUR 1,000.01, above the allowance: no question, and all of it is in the
    // form (FURS applies the allowance).
    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.report(ReportType::BankDepositInterest).mStatus, ReportStatus::Generated);
    ASSERT_TRUE(result.report(ReportType::BankDepositInterest).mIncome.has_value());
    EXPECT_EQ(result.report(ReportType::BankDepositInterest).mIncome->mGross,
              10'000'100); // EUR 1,000.0100
}

// One history file and yearly files hold the same rows, so they give the same forms.
TEST(ProcessorHistoryIntegrationTest, OneHistoryFileAndThreeYearlyFilesGiveTheSameIncomeFor2024) {
    constexpr std::string_view dividend2023 =
        R"(2023-05-15T10:00:00Z,2023-05-15,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share,XX0000000001,,,10,,-1.50,EUR,,,,,synthetic-dividend-1,,,,)";
    constexpr std::string_view interest2023 =
        R"(2023-11-02T10:00:00Z,2023-11-02,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,2.00,,,EUR,,,,Synthetic broker interest,synthetic-interest-1,,,,)";
    constexpr std::string_view dividend2024 =
        R"(2024-05-15T10:00:00Z,2024-05-15,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share,XX0000000001,,,12,,-1.80,EUR,,,,,synthetic-dividend-2,,,,)";
    constexpr std::string_view interest2024March =
        R"(2024-03-01T10:00:00Z,2024-03-01,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,700.00,,,EUR,,,,Synthetic bank interest,synthetic-interest-2,,,,)";
    constexpr std::string_view interest2024September =
        R"(2024-09-02T10:00:00Z,2024-09-02,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,800.00,,,EUR,,,,Synthetic bank interest,synthetic-interest-3,,,,)";
    constexpr std::string_view coupon2024 =
        R"(2024-03-10T10:00:00Z,2024-03-10,DEFAULT,CASH,INTEREST_PAYMENT,BOND,Synthetic Bond,XS0000000002,,,25.00,,,EUR,,,,Synthetic bond interest,synthetic-coupon-1,,,,)";
    constexpr std::string_view dividend2025 =
        R"(2025-05-15T10:00:00Z,2025-05-15,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Share,XX0000000001,,,14,,-2.10,EUR,,,,,synthetic-dividend-3,,,,)";

    std::optional<ApplicationRun> oneFile = startRun(requestOf(2024,
                                                               {fileOfRows("history.csv",
                                                                           {dividend2023,
                                                                            interest2023,
                                                                            interest2024March,
                                                                            coupon2024,
                                                                            dividend2024,
                                                                            interest2024September,
                                                                            dividend2025})}));
    std::optional<ApplicationRun> yearlyFiles = startRun(
        requestOf(2024,
                  {fileOfRows("2023.csv", {dividend2023, interest2023}),
                   fileOfRows("2024.csv",
                              {interest2024March, coupon2024, dividend2024, interest2024September}),
                   fileOfRows("2025.csv", {dividend2025})}));

    ASSERT_TRUE(oneFile.has_value());
    ASSERT_TRUE(yearlyFiles.has_value());

    // The 2024 forms: Doh-Div the dividend of EUR 12.00 with EUR 1.80 tax (2023 and 2025 are other
    // years); Doh-DHO EUR 700.00 + EUR 800.00 = EUR 1,500.00 (above EUR 1,000, no question);
    // Doh-Obr the coupon of EUR 25.00 and no cash interest (the interest of 2023 is another year);
    // no sale, so Doh-KDVP has no data.
    for (const ApplicationRun* run : {&*oneFile, &*yearlyFiles})
    {
        const ApplicationResult& result = run->result();

        EXPECT_TRUE(result.mRequests.empty());

        ASSERT_TRUE(result.report(ReportType::Dividends).mIncome.has_value());
        ASSERT_EQ(result.report(ReportType::Dividends).mIncome->mRows.size(), 1U);
        EXPECT_EQ(result.report(ReportType::Dividends).mIncome->mGross, 120'000);     // EUR 12.00
        EXPECT_EQ(result.report(ReportType::Dividends).mIncome->mForeignTax, 18'000); // EUR 1.80

        EXPECT_EQ(result.report(ReportType::BankDepositInterest).mStatus, ReportStatus::Generated);
        ASSERT_TRUE(result.report(ReportType::BankDepositInterest).mIncome.has_value());
        EXPECT_EQ(result.report(ReportType::BankDepositInterest).mIncome->mGross,
                  15'000'000); // EUR 1,500.00

        ASSERT_TRUE(result.report(ReportType::OtherInterest).mIncome.has_value());
        ASSERT_EQ(result.report(ReportType::OtherInterest).mIncome->mRows.size(), 1U);
        EXPECT_EQ(result.report(ReportType::OtherInterest).mIncome->mRows[0].mKind,
                  IncomeRowKind::BondCoupon);
        EXPECT_EQ(result.report(ReportType::OtherInterest).mIncome->mGross, 250'000); // EUR 25.00

        EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::NoData);
    }

    // The three documents are the same strings; the file names are not in the XML.
    const ApplicationResult& left = oneFile->result();
    const ApplicationResult& right = yearlyFiles->result();

    ASSERT_TRUE(left.report(ReportType::Dividends).mXml.has_value());
    EXPECT_EQ(left.report(ReportType::Dividends).mXml, right.report(ReportType::Dividends).mXml);
    ASSERT_TRUE(left.report(ReportType::BankDepositInterest).mXml.has_value());
    EXPECT_EQ(left.report(ReportType::BankDepositInterest).mXml,
              right.report(ReportType::BankDepositInterest).mXml);
    ASSERT_TRUE(left.report(ReportType::OtherInterest).mXml.has_value());
    EXPECT_EQ(left.report(ReportType::OtherInterest).mXml,
              right.report(ReportType::OtherInterest).mXml);
}

// The rounding budget is EUR 0.01 per ISIN and run, shared across years: a rounding that the
// source digits prove (2023) is counted before the user is asked about a later sale (2024).
TEST(ProcessorHistoryIntegrationTest, ARoundingCorrectionOf2023CountsInTheBudgetOfThe2024Question) {
    std::optional<ApplicationRun> run = startRun(requestOf(
        2024,
        {fileOfRows(
            "history.csv",
            {
                // Three purchases of 0.3333333340 units at EUR 30 (amount EUR 10.00): stored as
                // 0.33333333 each, 0.99999999 in all, while the source digits add up
                // to 1.000000002.
                R"(2023-01-10T09:00:00Z,2023-01-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,0.3333333340,30,-10,,,EUR,,,,,synthetic-buy-1,,,,)",
                R"(2023-01-11T09:00:00Z,2023-01-11,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,0.3333333340,30,-10,,,EUR,,,,,synthetic-buy-2,,,,)",
                R"(2023-01-12T09:00:00Z,2023-01-12,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,0.3333333340,30,-10,,,EUR,,,,,synthetic-buy-3,,,,)",
                // The sale of 1 unit finds 0.99999999: the missing 0.00000001 is proven rounding.
                R"(2023-03-10T09:00:00Z,2023-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-1,36,36,,,EUR,,,,,synthetic-sell-1,,,,)",
                R"(2024-02-01T09:00:00Z,2024-02-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,0.99999999,30,-30,,,EUR,,,,,synthetic-buy-4,,,,)",
                // The same shortage of 0.00000001 without proof in the digits: the user is asked.
                R"(2024-03-04T09:00:00Z,2024-03-04,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-1,36,36,,,EUR,,,,,synthetic-sell-2,,,,)",
            })}));

    ASSERT_TRUE(run.has_value());

    const ApplicationResult& asked = run->result();

    ASSERT_EQ(asked.mRequests.size(), 1U);
    EXPECT_EQ(asked.mRequests[0].mType, RequestType::QuantityCorrectionRequired);
    EXPECT_EQ(asked.report(ReportType::CapitalGains).mStatus, ReportStatus::NeedsInput);

    const QuantityCorrectionQuestion* question =
        std::get_if<QuantityCorrectionQuestion>(&asked.mRequests[0].mData);

    ASSERT_NE(question, nullptr);
    EXPECT_EQ(question->mSaleDate, day("2024-03-04"));
    EXPECT_EQ(question->mOriginalUnits, 100'000'000);
    EXPECT_EQ(question->mOpenUnits, 99'999'999); // the proposed quantity
    // The 2023 rounding removed 0.00000001 units at the sale price of EUR 36 (the lot value
    // 10 / 0.33333333 = EUR 30.0000003 is lower): 0.00000001 x 36 = EUR 0.00000036 = 9/25000000.
    EXPECT_EQ(question->mBudgetUsed, eur(9, 25'000'000));

    const std::string requestId = asked.mRequests[0].mId;

    EXPECT_TRUE(
        answerRequest(*run, requestId, QuantityCorrectionAnswer{.mCorrectedUnits = 99'999'999})
            .empty());

    const ApplicationResult& answered = run->result();

    EXPECT_TRUE(answered.mRequests.empty());
    EXPECT_EQ(answered.report(ReportType::CapitalGains).mStatus, ReportStatus::Generated);

    // The impact of the ISIN is the two roundings together: EUR 0.00000036 + EUR 0.00000036 =
    // EUR 0.00000072 = 18/25000000 (the lot of the second purchase is worth EUR 30 / 0.99999999 =
    // EUR 30.0000003, below the sale price of EUR 36).
    ASSERT_EQ(answered.mReconciliation.size(), 1U);
    EXPECT_EQ(answered.mReconciliation[0].mIsin, "XX0000000001");
    EXPECT_EQ(answered.mReconciliation[0].mImpact, eur(18, 25'000'000));

    // The sale row of 2024 has the corrected 0.99999999 units (F7); the sale of 2023 has no row.
    ASSERT_TRUE(answered.report(ReportType::CapitalGains).mXml.has_value());
    EXPECT_NE(answered.report(ReportType::CapitalGains).mXml->find("<F7>0.99999999</F7>"),
              std::string::npos);

    // Two corrections are applied: the 2023 sale (proven by the source digits) and the 2024 sale
    // (confirmed by the user), each with the impact EUR 0.00000036. Only the 2024 sale is in the
    // 2024 report.
    ASSERT_EQ(answered.mCorrections.size(), 2U);
    EXPECT_EQ(answered.mCorrections[0].mSaleDate, day("2023-03-10"));
    EXPECT_EQ(answered.mCorrections[0].mOriginalUnits, 100'000'000);
    EXPECT_EQ(answered.mCorrections[0].mCorrectedUnits, 99'999'999);
    EXPECT_EQ(answered.mCorrections[0].mKind, CorrectionKind::RoundingEvidence);
    EXPECT_EQ(answered.mCorrections[0].mImpact, eur(9, 25'000'000));
    EXPECT_EQ(answered.mCorrections[1].mSaleDate, day("2024-03-04"));
    EXPECT_EQ(answered.mCorrections[1].mOriginalUnits, 100'000'000);
    EXPECT_EQ(answered.mCorrections[1].mCorrectedUnits, 99'999'999);
    EXPECT_EQ(answered.mCorrections[1].mKind, CorrectionKind::UserConfirmed);
    EXPECT_EQ(answered.mCorrections[1].mImpact, eur(9, 25'000'000));
    EXPECT_EQ(diagnosticsWithKey(answered, MessageKey::QuantityRoundingReconciled).size(), 1U);
    EXPECT_EQ(diagnosticsWithKey(answered, MessageKey::SourceQuantityCorrected).size(), 1U);
}

// A split that is booked on 8 January 2025 and is in two files matters for the year 2024: it is
// inside the 30-day window of a loss sale of December, and the files may describe it differently.
TEST(ProcessorHistoryIntegrationTest, TheJanuarySplitRowOfTwoFilesWarnsForTheNextYear) {
    constexpr std::string_view buy =
        R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,4,10,-40,,,EUR,,,,,synthetic-buy-1,,,,)";
    constexpr std::string_view lossSell =
        R"(2024-12-10T09:00:00Z,2024-12-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-1,5,5,,,EUR,,,,,synthetic-sell-1,,,,)";
    constexpr std::string_view split =
        R"(2025-01-08T09:00:00Z,2025-01-08,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,3,,,,,,,,,,synthetic-split-1,,,,)";

    std::optional<ApplicationRun> run =
        startRun(requestOf(2024,
                           {fileOfRows("a.csv", {buy, lossSell, split}),
                            fileOfRows("b.csv", {buy, lossSell, split})}));

    ASSERT_TRUE(run.has_value());

    const ApplicationResult& result = run->result();

    // The same rows in two files are one event with two sources, and the rows of 2025 come from
    // two files: one warning for the ISIN, with the year of the rows.
    const std::vector<const ApplicationDiagnostic*> warnings =
        diagnosticsWithKey(result, MessageKey::CorporateActionSourcesMismatchPossible);

    ASSERT_EQ(warnings.size(), 1U);
    EXPECT_EQ(warnings[0]->mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(warnings[0]->mIsin, "XX0000000001");
    EXPECT_EQ(warnings[0]->mMessageParams, (std::vector<MessageParam>{numberParam("year", 2025)}));

    // The split row is asked as one action with its effective date; Doh-KDVP waits for the ratio.
    ASSERT_FALSE(result.mRequests.empty());
    EXPECT_EQ(result.mRequests[0].mType, RequestType::CorporateActionRatioRequired);
    EXPECT_EQ(std::get<CorporateActionRatioQuestion>(result.mRequests[0].mData).mEffectiveDate,
              day("2025-01-08"));
    EXPECT_EQ(result.report(ReportType::CapitalGains).mStatus, ReportStatus::NeedsInput);
}

// A confirmation applies only to the action identity, its complete source set and the units held
// before it. A new source row makes a new question.
TEST(ProcessorHistoryIntegrationTest, ANewSplitRowAfterARestartAsksAgainAndTheRatioIsNotUsed) {
    constexpr std::string_view buy =
        R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,20,-200,,,EUR,,,,,synthetic-buy-1,,,,)";
    constexpr std::string_view split1 =
        R"(2024-06-10T09:00:00Z,2024-06-10,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,10,,,,,,,,,,synthetic-split-1,,,,)";
    constexpr std::string_view split2 =
        R"(2024-06-10T09:00:00Z,2024-06-10,DEFAULT,CORPORATE_ACTION,SPLIT,STOCK,Synthetic Share,XX0000000001,10,,,,,,,,,,synthetic-split-2,,,,)";
    constexpr std::string_view sell =
        R"(2024-09-16T09:00:00Z,2024-09-16,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-20,12,240,,,EUR,,,,,synthetic-sell-1,,,,)";

    const ApplicationRequest oneRow = requestOf(2024, {fileOfRows("a.csv", {buy, split1, sell})});
    std::optional<ApplicationRun> run = startRun(oneRow);

    ASSERT_TRUE(run.has_value());
    ASSERT_EQ(run->result().mRequests.size(), 1U);
    EXPECT_EQ(run->result().mRequests[0].mType, RequestType::CorporateActionRatioRequired);

    // 10 units with 2 / 1 are 20 units after the split; the sale of 20 units then fits.
    const std::string ratioId = run->result().mRequests[0].mId;

    EXPECT_TRUE(answerRequest(*run,
                              ratioId,
                              SplitRatioAnswer{.mNewShares = 2,
                                               .mOldShares = 1,
                                               .mEffectiveDate = day("2024-06-10"),
                                               .mConfirmedUnits = 2'000'000'000})
                    .empty());
    EXPECT_TRUE(run->result().mRequests.empty());
    EXPECT_EQ(run->result().report(ReportType::CapitalGains).mStatus, ReportStatus::Generated);

    // A second file brings a second split row of the same day with another transaction ID: the
    // source set of the action is not the one that was confirmed.
    const ApplicationRequest twoRows =
        requestOf(2024, {fileOfRows("a.csv", {buy, split1, sell}), fileOfRows("b.csv", {split2})});

    EXPECT_FALSE(run->restart(twoRows).has_value());

    const ApplicationResult& restarted = run->result();

    ASSERT_EQ(restarted.mRequests.size(), 1U);
    EXPECT_EQ(restarted.mRequests[0].mType, RequestType::CorporateActionIdentityRequired);
    EXPECT_EQ(restarted.report(ReportType::CapitalGains).mStatus, ReportStatus::NeedsInput);

    for (const RecordedDecision& decision : restarted.mDecisions)
    {
        EXPECT_FALSE(decision.mUsed);
    }

    // The two rows are one action: the ratio is asked again, because the earlier answer was for
    // one row.
    const std::string identityId = restarted.mRequests[0].mId;

    EXPECT_TRUE(
        answerRequest(*run, identityId, CorporateActionGroupingAnswer{.mActionOfRow = {1, 1}})
            .empty());

    const ApplicationResult& grouped = run->result();

    ASSERT_EQ(grouped.mRequests.size(), 1U);
    EXPECT_EQ(grouped.mRequests[0].mType, RequestType::CorporateActionRatioRequired);
    EXPECT_EQ(grouped.report(ReportType::CapitalGains).mStatus, ReportStatus::NeedsInput);
}

} // namespace
