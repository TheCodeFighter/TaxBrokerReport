#include "../support/kdvp_row_fifo.hpp"
#include "../support/xsd_validation.hpp"
#include "parsers/traderepublic_parser.hpp"
#include "processors/capital_gains_report_builder.hpp"
#include "processors/capital_report_gates.hpp"
#include "processors/fifo_matcher.hpp"
#include "processors/loss_eligibility.hpp"
#include "processors/tax_processor.hpp"
#include "taxbroker/decision_session.hpp"
#include "xml/kdvp_xml_writer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace taxbroker;
using taxbroker::test::expectValidForKdvpSchemas;
using taxbroker::test::fifoOverKdvpRows;

class TemporaryLossInputs {
  public:
    TemporaryLossInputs() {
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
        mDirectory = std::filesystem::temp_directory_path() /
                     ("taxbroker_loss_synthetic_" + std::to_string(token));

        if (!std::filesystem::create_directory(mDirectory))
        {
            throw std::runtime_error{"Unable to create synthetic loss directory"};
        }
    }

    TemporaryLossInputs(const TemporaryLossInputs&) = delete;
    TemporaryLossInputs& operator=(const TemporaryLossInputs&) = delete;

    ~TemporaryLossInputs() {
        std::error_code error;
        std::filesystem::remove_all(mDirectory, error);
    }

    /// Writes exactly the synthetic rows supplied by each scenario.
    std::filesystem::path write(std::string_view aName,
                                std::initializer_list<std::string_view> aRows) const {
        const auto path = mDirectory / aName;
        std::ofstream output{path};

        output << "datetime,date,account_type,category,type,asset_class,name,symbol,shares,price,"
                  "amount,fee,tax,currency,original_amount,original_currency,fx_rate,description,"
                  "transaction_id,counterparty_name,counterparty_iban,payment_reference,mcc_code\n";

        for (const auto row : aRows)
        {
            output << row << '\n';
        }

        output.close();

        if (!output)
        {
            throw std::runtime_error{"Unable to write synthetic loss CSV"};
        }

        return path;
    }

  private:
    std::filesystem::path mDirectory;
};

/// The capital-gains part of one run, as the CLI runs it, from the parsed files: merge, year,
/// FIFO, the loss rule with the session's answers, the report builder, the gates and the writer.
ApplicationResultInput calculateCapitalGains(const std::vector<ParseResult>& aParsed,
                                             int aYear,
                                             RunDecisions& aDecisions) {
    std::vector<StatementMergeInput> inputs;

    for (std::size_t index = 0; index < aParsed.size(); ++index)
    {
        inputs.push_back({index, aParsed[index]});
    }

    auto prepared = TaxProcessor::prepareReportingYear(
        {aYear, DeterministicStatementMerger{}.merge(std::move(inputs))});
    const auto& ledger = prepared.mLedger.value();
    const auto& statement = prepared.mMerged.mStatement;
    auto fifo = FifoMatcher::match(statement, ledger);
    auto loss = LossEligibilityChecker::check(statement, ledger, fifo, aDecisions);
    const auto built = CapitalGainsReportBuilder::build(statement, ledger, fifo, loss, {});
    auto gates = CapitalReportGates::check(statement, ledger, fifo, loss, built);
    const bool hasGateError =
        std::any_of(gates.begin(), gates.end(), [](const ProcessingDiagnostic& aDiagnostic) {
            return aDiagnostic.mSeverity == DiagnosticSeverity::Error;
        });

    ApplicationResultInput input{
        .mReportingYear = aYear,
        .mMergeDiagnostics = std::move(prepared.mMerged.mDiagnostics),
        .mProcessingDiagnostics = std::move(prepared.mDiagnostics),
    };

    std::move(fifo.mDiagnostics.begin(),
              fifo.mDiagnostics.end(),
              std::back_inserter(input.mProcessingDiagnostics));
    appendStageOutput(input, std::move(loss.mDiagnostics), std::move(loss.mRequests));
    std::move(gates.begin(), gates.end(), std::back_inserter(input.mProcessingDiagnostics));

    if (built.mReport && !hasGateError && !built.mReport->mLists.empty())
    {
        const ReportMetadata metadata{
            .mTaxpayer = {.mTaxNumber = "12345678", .mIsResident = true},
            .mReportingYear = aYear,
        };

        input.mReports[0].mXml = KdvpXmlWriter::write(metadata, *built.mReport).mXml;
    }

    return input;
}

/// The question data of the request at aIndex.
const LossEligibilityQuestion& lossCheckAt(const ApplicationResult& aResult, std::size_t aIndex) {
    return std::get<LossEligibilityQuestion>(aResult.mRequests.at(aIndex).mData);
}

bool contains(const std::string& aText, std::string_view aPart) {
    return aText.find(aPart) != std::string::npos;
}

