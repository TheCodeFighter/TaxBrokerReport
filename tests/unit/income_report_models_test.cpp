#include "taxbroker/income_report_models.hpp"
#include "utils/date_utils.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string_view>
#include <variant>

namespace {
using namespace taxbroker;

Date day(std::string_view aDate) {
    return *parseCalendarDate(aDate);
}

// The Doh-Obr model cannot hold an unknown subtype.
static_assert(std::variant_size_v<decltype(ObrInterestRow::mKind)> == 2);
} // namespace

TEST(IncomeReportModelsTest, ObrModelHoldsCashInterestAndBondCouponInOneReport) {
    // The two rows of the synthetic Doh-Obr example in tax_rules.md.
    OtherInterestReportObr report{
        .mRows = {
            ObrInterestRow{
                .mPayment = {StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                .mKind = BrokerCashInterest{},
                .mDate = day("2025-03-31"),
                .mPayer = {.mName = "Synthetic Broker AG", .mCountry = "DE"},
                .mGrossInterest = 123'400, // EUR 12.34 at 4 decimals
                .mWithholding = ForeignWithholding{.mForeignTax = 0, .mBlankInExport = true},
                .mSourceCountry = "DE",
                .mTreatyClaim = std::nullopt,
            },
            ObrInterestRow{
                .mPayment = {StatementEventKind::Interest, std::size_t{1}, std::size_t{0}},
                .mKind = BondCoupon{.mIsin = "XX0000000001"},
                .mDate = day("2025-06-15"),
                .mPayer = {.mName = "Synthetic Issuer SA", .mCountry = "FR"},
                .mGrossInterest = 250'000, // EUR 25.00 at 4 decimals
                .mWithholding = ForeignWithholding{.mForeignTax = 37'500, .mBlankInExport = false},
                .mSourceCountry = "FR",
                .mTreatyClaim = std::nullopt,
            },
        }};

    ASSERT_EQ(report.mRows.size(), 2U);

    EXPECT_TRUE(std::holds_alternative<BrokerCashInterest>(report.mRows[0].mKind));
    ASSERT_TRUE(std::holds_alternative<BondCoupon>(report.mRows[1].mKind));
    EXPECT_EQ(std::get<BondCoupon>(report.mRows[1].mKind).mIsin, "XX0000000001");

    OtherInterestReportObr changed = report;
    changed.mRows[1].mKind = BrokerCashInterest{};
    EXPECT_FALSE(changed == report);

    // The same subtype with another coupon ISIN is a different report too.
    OtherInterestReportObr otherIsin = report;
    otherIsin.mRows[1].mKind = BondCoupon{.mIsin = "XX0000000002"};
    EXPECT_FALSE(otherIsin == report);
}

TEST(IncomeReportModelsTest, DivRowWithoutPayerFactsKeepsAmountAndIsin) {
    // The payer is not in the reference files: every payer fact and the source country are empty.
    const DivDividendRow row{
        .mPayment = {StatementEventKind::Dividend, std::size_t{0}, std::size_t{0}},
        .mIsin = "XX0000000001",
        .mDate = day("2025-05-10"),
        .mPayer = {},
        .mKind = DividendKind::Ordinary,
        .mGrossIncome = 500'000, // EUR 50.00 at 4 decimals
        .mWithholding = ForeignWithholding{.mForeignTax = 75'000, .mBlankInExport = false},
        .mSourceCountry = std::nullopt,
        .mTreatyClaim = std::nullopt,
    };

    EXPECT_EQ(row.mIsin, "XX0000000001");
    EXPECT_EQ(row.mGrossIncome, 500'000);
    EXPECT_EQ(row.mWithholding.mForeignTax, 75'000); // EUR 7.50, 15%

    EXPECT_FALSE(row.mPayer.mCatalogueRow.has_value());
    EXPECT_FALSE(row.mPayer.mName.has_value());
    EXPECT_FALSE(row.mPayer.mAddress.has_value());
    EXPECT_FALSE(row.mPayer.mCountry.has_value());
    EXPECT_FALSE(row.mPayer.mTaxNumber.has_value());
    EXPECT_FALSE(row.mPayer.mIdentificationNumber.has_value());
    EXPECT_FALSE(row.mSourceCountry.has_value());
}

TEST(IncomeReportModelsTest, DivRowWithFoundPayerKeepsItsCatalogueRow) {
    const DivDividendRow notFound{
        .mPayment = {StatementEventKind::Dividend, std::size_t{0}, std::size_t{0}},
        .mIsin = "XX0000000001",
        .mDate = day("2025-05-10"),
        .mPayer = {},
        .mKind = DividendKind::Ordinary,
        .mGrossIncome = 500'000,
        .mWithholding = ForeignWithholding{.mForeignTax = 75'000, .mBlankInExport = false},
        .mSourceCountry = std::nullopt,
        .mTreatyClaim = std::nullopt,
    };
    const DivDividendRow found{
        .mPayment = {StatementEventKind::Dividend, std::size_t{0}, std::size_t{0}},
        .mIsin = "XX0000000001",
        .mDate = day("2025-05-10"),
        .mPayer = {.mName = "Synthetic Corp",
                   .mAddress = "1 Synthetic Street, Synthetic City",
                   .mCountry = "US",
                   .mCatalogueRow = CatalogueSource{.mSource = "ib-edavki",
                                                    .mSourceRevision = "synthetic-revision",
                                                    .mFile = "payers.csv",
                                                    .mRowNumber = 2,
                                                    .mOrigin = CatalogueOrigin::BuiltIn}},
        .mKind = DividendKind::Ordinary,
        .mGrossIncome = 500'000,
        .mWithholding = ForeignWithholding{.mForeignTax = 75'000, .mBlankInExport = false},
        .mSourceCountry = "US",
        .mTreatyClaim = std::nullopt,
    };

    ASSERT_TRUE(found.mPayer.mCatalogueRow.has_value());
    EXPECT_EQ(found.mPayer.mCatalogueRow->mFile, "payers.csv");
    EXPECT_EQ(found.mPayer.mCatalogueRow->mRowNumber, 2U);
    EXPECT_EQ(found.mPayer.mCatalogueRow->mOrigin, CatalogueOrigin::BuiltIn);

    EXPECT_FALSE(found == notFound);
}

TEST(IncomeReportModelsTest, DivRowKeepsBlankTaxApartFromExplicitZero) {
    const DivDividendRow blank{
        .mPayment = {StatementEventKind::Dividend, std::size_t{0}, std::size_t{0}},
        .mIsin = "XX0000000001",
        .mDate = day("2025-05-10"),
        .mPayer = {},
        .mKind = DividendKind::Ordinary,
        .mGrossIncome = 500'000,
        .mWithholding = ForeignWithholding{.mForeignTax = 0, .mBlankInExport = true},
        .mSourceCountry = std::nullopt,
        .mTreatyClaim = std::nullopt,
    };
    const DivDividendRow explicitZero{
        .mPayment = {StatementEventKind::Dividend, std::size_t{0}, std::size_t{0}},
        .mIsin = "XX0000000001",
        .mDate = day("2025-05-10"),
        .mPayer = {},
        .mKind = DividendKind::Ordinary,
        .mGrossIncome = 500'000,
        .mWithholding = ForeignWithholding{.mForeignTax = 0, .mBlankInExport = false},
        .mSourceCountry = std::nullopt,
        .mTreatyClaim = std::nullopt,
    };

    // Both mean no foreign tax paid, but the row remembers that the export had no tax.
    EXPECT_EQ(blank.mWithholding.mForeignTax, 0);
    EXPECT_EQ(explicitZero.mWithholding.mForeignTax, 0);
    EXPECT_TRUE(blank.mWithholding.mBlankInExport);
    EXPECT_FALSE(explicitZero.mWithholding.mBlankInExport);

    EXPECT_FALSE(blank == explicitZero);
}

TEST(IncomeReportModelsTest, DivRowKeepsConfirmedTreatyClaim) {
    const DivDividendRow withoutClaim{
        .mPayment = {StatementEventKind::Dividend, std::size_t{0}, std::size_t{0}},
        .mIsin = "XX0000000001",
        .mDate = day("2025-05-10"),
        .mPayer = {},
        .mKind = DividendKind::Ordinary,
        .mGrossIncome = 500'000,
        .mWithholding = ForeignWithholding{.mForeignTax = 75'000, .mBlankInExport = false},
        .mSourceCountry = "US",
        .mTreatyClaim = std::nullopt,
    };
    const DivDividendRow withClaim{
        .mPayment = {StatementEventKind::Dividend, std::size_t{0}, std::size_t{0}},
        .mIsin = "XX0000000001",
        .mDate = day("2025-05-10"),
        .mPayer = {},
        .mKind = DividendKind::Ordinary,
        .mGrossIncome = 500'000,
        .mWithholding = ForeignWithholding{.mForeignTax = 75'000, .mBlankInExport = false},
        .mSourceCountry = "US",
        .mTreatyClaim =
            TreatyClaim{.mStatement = "Synthetic treaty, article 10",
                        .mCatalogueRow = CatalogueSource{.mSource = "ib-edavki",
                                                         .mSourceRevision = "synthetic-revision",
                                                         .mFile = "relief_statements.csv",
                                                         .mRowNumber = 2,
                                                         .mOrigin = CatalogueOrigin::BuiltIn}},
    };

    // The same claim, written out again: equal to `withClaim`.
    const TreatyClaim sameClaim{.mStatement = "Synthetic treaty, article 10",
                                .mCatalogueRow =
                                    CatalogueSource{.mSource = "ib-edavki",
                                                    .mSourceRevision = "synthetic-revision",
                                                    .mFile = "relief_statements.csv",
                                                    .mRowNumber = 2,
                                                    .mOrigin = CatalogueOrigin::BuiltIn}};

    ASSERT_TRUE(withClaim.mTreatyClaim.has_value());
    EXPECT_TRUE(withClaim.mTreatyClaim == sameClaim);
    EXPECT_EQ(withClaim.mTreatyClaim->mStatement, "Synthetic treaty, article 10");
    ASSERT_TRUE(withClaim.mTreatyClaim->mCatalogueRow.has_value());
    EXPECT_EQ(withClaim.mTreatyClaim->mCatalogueRow->mFile, "relief_statements.csv");
    EXPECT_FALSE(withoutClaim.mTreatyClaim.has_value());

    EXPECT_FALSE(withClaim == withoutClaim);
}

TEST(IncomeReportModelsTest, DhoRowSumsTwoImportedPayments) {
    // Imported interest only: EUR 600 + EUR 500 = EUR 1,100, above the EUR 1,000 limit.
    const DepositInterestReportDHO report{
        .mRows = {DhoInterestRow{
            .mPayments = {{StatementEventKind::Interest, std::size_t{0}, std::size_t{0}},
                          {StatementEventKind::Interest, std::size_t{0}, std::size_t{1}}},
            .mPayer = {.mName = "Synthetic Bank d.d.", .mCountry = "SI"},
            .mKind = DepositInterestKind::BankDeposit,
            .mInterest = 11'000'000, // 6'000'000 + 5'000'000 = EUR 1,100 at 4 decimals
            .mForeignTax = 0,
            .mSourceCountry = "SI",
        }}};

    ASSERT_EQ(report.mRows.size(), 1U);

    const DhoInterestRow& row = report.mRows[0];
    ASSERT_EQ(row.mPayments.size(), 2U);
    EXPECT_EQ(row.mPayments[0].mEventIndex, 0U);
    EXPECT_EQ(row.mPayments[1].mEventIndex, 1U);
    EXPECT_EQ(row.mInterest, 11'000'000);
    EXPECT_EQ(row.mForeignTax, 0);
}