constexpr auto kCapital = ReportType::CapitalGains;
// The answers below reply to the first run, which has revision 1.
constexpr std::uint64_t kFirstRunRevision = 1;

// The base scenario: FURS example 7 a year later. 25 of the 50 units bought on 2023-12-12 are
// sold at a loss on 2024-01-05; that purchase is the sale's own, so no unit is replaced.
constexpr std::string_view kBasePurchase =
    R"(2023-12-12T09:00:00Z,2023-12-12,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,50,10,-500,,,EUR,,,,,synthetic-buy-1,,,,)";
constexpr std::string_view kBaseSale =
    R"(2024-01-05T09:00:00Z,2024-01-05,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-25,8,200,,,EUR,,,,,synthetic-sell-1,,,,)";

TEST(LossEligibilityIntegrationTest, AnsweringNoKeepsTheLoss) {
    const TemporaryLossInputs files;
    tr::TradeRepublicParser parser;
    const std::vector<ParseResult> parsed{
        parser.parse(files.write("history.csv", {kBasePurchase, kBaseSale}))};
    const auto calculation = [&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions);
    };
    DecisionSession session;

    const auto& first = session.start(calculation);

    EXPECT_EQ(first.mRunRevision, 1U);
    EXPECT_EQ(first.report(kCapital).mStatus, ReportStatus::NeedsInput);
    ASSERT_EQ(first.mRequests.size(), 1U);
    EXPECT_EQ(first.mRequests[0].mId, "request-0001");
    EXPECT_EQ(first.mRequests[0].mType, RequestType::LossEligibilityConfirmationRequired);
    ASSERT_EQ(lossCheckAt(first, 0).mSales.size(), 1U);
    EXPECT_EQ(lossCheckAt(first, 0).mSales[0].mOpenUnits, 2'500'000'000); // 25 units
    ASSERT_EQ(first.mDiagnostics.size(), 1U);
    EXPECT_EQ(first.mDiagnostics[0].mId, "diagnostic-0001");
    EXPECT_EQ(first.mDiagnostics[0].mMessageKey,
              MessageKey::LossEligibilityUnresolvedConfirmationRequired);
    EXPECT_EQ(first.mDiagnostics[0].mRequestId, "request-0001");

    const auto rejections =
        session.answer({{"request-0001", kFirstRunRevision, NoOtherActivityAnswer{}}});

    EXPECT_TRUE(rejections.empty());

    const auto& second = session.result();

    EXPECT_EQ(second.mRunRevision, 2U);
    EXPECT_EQ(second.report(kCapital).mStatus, ReportStatus::Generated);
    EXPECT_TRUE(second.mRequests.empty());
    EXPECT_TRUE(second.mDiagnostics.empty());
    ASSERT_EQ(second.mDecisions.size(), 1U);
    EXPECT_TRUE(second.mDecisions[0].mUsed);

    const auto& xml = second.report(kCapital).mXml.value();

    EXPECT_TRUE(contains(xml, R"xml(          <Row>
            <ID>0</ID>
            <Purchase>
              <F1>2023-12-12</F1>
              <F2>B</F2>
              <F3>25.0000</F3>
              <F4>10.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>25.0000</F8>
          </Row>
          <Row>
            <ID>1</ID>
            <Sale>
              <F6>2024-01-05</F6>
              <F7>25.0000</F7>
              <F9>8.0000</F9>
              <F10>true</F10>
            </Sale>
            <F8>0.0000</F8>
          </Row>
)xml"));
    expectValidForKdvpSchemas(xml);

    const auto fifoLists = fifoOverKdvpRows(xml);

    ASSERT_EQ(fifoLists.size(), 1U);
    EXPECT_TRUE(fifoLists[0].mProblems.empty());
    // 25 x 8 - 25 x 10 = -50: a loss that may reduce the gains.
    EXPECT_EQ(fifoLists[0].mSaleGains, (std::vector<std::string>{"-50"}));
}

TEST(LossEligibilityIntegrationTest, AnsweringYesBlocksTheWholeOpenLoss) {
    const TemporaryLossInputs files;
    tr::TradeRepublicParser parser;
    const std::vector<ParseResult> parsed{
        parser.parse(files.write("history.csv", {kBasePurchase, kBaseSale}))};
    const auto calculation = [&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions);
    };
    DecisionSession session;
    session.start(calculation);

    const auto rejections =
        session.answer({{"request-0001", kFirstRunRevision, OtherActivityAnswer{}}});

    EXPECT_TRUE(rejections.empty());

    const auto& result = session.result();

    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Generated);

    // The safe side: all 25 units are blocked, and the user corrects it in eDavki.
    const auto& xml = result.report(kCapital).mXml.value();

    EXPECT_TRUE(contains(xml, R"xml(            <Sale>
              <F6>2024-01-05</F6>
              <F7>25.0000</F7>
              <F9>8.0000</F9>
              <F10>false</F10>
            </Sale>
)xml"));
    expectValidForKdvpSchemas(xml);
}

TEST(LossEligibilityIntegrationTest, APartlyBlockedLossIsNotReportableYet) {
    // FURS example 10 a year later: the 2023-12-12 purchase blocks 10 of the 40 loss units. With
    // `no`, the other 30 keep their loss: one sale row cannot say that (#267).
    constexpr std::string_view olderPurchase =
        R"(2023-02-01T09:00:00Z,2023-02-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,50,10,-500,,,EUR,,,,,synthetic-buy-1,,,,)";
    constexpr std::string_view decemberPurchase =
        R"(2023-12-12T09:00:00Z,2023-12-12,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-2,,,,)";
    constexpr std::string_view sale =
        R"(2024-01-05T09:00:00Z,2024-01-05,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-40,8,320,,,EUR,,,,,synthetic-sell-1,,,,)";

    const TemporaryLossInputs files;
    tr::TradeRepublicParser parser;
    const std::vector<ParseResult> parsed{
        parser.parse(files.write("history.csv", {olderPurchase, decemberPurchase, sale}))};
    const auto calculation = [&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions);
    };
    DecisionSession session;

    const auto& first = session.start(calculation);

    ASSERT_EQ(first.mRequests.size(), 1U);
    ASSERT_EQ(lossCheckAt(first, 0).mSales.size(), 1U);
    EXPECT_EQ(lossCheckAt(first, 0).mSales[0].mKnownIneligibleUnits, 1'000'000'000); // 10 units
    EXPECT_EQ(lossCheckAt(first, 0).mSales[0].mOpenUnits, 3'000'000'000);            // 30 units

    ASSERT_TRUE(
        session.answer({{"request-0001", kFirstRunRevision, NoOtherActivityAnswer{}}}).empty());

    const auto& second = session.result();

    EXPECT_EQ(second.report(kCapital).mStatus, ReportStatus::Failed);
    EXPECT_FALSE(second.report(kCapital).mXml.has_value());
    ASSERT_EQ(second.mDiagnostics.size(), 1U);
    EXPECT_EQ(std::get<ProcessingDiagnosticCode>(second.mDiagnostics[0].mCode),
              ProcessingDiagnosticCode::UnsupportedTaxTreatment);
    EXPECT_EQ(second.mDiagnostics[0].mMessageKey,
              MessageKey::UnsupportedTaxTreatmentPartialLossEligibility);
    EXPECT_EQ(second.mDiagnostics[0].mTaxDate,
              Date{std::chrono::sys_days{std::chrono::year{2024} / 1 / 5}.time_since_epoch()});
    EXPECT_TRUE(second.mDiagnostics[0].mBlocking);
}

TEST(LossEligibilityIntegrationTest, AnsweringYesBlocksAPartlyKnownLossInFull) {
    // The same history; `yes` blocks the 30 open units too, so all 40 listed units are blocked
    // and one row with F10 false says it.
    constexpr std::string_view olderPurchase =
        R"(2023-02-01T09:00:00Z,2023-02-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,50,10,-500,,,EUR,,,,,synthetic-buy-1,,,,)";
    constexpr std::string_view decemberPurchase =
        R"(2023-12-12T09:00:00Z,2023-12-12,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-2,,,,)";
    constexpr std::string_view sale =
        R"(2024-01-05T09:00:00Z,2024-01-05,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-40,8,320,,,EUR,,,,,synthetic-sell-1,,,,)";

    const TemporaryLossInputs files;
    tr::TradeRepublicParser parser;
    const std::vector<ParseResult> parsed{
        parser.parse(files.write("history.csv", {olderPurchase, decemberPurchase, sale}))};
    const auto calculation = [&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions);
    };
    DecisionSession session;
    session.start(calculation);

    ASSERT_TRUE(
        session.answer({{"request-0001", kFirstRunRevision, OtherActivityAnswer{}}}).empty());

    const auto& result = session.result();

    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Generated);

    // The sale uses 40 of the 50 units of 2023-02-01; the December purchase has no row.
    const auto& xml = result.report(kCapital).mXml.value();

    EXPECT_TRUE(contains(xml, R"xml(            <Purchase>
              <F1>2023-02-01</F1>
              <F2>B</F2>
              <F3>40.0000</F3>
              <F4>10.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>40.0000</F8>
)xml"));
    EXPECT_TRUE(contains(xml, R"xml(              <F7>40.0000</F7>
              <F9>8.0000</F9>
              <F10>false</F10>
)xml"));
    EXPECT_FALSE(contains(xml, "<F1>2023-12-12</F1>"));
    expectValidForKdvpSchemas(xml);
}

TEST(LossEligibilityIntegrationTest, CancellingTheLossQuestionFailsTheReport) {
    const TemporaryLossInputs files;
    tr::TradeRepublicParser parser;
    const std::vector<ParseResult> parsed{
        parser.parse(files.write("history.csv", {kBasePurchase, kBaseSale}))};
    const auto calculation = [&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions);
    };
    DecisionSession session;
    session.start(calculation);

    ASSERT_TRUE(session.answer({{"request-0001", kFirstRunRevision, CancelAnswer{}}}).empty());

    // The exclusion choice after a cancel comes with #227; until then the error fails the form.
    const auto& result = session.result();

    EXPECT_EQ(result.mRunRevision, 2U);
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Failed);
    EXPECT_TRUE(result.mRequests.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(std::get<ProcessingDiagnosticCode>(result.mDiagnostics[0].mCode),
              ProcessingDiagnosticCode::LossEligibilityUnresolved);
    EXPECT_EQ(result.mDiagnostics[0].mMessageKey, MessageKey::LossEligibilityUnresolvedCancelled);
    EXPECT_TRUE(result.mDiagnostics[0].mBlocking);
    EXPECT_FALSE(result.mDiagnostics[0].mRequestId.has_value());
}

TEST(LossEligibilityIntegrationTest, ANewRunWithTheSameFileUsesTheRecordedAnswer) {
    const TemporaryLossInputs files;
    tr::TradeRepublicParser parser;
    const std::vector<ParseResult> parsed{
        parser.parse(files.write("history.csv", {kBasePurchase, kBaseSale}))};
    const auto calculation = [&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions);
    };
    DecisionSession session;
    session.start(calculation);
    ASSERT_TRUE(
        session.answer({{"request-0001", kFirstRunRevision, NoOtherActivityAnswer{}}}).empty());

    const auto& result = session.start(calculation);

    EXPECT_EQ(result.mRunRevision, 3U);
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Generated);
    EXPECT_TRUE(result.mRequests.empty());
    ASSERT_EQ(result.mDecisions.size(), 1U);
    EXPECT_EQ(result.mDecisions[0].mId, "decision-0001");
    EXPECT_TRUE(result.mDecisions[0].mFromEarlierRun);
    EXPECT_TRUE(result.mDecisions[0].mUsed);
}

TEST(LossEligibilityIntegrationTest, AnAddedPurchaseAsksANewQuestion) {
    // The second run adds a file with 10 units bought on 2024-01-20, 15 days after the sale: they
    // block 10 of the 25 units, so the question is a new one.
    constexpr std::string_view addedPurchase =
        R"(2024-01-20T09:00:00Z,2024-01-20,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,9,-90,,,EUR,,,,,synthetic-buy-2,,,,)";

    const TemporaryLossInputs files;
    tr::TradeRepublicParser parser;
    const std::vector<ParseResult> firstFiles{
        parser.parse(files.write("history.csv", {kBasePurchase, kBaseSale}))};
    const std::vector<ParseResult> secondFiles{
        parser.parse(files.write("history.csv", {kBasePurchase, kBaseSale})),
        parser.parse(files.write("january.csv", {addedPurchase}), 1)};
    DecisionSession session;
    session.start([&firstFiles](RunDecisions& aDecisions) {
        return calculateCapitalGains(firstFiles, 2024, aDecisions);
    });
    ASSERT_TRUE(
        session.answer({{"request-0001", kFirstRunRevision, NoOtherActivityAnswer{}}}).empty());

    const auto& result = session.start([&secondFiles](RunDecisions& aDecisions) {
        return calculateCapitalGains(secondFiles, 2024, aDecisions);
    });

    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::NeedsInput);
    ASSERT_EQ(result.mRequests.size(), 1U);
    ASSERT_EQ(lossCheckAt(result, 0).mSales.size(), 1U);

    const auto& facts = lossCheckAt(result, 0).mSales[0];

    EXPECT_EQ(facts.mKnownIneligibleUnits, 1'000'000'000); // 10 units
    EXPECT_EQ(facts.mOpenUnits, 1'500'000'000);            // 15 units
    ASSERT_EQ(facts.mKnownReplacements.size(), 1U);
    EXPECT_EQ(facts.mKnownReplacements[0].mDate,
              Date{std::chrono::sys_days{std::chrono::year{2024} / 1 / 20}.time_since_epoch()});

    // The old answer stays listed, but no stage asked its question.
    ASSERT_EQ(result.mDecisions.size(), 1U);
    EXPECT_FALSE(result.mDecisions[0].mUsed);
}

TEST(LossEligibilityIntegrationTest, DayThirtyCountsAndDayThirtyOneDoesNot) {
    // XX0000000001 has purchases 30 days before (2) and after (3) its sale of 2024-03-10;
    // XX0000000002 has them 31 days before and after.
    const TemporaryLossInputs files;
    tr::TradeRepublicParser parser;
    const std::vector<ParseResult> parsed{parser.parse(files.write(
        "history.csv",
        {
            R"(2023-06-01T09:00:00Z,2023-06-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,100,10,-1000,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2023-06-01T10:00:00Z,2023-06-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Fund,XX0000000002,100,10,-1000,,,EUR,,,,,synthetic-buy-2,,,,)",
            R"(2024-02-08T09:00:00Z,2024-02-08,DEFAULT,TRADING,BUY,STOCK,Synthetic Fund,XX0000000002,2,10,-20,,,EUR,,,,,synthetic-buy-3,,,,)",
            R"(2024-02-09T09:00:00Z,2024-02-09,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-buy-4,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,8,80,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2024-03-10T10:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Fund,XX0000000002,-10,8,80,,,EUR,,,,,synthetic-sell-2,,,,)",
            R"(2024-04-09T09:00:00Z,2024-04-09,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,3,10,-30,,,EUR,,,,,synthetic-buy-5,,,,)",
            R"(2024-04-10T09:00:00Z,2024-04-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Fund,XX0000000002,3,10,-30,,,EUR,,,,,synthetic-buy-6,,,,)",
        }))};
    DecisionSession session;

    const auto& result = session.start([&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions);
    });

    ASSERT_EQ(result.mRequests.size(), 2U);

    const auto& share = lossCheckAt(result, 0);
    const auto& fund = lossCheckAt(result, 1);

    EXPECT_EQ(share.mIsin, "XX0000000001");
    ASSERT_EQ(share.mSales.size(), 1U);
    EXPECT_EQ(share.mSales[0].mKnownIneligibleUnits, 500'000'000); // 2 + 3 units
    EXPECT_EQ(share.mSales[0].mOpenUnits, 500'000'000);            // 5 units

    EXPECT_EQ(fund.mIsin, "XX0000000002");
    ASSERT_EQ(fund.mSales.size(), 1U);
    EXPECT_EQ(fund.mSales[0].mKnownIneligibleUnits, 0);
    EXPECT_EQ(fund.mSales[0].mOpenUnits, 1'000'000'000); // 10 units
}

TEST(LossEligibilityIntegrationTest, ADecember31LossCountsJanuary30AndNotJanuary31) {
    // Both ISINs sell 10 at a loss on 2024-12-31 and buy 10 again in a second file:
    // XX0000000001 on 2025-01-30 (day 30, the last day of the window), XX0000000002 on
    // 2025-01-31 (day 31). Only XX0000000002 gets a question.
    const TemporaryLossInputs files;
    tr::TradeRepublicParser parser;
    const std::vector<ParseResult> parsed{
        parser.parse(files.write(
            "history.csv",
            {
                R"(2024-06-03T09:00:00Z,2024-06-03,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,100,10,-1000,,,EUR,,,,,synthetic-buy-1,,,,)",
                R"(2024-06-03T10:00:00Z,2024-06-03,DEFAULT,TRADING,BUY,STOCK,Synthetic Fund,XX0000000002,100,10,-1000,,,EUR,,,,,synthetic-buy-2,,,,)",
                R"(2024-12-31T09:00:00Z,2024-12-31,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,8,80,,,EUR,,,,,synthetic-sell-1,,,,)",
                R"(2024-12-31T10:00:00Z,2024-12-31,DEFAULT,TRADING,SELL,STOCK,Synthetic Fund,XX0000000002,-10,8,80,,,EUR,,,,,synthetic-sell-2,,,,)",
            })),
        parser.parse(
            files.write(
                "january.csv",
                {
                    R"(2025-01-30T09:00:00Z,2025-01-30,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,8,-80,,,EUR,,,,,synthetic-buy-3,,,,)",
                    R"(2025-01-31T09:00:00Z,2025-01-31,DEFAULT,TRADING,BUY,STOCK,Synthetic Fund,XX0000000002,10,8,-80,,,EUR,,,,,synthetic-buy-4,,,,)",
                }),
            1)};
    DecisionSession session;

    const auto& result = session.start([&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions);
    });

    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::NeedsInput);
    ASSERT_EQ(result.mRequests.size(), 1U);

    const auto& fund = lossCheckAt(result, 0);

    EXPECT_EQ(fund.mIsin, "XX0000000002");
    ASSERT_EQ(fund.mSales.size(), 1U);
    EXPECT_EQ(fund.mSales[0].mKnownIneligibleUnits, 0);
    EXPECT_EQ(fund.mSales[0].mOpenUnits, 1'000'000'000); // 10 units
    EXPECT_TRUE(fund.mSales[0].mKnownReplacements.empty());
    EXPECT_EQ(fund.mSales[0].mWindowEnd,
              Date{std::chrono::sys_days{std::chrono::year{2025} / 1 / 30}.time_since_epoch()});
}

TEST(LossEligibilityIntegrationTest, ASameDayRebuyBlocksTheLoss) {
    // On 2024-03-01: buy 10 at 09:00, sell all 10 at a loss at 11:00, buy 10 again at 13:00.
    // The rebuy comes after the sale, so it replaces all 10 units: no question, F10 false.
    const TemporaryLossInputs files;
    tr::TradeRepublicParser parser;
    const std::vector<ParseResult> parsed{parser.parse(files.write(
        "history.csv",
        {
            R"(2024-03-01T09:00:00Z,2024-03-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,10,-100,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-01T11:00:00Z,2024-03-01,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,8,80,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2024-03-01T13:00:00Z,2024-03-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,8,-80,,,EUR,,,,,synthetic-buy-2,,,,)",
        }))};
    DecisionSession session;

    const auto& result = session.start([&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions);
    });

    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Generated);
    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_TRUE(result.mDiagnostics.empty());

    const auto& xml = result.report(kCapital).mXml.value();

    // The 13:00 purchase is not used by a sale of the year, so it has no row.
    EXPECT_TRUE(contains(xml, R"xml(          <Row>
            <ID>0</ID>
            <Purchase>
              <F1>2024-03-01</F1>
              <F2>B</F2>
              <F3>10.0000</F3>
              <F4>10.0000</F4>
              <F5>0.0000</F5>
            </Purchase>
            <F8>10.0000</F8>
          </Row>
          <Row>
            <ID>1</ID>
            <Sale>
              <F6>2024-03-01</F6>
              <F7>10.0000</F7>
              <F9>8.0000</F9>
              <F10>false</F10>
            </Sale>
            <F8>0.0000</F8>
          </Row>
)xml"));
    expectValidForKdvpSchemas(xml);

    const auto fifoLists = fifoOverKdvpRows(xml);

    ASSERT_EQ(fifoLists.size(), 1U);
    EXPECT_TRUE(fifoLists[0].mProblems.empty());
    // 10 x 8 - 10 x 10 = -20: a loss the 30-day rule blocks.
    EXPECT_EQ(fifoLists[0].mSaleGains, (std::vector<std::string>{"-20"}));
}

TEST(LossEligibilityIntegrationTest, AJanuaryPurchaseInALaterFileBlocksADecemberLoss) {
    // tax_rules.md: buy 100 in September 2023, sell 4 at a loss on 2024-12-20, buy 4 on
    // 2025-01-05 (in a second file). No question: all 4 units are blocked.
    const TemporaryLossInputs files;
    tr::TradeRepublicParser parser;
    const std::vector<ParseResult> parsed{
        parser.parse(files.write(
            "history.csv",
            {
                R"(2023-09-01T09:00:00Z,2023-09-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,100,10,-1000,,,EUR,,,,,synthetic-buy-1,,,,)",
                R"(2024-12-20T09:00:00Z,2024-12-20,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-4,8,32,,,EUR,,,,,synthetic-sell-1,,,,)",
            })),
        parser.parse(
            files.write(
                "january.csv",
                {
                    R"(2025-01-05T09:00:00Z,2025-01-05,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,4,8,-32,,,EUR,,,,,synthetic-buy-2,,,,)",
                }),
            1)};
    DecisionSession session;

    const auto& result = session.start([&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions);
    });

    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Generated);
    EXPECT_TRUE(result.mRequests.empty());

    const auto& xml = result.report(kCapital).mXml.value();

    EXPECT_TRUE(contains(xml, "<F10>false</F10>"));
    EXPECT_FALSE(contains(xml, "<F1>2025-01-05</F1>"));

    const auto fifoLists = fifoOverKdvpRows(xml);

    ASSERT_EQ(fifoLists.size(), 1U);
    // F8: 4 purchased, 4 - 4 = 0. Gain 4 x 8 - 4 x 10 = -8.
    EXPECT_EQ(fifoLists[0].mWrittenF8, (std::vector<std::string>{"4", "0"}));
    EXPECT_EQ(fifoLists[0].mSaleGains, (std::vector<std::string>{"-8"}));
}

TEST(LossEligibilityIntegrationTest, AJanuaryReplacementOfTheYearBeforeIsNotUsedAgain) {
    // The 2024-01-05 purchase is taken by the loss of 2023-12-20, so the loss of 2024-01-10
    // has no known replacement and all its 10 units are open.
    const TemporaryLossInputs files;
    tr::TradeRepublicParser parser;
    const std::vector<ParseResult> parsed{parser.parse(files.write(
        "history.csv",
        {
            R"(2023-06-01T09:00:00Z,2023-06-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,100,10,-1000,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2023-12-20T09:00:00Z,2023-12-20,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,8,80,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2024-01-05T09:00:00Z,2024-01-05,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,10,8,-80,,,EUR,,,,,synthetic-buy-2,,,,)",
            R"(2024-01-10T09:00:00Z,2024-01-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-10,7,70,,,EUR,,,,,synthetic-sell-2,,,,)",
        }))};
    DecisionSession session;

    const auto& result = session.start([&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions);
    });

    ASSERT_EQ(result.mRequests.size(), 1U);
    ASSERT_EQ(lossCheckAt(result, 0).mSales.size(), 1U);

    const auto& facts = lossCheckAt(result, 0).mSales[0];

    EXPECT_EQ(facts.mSaleDate,
              Date{std::chrono::sys_days{std::chrono::year{2024} / 1 / 10}.time_since_epoch()});
    EXPECT_EQ(facts.mKnownIneligibleUnits, 0);
    EXPECT_EQ(facts.mOpenUnits, 1'000'000'000); // 10 units
}

TEST(LossEligibilityIntegrationTest, SellingEveryUnitKeepsTheLoss) {
    // FURS example 14 in 2024: the purchases of 2024-02-04 and 2024-02-05 are within 30 days
    // before the sale, but the sale sells every unit held, so none of them counts.
    const TemporaryLossInputs files;
    tr::TradeRepublicParser parser;
    const std::vector<ParseResult> parsed{parser.parse(files.write(
        "history.csv",
        {
            R"(2023-09-01T09:00:00Z,2023-09-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,100,10,-1000,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2023-09-17T09:00:00Z,2023-09-17,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,100,10,-1000,,,EUR,,,,,synthetic-buy-2,,,,)",
            R"(2023-09-18T09:00:00Z,2023-09-18,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,100,10,-1000,,,EUR,,,,,synthetic-buy-3,,,,)",
            R"(2024-02-04T09:00:00Z,2024-02-04,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,100,10,-1000,,,EUR,,,,,synthetic-buy-4,,,,)",
            R"(2024-02-05T09:00:00Z,2024-02-05,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,100,10,-1000,,,EUR,,,,,synthetic-buy-5,,,,)",
            R"(2024-02-05T10:00:00Z,2024-02-05,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-500,8,4000,,,EUR,,,,,synthetic-sell-1,,,,)",
        }))};
    const auto calculation = [&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions);
    };
    DecisionSession session;

    const auto& first = session.start(calculation);

    ASSERT_EQ(first.mRequests.size(), 1U);
    ASSERT_EQ(lossCheckAt(first, 0).mSales.size(), 1U);
    EXPECT_EQ(lossCheckAt(first, 0).mSales[0].mKnownIneligibleUnits, 0);
    EXPECT_EQ(lossCheckAt(first, 0).mSales[0].mOpenUnits, 50'000'000'000); // 500 units

    ASSERT_TRUE(
        session.answer({{"request-0001", kFirstRunRevision, NoOtherActivityAnswer{}}}).empty());

    const auto& xml = session.result().report(kCapital).mXml.value();

    EXPECT_TRUE(contains(xml, "<F10>true</F10>"));

    const auto fifoLists = fifoOverKdvpRows(xml);

    ASSERT_EQ(fifoLists.size(), 1U);
    // 500 x 8 - 500 x 10 = -1000.
    EXPECT_EQ(fifoLists[0].mSaleGains, (std::vector<std::string>{"-1000"}));
}

TEST(LossEligibilityIntegrationTest, CompetingLossesTakeTheReplacementInOrder) {
    // FURS example 11 in 2024: the 2024-03-20 purchase of 100 units blocks the earlier loss in
    // full, so its row has F10 false without a question. The later loss finds nothing left.
    const TemporaryLossInputs files;
    tr::TradeRepublicParser parser;
    const std::vector<ParseResult> parsed{parser.parse(files.write(
        "history.csv",
        {
            R"(2024-01-02T09:00:00Z,2024-01-02,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,200,10,-2000,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2024-03-10T09:00:00Z,2024-03-10,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-100,8,800,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2024-03-15T09:00:00Z,2024-03-15,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-100,8,800,,,EUR,,,,,synthetic-sell-2,,,,)",
            R"(2024-03-20T09:00:00Z,2024-03-20,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,100,8,-800,,,EUR,,,,,synthetic-buy-2,,,,)",
        }))};
    const auto calculation = [&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2024, aDecisions);
    };
    DecisionSession session;

    const auto& first = session.start(calculation);

    // Only the later sale is asked about.
    ASSERT_EQ(first.mRequests.size(), 1U);
    ASSERT_EQ(lossCheckAt(first, 0).mSales.size(), 1U);
    EXPECT_EQ(lossCheckAt(first, 0).mSales[0].mSaleDate,
              Date{std::chrono::sys_days{std::chrono::year{2024} / 3 / 15}.time_since_epoch()});
    EXPECT_EQ(lossCheckAt(first, 0).mSales[0].mOpenUnits, 10'000'000'000); // 100 units

    ASSERT_TRUE(
        session.answer({{"request-0001", kFirstRunRevision, NoOtherActivityAnswer{}}}).empty());

    const auto& xml = session.result().report(kCapital).mXml.value();

    EXPECT_TRUE(contains(xml, R"xml(              <F6>2024-03-10</F6>
              <F7>100.0000</F7>
              <F9>8.0000</F9>
              <F10>false</F10>
)xml"));
    EXPECT_TRUE(contains(xml, R"xml(              <F6>2024-03-15</F6>
              <F7>100.0000</F7>
              <F9>8.0000</F9>
              <F10>true</F10>
)xml"));
    expectValidForKdvpSchemas(xml);

    const auto fifoLists = fifoOverKdvpRows(xml);

    ASSERT_EQ(fifoLists.size(), 1U);
    // F8: 200, 200 - 100 = 100, 100 - 100 = 0. Each sale: 100 x 8 - 100 x 10 = -200.
    EXPECT_EQ(fifoLists[0].mWrittenF8, (std::vector<std::string>{"200", "100", "0"}));
    EXPECT_EQ(fifoLists[0].mSaleGains, (std::vector<std::string>{"-200", "-200"}));
}

TEST(LossEligibilityIntegrationTest, ProfitableAndLongHeldUnitsAreNotChecked) {
    // Two sales on 2025-06-02, each of 2 units of 2005 (past 15 years, tax free) and 1 unit of
    // 2020, with 1 unit bought again on 2025-06-10. XX0000000001: the 2020 unit is a gain
    // (8 - 5), so nothing is checked. XX0000000002: the 2020 unit is a loss (8 - 10), and the
    // June purchase blocks it.
    const TemporaryLossInputs files;
    tr::TradeRepublicParser parser;
    const std::vector<ParseResult> parsed{parser.parse(files.write(
        "history.csv",
        {
            R"(2005-04-01T09:00:00Z,2005-04-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,10,-20,,,EUR,,,,,synthetic-buy-1,,,,)",
            R"(2005-04-01T10:00:00Z,2005-04-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Fund,XX0000000002,2,5,-10,,,EUR,,,,,synthetic-buy-2,,,,)",
            R"(2020-04-01T09:00:00Z,2020-04-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,2,5,-10,,,EUR,,,,,synthetic-buy-3,,,,)",
            R"(2020-04-01T10:00:00Z,2020-04-01,DEFAULT,TRADING,BUY,STOCK,Synthetic Fund,XX0000000002,2,10,-20,,,EUR,,,,,synthetic-buy-4,,,,)",
            R"(2025-06-02T09:00:00Z,2025-06-02,DEFAULT,TRADING,SELL,STOCK,Synthetic Share,XX0000000001,-3,8,24,,,EUR,,,,,synthetic-sell-1,,,,)",
            R"(2025-06-02T10:00:00Z,2025-06-02,DEFAULT,TRADING,SELL,STOCK,Synthetic Fund,XX0000000002,-3,8,24,,,EUR,,,,,synthetic-sell-2,,,,)",
            R"(2025-06-10T09:00:00Z,2025-06-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Share,XX0000000001,1,8,-8,,,EUR,,,,,synthetic-buy-5,,,,)",
            R"(2025-06-10T10:00:00Z,2025-06-10,DEFAULT,TRADING,BUY,STOCK,Synthetic Fund,XX0000000002,1,8,-8,,,EUR,,,,,synthetic-buy-6,,,,)",
        }))};
    DecisionSession session;

    const auto& result = session.start([&parsed](RunDecisions& aDecisions) {
        return calculateCapitalGains(parsed, 2025, aDecisions);
    });

    EXPECT_TRUE(result.mRequests.empty());
    EXPECT_EQ(result.report(kCapital).mStatus, ReportStatus::Generated);

    const auto& xml = result.report(kCapital).mXml.value();
    const auto fifoLists = fifoOverKdvpRows(xml);

    // Each list has the 2020 purchase row of 1 unit and the sale row of 1 unit.
    ASSERT_EQ(fifoLists.size(), 2U);
    EXPECT_EQ(fifoLists[0].mIsin, "XX0000000001");
    EXPECT_EQ(fifoLists[0].mSaleGains, (std::vector<std::string>{"3"})); // 8 - 5
    EXPECT_EQ(fifoLists[1].mIsin, "XX0000000002");
    EXPECT_EQ(fifoLists[1].mSaleGains, (std::vector<std::string>{"-2"})); // 8 - 10

    // The first sale row of the document is the share's (F10 true), the second the fund's.
    const auto shareRow = xml.find("<F10>");
    const auto fundRow = xml.find("<F10>", shareRow + 1);

    ASSERT_NE(fundRow, std::string::npos);
    EXPECT_EQ(xml.substr(shareRow, 15), "<F10>true</F10>");
    EXPECT_EQ(xml.substr(fundRow, 16), "<F10>false</F10>");
    expectValidForKdvpSchemas(xml);
}

} // namespace
