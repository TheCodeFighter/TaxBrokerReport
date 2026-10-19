#include "../support/statement_merge_assertions.hpp"
#include "parsers/traderepublic_parser.hpp"
#include "utils/date_utils.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {
using namespace taxbroker;

std::filesystem::path fixturePath(std::string_view aFilename) {
    return std::filesystem::path{__FILE__}.parent_path().parent_path() / "test_data" / "csv" /
           "merger" / aFilename;
}

// Tests supply synthetic CSV fields directly; unspecified fields are empty.
struct IncomeCsvRow {
    std::string_view mDatetime;
    std::string_view mDate;
    std::string_view mAccountType;
    std::string_view mCategory;
    std::string_view mType;
    std::string_view mAssetClass;
    std::string_view mName;
    std::string_view mSymbol;
    std::string_view mShares;
    std::string_view mPrice;
    std::string_view mAmount;
    std::string_view mFee;
    std::string_view mTax;
    std::string_view mCurrency;
    std::string_view mOriginalAmount;
    std::string_view mOriginalCurrency;
    std::string_view mFxRate;
    std::string_view mDescription;
    std::string_view mTransactionId;
    std::string_view mCounterpartyName;
    std::string_view mCounterpartyIban;
    std::string_view mPaymentReference;
    std::string_view mMccCode;
};

class TemporaryIncomeInputs {
  public:
    TemporaryIncomeInputs() {
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
        mDirectory = std::filesystem::temp_directory_path() /
                     ("taxbroker_income_synthetic_" + std::to_string(token));

        if (!std::filesystem::create_directory(mDirectory))
        {
            throw std::runtime_error{"Unable to create synthetic income directory"};
        }
    }

    TemporaryIncomeInputs(const TemporaryIncomeInputs&) = delete;
    TemporaryIncomeInputs& operator=(const TemporaryIncomeInputs&) = delete;

    ~TemporaryIncomeInputs() {
        std::error_code error;
        std::filesystem::remove_all(mDirectory, error);
    }

    std::filesystem::path write(std::string_view aName,
                                std::initializer_list<IncomeCsvRow> aRows) const {
        const auto path = mDirectory / aName;
        std::ofstream output{path};

        output << "datetime,date,account_type,category,type,asset_class,name,symbol,shares,"
                  "price,amount,fee,tax,currency,original_amount,original_currency,fx_rate,"
                  "description,transaction_id,counterparty_name,counterparty_iban,"
                  "payment_reference,mcc_code\n";

        for (const auto& aRow : aRows)
        {
            const std::array fields{aRow.mDatetime,
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
                                    aRow.mMccCode};

            for (std::size_t index = 0; index < fields.size(); ++index)
            {
                if (index != 0)
                {
                    output << ',';
                }

                output << '"';

                for (const char character : fields[index])
                {
                    if (character == '"')
                    {
                        output << '"';
                    }

                    output << character;
                }

                output << '"';
            }

            output << '\n';
        }

        output.close();

        if (!output)
        {
            throw std::runtime_error{"Unable to write synthetic income input"};
        }

        return path;
    }

  private:
    std::filesystem::path mDirectory;
};

// Read synthetic history.csv once, then copy selected rows into temporary CSV inputs.
class TemporaryHistory {
  public:
    TemporaryHistory() {
        std::ifstream input{fixturePath("history.csv")};

        if (!input || !std::getline(input, mHeader))
        {
            throw std::runtime_error{"Unable to read synthetic merger fixture"};
        }

        std::string row;

        while (std::getline(input, row))
        {
            mRows.push_back(row);
        }

        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();

        for (unsigned attempt = 0; attempt < 100; ++attempt)
        {
            const auto candidate =
                std::filesystem::temp_directory_path() /
                ("taxbroker_merger_" + std::to_string(token) + "_" + std::to_string(attempt));

            if (std::filesystem::create_directory(candidate))
            {
                mDirectory = candidate;

                return;
            }
        }

        throw std::runtime_error{"Unable to create synthetic merger directory"};
    }

    TemporaryHistory(const TemporaryHistory&) = delete;
    TemporaryHistory& operator=(const TemporaryHistory&) = delete;

    ~TemporaryHistory() {
        std::error_code error;
        std::filesystem::remove_all(mDirectory, error);
    }

    // Row indices start at zero after the header. An optional timestamp replaces only datetime.
    std::filesystem::path
    writeRows(std::string_view aFilename,
              std::initializer_list<std::size_t> aRows,
              std::optional<std::string_view> aTimestamp = std::nullopt) const {
        const auto path = mDirectory / aFilename;
        std::ofstream output{path};

        output << mHeader << '\n';

        for (const auto index : aRows)
        {
            const auto& row = mRows.at(index);

            if (aTimestamp)
            {
                output << '"' << *aTimestamp << '"' << row.substr(row.find(',')) << '\n';
            }
            else
            {
                output << row << '\n';
            }
        }

        if (!output)
        {
            throw std::runtime_error{"Unable to write synthetic merger input"};
        }

        return path;
    }

  private:
    std::filesystem::path mDirectory;
    std::string mHeader;
    std::vector<std::string> mRows;
};

// aFiles fixes file request indices; aOrder simulates parser-result arrival order.
// Each returned input holds that file's parsed statement and parser diagnostics.
template <std::size_t SourceCount>
std::vector<StatementMergeInput>
parseInOrder(const std::array<std::filesystem::path, SourceCount>& aFiles,
             const std::array<std::size_t, SourceCount>& aOrder) {
    tr::TradeRepublicParser parser;
    std::vector<StatementMergeInput> inputs(aOrder.size());
    const auto parseInput = [&parser, &aFiles](std::size_t aSourceIndex) {
        return StatementMergeInput{.mSourceIndex = aSourceIndex,
                                   .mParseResult =
                                       parser.parse(aFiles[aSourceIndex], aSourceIndex)};
    };

    std::transform(aOrder.begin(), aOrder.end(), inputs.begin(), parseInput);

    return inputs;
}

std::vector<std::string> ledgerIds(const StatementMergeResult& aResult) {
    std::vector<std::string> ids;

    for (const auto& reference : aResult.mStatement.mChronologicalOrder)
    {
        const bool standalone = reference.mKind == StatementEventKind::Benefit ||
                                reference.mKind == StatementEventKind::PrivateMarket;

        EXPECT_EQ(reference.mInstrumentIndex.has_value(), !standalone);
        EXPECT_EQ(std::count(aResult.mStatement.mChronologicalOrder.begin(),
                             aResult.mStatement.mChronologicalOrder.end(),
                             reference),
                  1);

        const auto& metadata = test::referencedMetadata(aResult.mStatement, reference);

        EXPECT_FALSE(metadata.mSources.empty());
        ids.push_back(primarySource(metadata).mTransactionId.value_or(""));
    }

    return ids;
}

SourceReference expectedSource(std::size_t aSource, std::size_t aEvent, std::string_view aId) {
    return SourceReference{.mBroker = Broker::TradeRepublic,
                           .mFilename =
                               SourceFilename::fromPath("part-" + std::to_string(aSource) + ".csv"),
                           .mSourceRow = aEvent + 2,
                           .mTransactionId = std::string{aId},
                           .mInputSequence = {aSource, aEvent}};
}

TEST(StatementMergerIntegrationTest,
     DisjointFilesMatchWholeHistoryWithoutChangingTaxDatesOrValues) {
    // Getting data from history.csv
    const TemporaryHistory history;

    // These three files together contain every data row from history.csv, with no overlap.
    const std::array files{history.writeRows("part-0.csv", {0, 1, 2, 3}),
                           history.writeRows("part-1.csv", {4, 5, 6}),
                           history.writeRows("part-2.csv", {7, 8, 9, 10, 11})};

    // Parse and merge the unsplit history as a baseline for the three-file result.
    const StatementMergeInput whole{
        .mSourceIndex = 0,
        .mParseResult = tr::TradeRepublicParser{}.parse(fixturePath("history.csv"), 0)};

    const auto expected = DeterministicStatementMerger{}.merge(std::span{&whole, 1U});

    const std::vector<std::string> ids{"synthetic-older",
                                       "synthetic-dividend",
                                       "synthetic-interest",
                                       "synthetic-tied",
                                       "synthetic-benefit",
                                       "synthetic-action",
                                       "synthetic-private",
                                       "",
                                       "synthetic-future"};

    // For each chronological ID above: file request index and data-row index within that file.
    const std::array<std::pair<std::size_t, std::size_t>, 9> sourcePositions{
        {{0, 1}, {1, 0}, {1, 1}, {0, 2}, {2, 0}, {1, 2}, {2, 1}, {2, 2}, {0, 0}}};

    std::array<std::size_t, 3> order{0, 1, 2};

    ASSERT_EQ(whole.mParseResult.mDiagnostics.size(), 4U);
    EXPECT_EQ(ledgerIds(expected), ids);

    do
    {
        // Parse the temporary history parts in this arrival order, keeping their file indices.
        const auto inputs = parseInOrder(files, order);
        // Combine parsed records into presentation storage, chronological references
        // and diagnostics.
        const auto result = DeterministicStatementMerger{}.merge(inputs);

        test::expectStatementsEqual(result.mStatement.mPresentation,
                                    expected.mStatement.mPresentation,
                                    false);
        EXPECT_EQ(result.mStatement.mChronologicalOrder, expected.mStatement.mChronologicalOrder);
        EXPECT_EQ(ledgerIds(result), ids);
        ASSERT_EQ(result.mDiagnostics.size(), 4U);
        ASSERT_TRUE(std::holds_alternative<SourcedParseDiagnostic>(result.mDiagnostics[0]));

        const auto& futureDateWarning = std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]);

        EXPECT_EQ(futureDateWarning.mSourceIndex, 0U);
        EXPECT_EQ(futureDateWarning.mDiagnostic.mSeverity, DiagnosticSeverity::Warning);
        EXPECT_EQ(futureDateWarning.mDiagnostic.mCode, DiagnosticCode::InconsistentValue);
        EXPECT_EQ(futureDateWarning.mDiagnostic.mField, "datetime");
        EXPECT_EQ(futureDateWarning.mDiagnostic.mSourceFile, "part-0.csv");
        EXPECT_EQ(futureDateWarning.mDiagnostic.mRowIndex, 2U);
        EXPECT_EQ(futureDateWarning.mDiagnostic.mTransactionId, "synthetic-future");

        ASSERT_TRUE(std::holds_alternative<SourcedParseDiagnostic>(result.mDiagnostics[1]));

        const auto& olderDateWarning = std::get<SourcedParseDiagnostic>(result.mDiagnostics[1]);

        EXPECT_EQ(olderDateWarning.mSourceIndex, 0U);
        EXPECT_EQ(olderDateWarning.mDiagnostic.mSeverity, DiagnosticSeverity::Warning);
        EXPECT_EQ(olderDateWarning.mDiagnostic.mCode, DiagnosticCode::InconsistentValue);
        EXPECT_EQ(olderDateWarning.mDiagnostic.mField, "datetime");
        EXPECT_EQ(olderDateWarning.mDiagnostic.mSourceFile, "part-0.csv");
        EXPECT_EQ(olderDateWarning.mDiagnostic.mRowIndex, 3U);
        EXPECT_EQ(olderDateWarning.mDiagnostic.mTransactionId, "synthetic-older");

        ASSERT_TRUE(std::holds_alternative<SourcedParseDiagnostic>(result.mDiagnostics[2]));
        const auto& missingDividendTimeError =
            std::get<SourcedParseDiagnostic>(result.mDiagnostics[2]);

        EXPECT_EQ(missingDividendTimeError.mSourceIndex, 2U);
        EXPECT_EQ(missingDividendTimeError.mDiagnostic.mSeverity, DiagnosticSeverity::Error);
        EXPECT_EQ(missingDividendTimeError.mDiagnostic.mCode, DiagnosticCode::MissingField);
        EXPECT_EQ(missingDividendTimeError.mDiagnostic.mField, "datetime");
        EXPECT_EQ(missingDividendTimeError.mDiagnostic.mSourceFile, "part-2.csv");
        EXPECT_EQ(missingDividendTimeError.mDiagnostic.mRowIndex, 5U);
        EXPECT_EQ(missingDividendTimeError.mDiagnostic.mTransactionId,
                  "synthetic-missing-dividend-time");

        ASSERT_TRUE(std::holds_alternative<SourcedParseDiagnostic>(result.mDiagnostics[3]));
        const auto& missingPrivateTimeError =
            std::get<SourcedParseDiagnostic>(result.mDiagnostics[3]);

        EXPECT_EQ(missingPrivateTimeError.mSourceIndex, 2U);
        EXPECT_EQ(missingPrivateTimeError.mDiagnostic.mSeverity, DiagnosticSeverity::Error);
        EXPECT_EQ(missingPrivateTimeError.mDiagnostic.mCode, DiagnosticCode::MissingField);
        EXPECT_EQ(missingPrivateTimeError.mDiagnostic.mField, "datetime");
        EXPECT_EQ(missingPrivateTimeError.mDiagnostic.mSourceFile, "part-2.csv");
        EXPECT_EQ(missingPrivateTimeError.mDiagnostic.mRowIndex, 6U);
        EXPECT_EQ(missingPrivateTimeError.mDiagnostic.mTransactionId,
                  "synthetic-missing-private-time");

        ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), sourcePositions.size());

        for (std::size_t event = 0; event < sourcePositions.size(); ++event)
        {
            const auto [source, position] = sourcePositions[event];
            auto provenance = expectedSource(source, position, ids[event]);

            if (ids[event].empty())
            {
                provenance.mTransactionId.reset();
            }

            EXPECT_EQ(test::referencedMetadata(result.mStatement,
                                               result.mStatement.mChronologicalOrder[event])
                          .mSources,
                      std::vector{provenance});
        }

        const auto& trades = result.mStatement.mPresentation.mTradeInstruments.at(0).mTransactions;

        ASSERT_EQ(trades.size(), 4U);
        EXPECT_EQ(trades[0].mMetadata.mTaxDate, *parseCalendarDate("2023-12-31"));
        EXPECT_EQ(trades[3].mMetadata.mTaxDate, *parseCalendarDate("2025-01-01"));
        EXPECT_EQ(trades[0].mMetadata.mSourceTimestamp,
                  parseSourceTimestamp("2023-12-31T22:30:00Z"));
        EXPECT_EQ(trades[3].mMetadata.mSourceTimestamp,
                  parseSourceTimestamp("2023-01-01T08:00:00Z"));
        EXPECT_EQ(trades[1].mUnits, 125000000);
        EXPECT_EQ(trades[1].mUnitPrice, 100000);
        EXPECT_EQ(trades[1].mAmount, 125000);
        EXPECT_EQ(trades[1].mFeePaid, 2500);
        EXPECT_EQ(trades[1].mMetadata.mSources,
                  std::vector{expectedSource(0, 2, "synthetic-tied")});

        const auto& presentation = result.mStatement.mPresentation;

        EXPECT_EQ(presentation.mDividendInstruments.at(0).mTransactions.at(0).mGrossAmount, 123400);
        EXPECT_EQ(presentation.mDividendInstruments.at(0).mTransactions.at(0).mTaxPaid, 23400);
        EXPECT_EQ(presentation.mInterestInstruments.at(0).mTransactions.at(0).mGrossAmount, 12300);
        EXPECT_EQ(presentation.mBenefitEvents.at(0).mAmount, 25000);
        EXPECT_EQ(presentation.mPrivateMarketEvents.at(0).mAmount, -250000);
        EXPECT_EQ(presentation.mPrivateMarketEvents.at(0).mFeePaid, 5000);
        EXPECT_EQ(presentation.mTradeInstruments.at(0).mCorporateActions.at(0).mUnitsDelta,
                  200000000);
        EXPECT_FALSE(
            presentation.mTradeInstruments.at(0).mCorporateActions.at(0).mRatio.has_value());
    } while (std::next_permutation(order.begin(), order.end()));
}

TEST(StatementMergerIntegrationTest,
     OverlappingFilesPreserveAllSixKindsAndEverySourceButKeepIdlessRows) {
    const TemporaryHistory history;
    // Repeat history.csv rows across files: matching IDs merge, but each ID-less row survives.
    const std::array files{history.writeRows("part-0.csv", {1, 2, 3, 4, 5, 6, 7, 8, 9}),
                           history.writeRows("part-1.csv", {0, 2, 4, 5, 6, 7, 8, 9}),
                           history.writeRows("part-2.csv", {2, 4, 5, 6, 7, 8, 9})};
    std::array<std::size_t, 3> order{0, 1, 2};
    const auto expected = DeterministicStatementMerger{}.merge(parseInOrder(files, order));
    const std::array ids{"synthetic-tied",
                         "synthetic-dividend",
                         "synthetic-interest",
                         "synthetic-action",
                         "synthetic-benefit",
                         "synthetic-private"};
    const std::array<std::size_t, 6> firstEventIndices{1, 3, 4, 5, 6, 7};
    const std::vector<std::string> expectedIds{"synthetic-older",
                                               "synthetic-dividend",
                                               "synthetic-interest",
                                               "synthetic-tied",
                                               "synthetic-benefit",
                                               "synthetic-action",
                                               "synthetic-private",
                                               "",
                                               "",
                                               "",
                                               "synthetic-future"};

    do
    {
        const auto inputs = parseInOrder(files, order);
        const auto result = DeterministicStatementMerger{}.merge(inputs);

        test::expectMergeResultsEqual(result, expected);

        const auto& presentation = result.mStatement.mPresentation;

        ASSERT_EQ(presentation.mTradeInstruments.size(), 1U);

        const auto& trades = presentation.mTradeInstruments.front().mTransactions;

        ASSERT_EQ(trades.size(), 6U);

        EXPECT_EQ(trades[0].mTradeSide, TradeSide::Buy);
        EXPECT_EQ(trades[0].mUnits, 200'000'000);
        EXPECT_EQ(trades[0].mUnitPrice, 90'000);
        EXPECT_EQ(trades[0].mAmount, 180'000);
        EXPECT_EQ(trades[0].mFeePaid, 0);
        EXPECT_EQ(trades[0].mCurrency, Currency::EUR);
        EXPECT_EQ(trades[0].mExchangeRate, 100'000'000);
        EXPECT_EQ(trades[0].mMetadata.mTaxDate, parseCalendarDate("2023-12-31"));

        EXPECT_EQ(trades[1].mTradeSide, TradeSide::Buy);
        EXPECT_EQ(trades[1].mUnits, 125'000'000);
        EXPECT_EQ(trades[1].mUnitPrice, 100'000);
        EXPECT_EQ(trades[1].mAmount, 125'000);
        EXPECT_EQ(trades[1].mFeePaid, 2'500);
        EXPECT_EQ(trades[1].mCurrency, Currency::EUR);
        EXPECT_EQ(trades[1].mExchangeRate, 100'000'000);
        EXPECT_EQ(trades[1].mMetadata.mTaxDate, parseCalendarDate("2024-01-02"));

        EXPECT_EQ(trades[2].mTradeSide, TradeSide::Buy);
        EXPECT_EQ(trades[2].mUnits, 12'500'000);
        EXPECT_EQ(trades[2].mUnitPrice, 80'000);
        EXPECT_EQ(trades[2].mAmount, 10'000);
        EXPECT_EQ(trades[2].mFeePaid, 0);
        EXPECT_EQ(trades[2].mCurrency, Currency::EUR);
        EXPECT_EQ(trades[2].mExchangeRate, 100'000'000);
        EXPECT_EQ(trades[2].mMetadata.mTaxDate, parseCalendarDate("2024-01-03"));

        EXPECT_EQ(trades[3].mTradeSide, TradeSide::Buy);
        EXPECT_EQ(trades[3].mUnits, 12'500'000);
        EXPECT_EQ(trades[3].mUnitPrice, 80'000);
        EXPECT_EQ(trades[3].mAmount, 10'000);
        EXPECT_EQ(trades[3].mFeePaid, 0);
        EXPECT_EQ(trades[3].mCurrency, Currency::EUR);
        EXPECT_EQ(trades[3].mExchangeRate, 100'000'000);
        EXPECT_EQ(trades[3].mMetadata.mTaxDate, parseCalendarDate("2024-01-03"));

        EXPECT_EQ(trades[4].mTradeSide, TradeSide::Buy);
        EXPECT_EQ(trades[4].mUnits, 12'500'000);
        EXPECT_EQ(trades[4].mUnitPrice, 80'000);
        EXPECT_EQ(trades[4].mAmount, 10'000);
        EXPECT_EQ(trades[4].mFeePaid, 0);
        EXPECT_EQ(trades[4].mCurrency, Currency::EUR);
        EXPECT_EQ(trades[4].mExchangeRate, 100'000'000);
        EXPECT_EQ(trades[4].mMetadata.mTaxDate, parseCalendarDate("2024-01-03"));

        EXPECT_EQ(trades[5].mTradeSide, TradeSide::Buy);
        EXPECT_EQ(trades[5].mUnits, 100'000'000);
        EXPECT_EQ(trades[5].mUnitPrice, 100'000);
        EXPECT_EQ(trades[5].mAmount, 100'000);
        EXPECT_EQ(trades[5].mFeePaid, 0);
        EXPECT_EQ(trades[5].mCurrency, Currency::EUR);
        EXPECT_EQ(trades[5].mExchangeRate, 100'000'000);
        EXPECT_EQ(trades[5].mMetadata.mTaxDate, parseCalendarDate("2025-01-01"));

        ASSERT_EQ(presentation.mDividendInstruments.size(), 1U);
        ASSERT_EQ(presentation.mDividendInstruments.front().mTransactions.size(), 1U);

        const auto& dividend = presentation.mDividendInstruments.front().mTransactions.front();

        EXPECT_EQ(dividend.mGrossAmount, 123'400);
        EXPECT_EQ(dividend.mTaxPaid, 23'400);
        EXPECT_EQ(dividend.mCurrency, Currency::EUR);
        EXPECT_EQ(dividend.mTaxCurrency, Currency::EUR);
        EXPECT_EQ(dividend.mExchangeRate, 100'000'000);
        ASSERT_EQ(presentation.mInterestInstruments.size(), 1U);
        ASSERT_EQ(presentation.mInterestInstruments.front().mTransactions.size(), 1U);

        const auto& interest = presentation.mInterestInstruments.front().mTransactions.front();

        EXPECT_EQ(interest.mGrossAmount, 12'300);
        EXPECT_FALSE(interest.mTaxPaid.has_value());
        EXPECT_EQ(interest.mCurrency, Currency::EUR);
        EXPECT_EQ(interest.mTaxCurrency, Currency::EUR);
        EXPECT_EQ(interest.mExchangeRate, 100'000'000);
        ASSERT_EQ(presentation.mBenefitEvents.size(), 1U);
        EXPECT_EQ(presentation.mBenefitEvents.front().mType, BenefitType::Saveback);
        EXPECT_EQ(presentation.mBenefitEvents.front().mAmount, 25'000);
        EXPECT_EQ(presentation.mBenefitEvents.front().mCurrency, Currency::EUR);
        ASSERT_EQ(presentation.mPrivateMarketEvents.size(), 1U);

        const auto& privatePurchase = presentation.mPrivateMarketEvents.front();

        EXPECT_EQ(privatePurchase.mType, PrivateMarketEventType::Buy);
        EXPECT_EQ(privatePurchase.mAmount, -250'000);
        EXPECT_EQ(privatePurchase.mFeePaid, 5'000);
        EXPECT_EQ(privatePurchase.mCurrency, Currency::EUR);
        ASSERT_EQ(presentation.mTradeInstruments.front().mCorporateActions.size(), 1U);

        const auto& action = presentation.mTradeInstruments.front().mCorporateActions.front();

        EXPECT_EQ(action.mType, CorporateActionType::UnresolvedSplit);
        EXPECT_EQ(action.mUnitsDelta, 200'000'000);
        EXPECT_FALSE(action.mRatio.has_value());

        EXPECT_EQ(ledgerIds(result), expectedIds);
        ASSERT_EQ(result.mDiagnostics.size(), 2U);
        ASSERT_TRUE(std::holds_alternative<SourcedParseDiagnostic>(result.mDiagnostics[0]));
        const auto& olderDateWarning = std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]);

        EXPECT_EQ(olderDateWarning.mSourceIndex, 0U);
        EXPECT_EQ(olderDateWarning.mDiagnostic.mSeverity, DiagnosticSeverity::Warning);
        EXPECT_EQ(olderDateWarning.mDiagnostic.mCode, DiagnosticCode::InconsistentValue);
        EXPECT_EQ(olderDateWarning.mDiagnostic.mField, "datetime");
        EXPECT_EQ(olderDateWarning.mDiagnostic.mSourceFile, "part-0.csv");
        EXPECT_EQ(olderDateWarning.mDiagnostic.mRowIndex, 2U);
        EXPECT_EQ(olderDateWarning.mDiagnostic.mTransactionId, "synthetic-older");

        ASSERT_TRUE(std::holds_alternative<SourcedParseDiagnostic>(result.mDiagnostics[1]));
        const auto& futureDateWarning = std::get<SourcedParseDiagnostic>(result.mDiagnostics[1]);

        EXPECT_EQ(futureDateWarning.mSourceIndex, 1U);
        EXPECT_EQ(futureDateWarning.mDiagnostic.mSeverity, DiagnosticSeverity::Warning);
        EXPECT_EQ(futureDateWarning.mDiagnostic.mCode, DiagnosticCode::InconsistentValue);
        EXPECT_EQ(futureDateWarning.mDiagnostic.mField, "datetime");
        EXPECT_EQ(futureDateWarning.mDiagnostic.mSourceFile, "part-1.csv");
        EXPECT_EQ(futureDateWarning.mDiagnostic.mRowIndex, 2U);
        EXPECT_EQ(futureDateWarning.mDiagnostic.mTransactionId, "synthetic-future");

        for (const auto& reference : result.mStatement.mChronologicalOrder)
        {
            const auto& metadata = test::referencedMetadata(result.mStatement, reference);
            const auto id = primarySource(metadata).mTransactionId.value_or("");
            const auto found = std::find(ids.begin(), ids.end(), id);

            if (found != ids.end())
            {
                const auto index = static_cast<std::size_t>(found - ids.begin());
                const std::vector sources{expectedSource(0, firstEventIndices[index], id),
                                          expectedSource(1, index + 1, id),
                                          expectedSource(2, index, id)};

                EXPECT_EQ(metadata.mSources, sources);
            }
            else
            {
                ASSERT_EQ(metadata.mSources.size(), 1U);

                if (id.empty())
                {
                    const auto source = metadata.mSources.front().mInputSequence.mSourceIndex;
                    const std::array<std::size_t, 3> indices{8, 7, 6};
                    auto expectedReference = expectedSource(source, indices[source], "");
                    expectedReference.mTransactionId.reset();

                    EXPECT_EQ(metadata.mSources.front(), expectedReference);
                }
            }
        }
    } while (std::next_permutation(order.begin(), order.end()));
}

TEST(StatementMergerIntegrationTest,
     ConflictsRejectEveryCopyAndKeepPartialParseAndInstrumentDiagnostics) {
    const std::array files{fixturePath("history.csv"),
                           fixturePath("conflict.csv"),
                           fixturePath("partial.csv")};

    std::array<std::size_t, 3> order{0, 1, 2};

    const auto expected = DeterministicStatementMerger{}.merge(parseInOrder(files, order));

    do
    {
        const auto inputs = parseInOrder(files, order);
        const auto result = DeterministicStatementMerger{}.merge(inputs);

        test::expectMergeResultsEqual(result, expected);

        const auto& presentation = result.mStatement.mPresentation;

        ASSERT_EQ(presentation.mTradeInstruments.size(), 1U);

        const auto& survivingTrades = presentation.mTradeInstruments.front().mTransactions;

        ASSERT_EQ(survivingTrades.size(), 4U);

        EXPECT_EQ(survivingTrades[0].mTradeSide, TradeSide::Buy);
        EXPECT_EQ(survivingTrades[0].mUnits, 200'000'000);
        EXPECT_EQ(survivingTrades[0].mUnitPrice, 90'000);
        EXPECT_EQ(survivingTrades[0].mAmount, 180'000);
        EXPECT_EQ(survivingTrades[0].mFeePaid, 0);
        EXPECT_EQ(survivingTrades[0].mCurrency, Currency::EUR);
        EXPECT_EQ(survivingTrades[0].mExchangeRate, 100'000'000);
        EXPECT_EQ(survivingTrades[0].mMetadata.mTaxDate, parseCalendarDate("2023-12-31"));

        EXPECT_EQ(survivingTrades[1].mTradeSide, TradeSide::Buy);
        EXPECT_EQ(survivingTrades[1].mUnits, 12'500'000);
        EXPECT_EQ(survivingTrades[1].mUnitPrice, 80'000);
        EXPECT_EQ(survivingTrades[1].mAmount, 10'000);
        EXPECT_EQ(survivingTrades[1].mFeePaid, 0);
        EXPECT_EQ(survivingTrades[1].mCurrency, Currency::EUR);
        EXPECT_EQ(survivingTrades[1].mExchangeRate, 100'000'000);
        EXPECT_EQ(survivingTrades[1].mMetadata.mTaxDate, parseCalendarDate("2024-01-03"));

        EXPECT_EQ(survivingTrades[2].mTradeSide, TradeSide::Buy);
        EXPECT_EQ(survivingTrades[2].mUnits, 100'000'000);
        EXPECT_EQ(survivingTrades[2].mUnitPrice, 100'000);
        EXPECT_EQ(survivingTrades[2].mAmount, 100'000);
        EXPECT_EQ(survivingTrades[2].mFeePaid, 0);
        EXPECT_EQ(survivingTrades[2].mCurrency, Currency::EUR);
        EXPECT_EQ(survivingTrades[2].mExchangeRate, 100'000'000);
        EXPECT_EQ(survivingTrades[2].mMetadata.mTaxDate, parseCalendarDate("2024-01-04"));

        EXPECT_EQ(survivingTrades[3].mTradeSide, TradeSide::Buy);
        EXPECT_EQ(survivingTrades[3].mUnits, 100'000'000);
        EXPECT_EQ(survivingTrades[3].mUnitPrice, 100'000);
        EXPECT_EQ(survivingTrades[3].mAmount, 100'000);
        EXPECT_EQ(survivingTrades[3].mFeePaid, 0);
        EXPECT_EQ(survivingTrades[3].mCurrency, Currency::EUR);
        EXPECT_EQ(survivingTrades[3].mExchangeRate, 100'000'000);
        EXPECT_EQ(survivingTrades[3].mMetadata.mTaxDate, parseCalendarDate("2025-01-01"));

        ASSERT_EQ(presentation.mDividendInstruments.size(), 1U);
        ASSERT_EQ(presentation.mDividendInstruments.front().mTransactions.size(), 1U);

        const auto& dividend = presentation.mDividendInstruments.front().mTransactions.front();

        EXPECT_EQ(dividend.mGrossAmount, 123'400);
        EXPECT_EQ(dividend.mTaxPaid, 23'400);
        EXPECT_EQ(dividend.mCurrency, Currency::EUR);
        EXPECT_EQ(dividend.mTaxCurrency, Currency::EUR);
        EXPECT_EQ(dividend.mExchangeRate, 100'000'000);
        ASSERT_EQ(presentation.mInterestInstruments.size(), 1U);
        ASSERT_EQ(presentation.mInterestInstruments.front().mTransactions.size(), 1U);

        const auto& interest = presentation.mInterestInstruments.front().mTransactions.front();

        EXPECT_EQ(interest.mGrossAmount, 12'300);
        EXPECT_FALSE(interest.mTaxPaid.has_value());
        EXPECT_EQ(interest.mCurrency, Currency::EUR);
        EXPECT_EQ(interest.mTaxCurrency, Currency::EUR);
        EXPECT_EQ(interest.mExchangeRate, 100'000'000);
        ASSERT_EQ(presentation.mBenefitEvents.size(), 1U);
        EXPECT_EQ(presentation.mBenefitEvents.front().mType, BenefitType::Saveback);
        EXPECT_EQ(presentation.mBenefitEvents.front().mAmount, 25'000);
        EXPECT_EQ(presentation.mBenefitEvents.front().mCurrency, Currency::EUR);
        ASSERT_EQ(presentation.mPrivateMarketEvents.size(), 1U);

        const auto& privatePurchase = presentation.mPrivateMarketEvents.front();

        EXPECT_EQ(privatePurchase.mType, PrivateMarketEventType::Buy);
        EXPECT_EQ(privatePurchase.mAmount, -250'000);
        EXPECT_EQ(privatePurchase.mFeePaid, 5'000);
        EXPECT_EQ(privatePurchase.mCurrency, Currency::EUR);
        ASSERT_EQ(presentation.mTradeInstruments.front().mCorporateActions.size(), 1U);

        const auto& action = presentation.mTradeInstruments.front().mCorporateActions.front();

        EXPECT_EQ(action.mType, CorporateActionType::UnresolvedSplit);
        EXPECT_EQ(action.mUnitsDelta, 200'000'000);
        EXPECT_FALSE(action.mRatio.has_value());

        EXPECT_EQ(ledgerIds(result),
                  (std::vector<std::string>{"synthetic-older",
                                            "synthetic-dividend",
                                            "synthetic-interest",
                                            "synthetic-benefit",
                                            "synthetic-action",
                                            "synthetic-private",
                                            "",
                                            "synthetic-class",
                                            "synthetic-future"}));

        const auto ids = ledgerIds(result);

        EXPECT_EQ(ids.size(), 9U);
        EXPECT_EQ(std::count(ids.begin(), ids.end(), "synthetic-tied"), 0);
        EXPECT_EQ(std::count(ids.begin(), ids.end(), "synthetic-rejected"), 0);
        EXPECT_EQ(std::count(ids.begin(), ids.end(), "synthetic-class"), 1);
        ASSERT_EQ(result.mDiagnostics.size(), 8U);
        ASSERT_TRUE(std::holds_alternative<SourcedParseDiagnostic>(result.mDiagnostics[0]));
        const auto& futureDateWarning = std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]);

        EXPECT_EQ(futureDateWarning.mSourceIndex, 0U);
        EXPECT_EQ(futureDateWarning.mDiagnostic.mSeverity, DiagnosticSeverity::Warning);
        EXPECT_EQ(futureDateWarning.mDiagnostic.mCode, DiagnosticCode::InconsistentValue);
        EXPECT_EQ(futureDateWarning.mDiagnostic.mField, "datetime");
        EXPECT_EQ(futureDateWarning.mDiagnostic.mSourceFile, "history.csv");
        EXPECT_EQ(futureDateWarning.mDiagnostic.mRowIndex, 2U);
        EXPECT_EQ(futureDateWarning.mDiagnostic.mTransactionId, "synthetic-future");

        ASSERT_TRUE(std::holds_alternative<SourcedParseDiagnostic>(result.mDiagnostics[1]));
        const auto& olderDateWarning = std::get<SourcedParseDiagnostic>(result.mDiagnostics[1]);

        EXPECT_EQ(olderDateWarning.mSourceIndex, 0U);
        EXPECT_EQ(olderDateWarning.mDiagnostic.mSeverity, DiagnosticSeverity::Warning);
        EXPECT_EQ(olderDateWarning.mDiagnostic.mCode, DiagnosticCode::InconsistentValue);
        EXPECT_EQ(olderDateWarning.mDiagnostic.mField, "datetime");
        EXPECT_EQ(olderDateWarning.mDiagnostic.mSourceFile, "history.csv");
        EXPECT_EQ(olderDateWarning.mDiagnostic.mRowIndex, 3U);
        EXPECT_EQ(olderDateWarning.mDiagnostic.mTransactionId, "synthetic-older");

        ASSERT_TRUE(std::holds_alternative<SourcedParseDiagnostic>(result.mDiagnostics[2]));
        const auto& missingDividendTimeError =
            std::get<SourcedParseDiagnostic>(result.mDiagnostics[2]);

        EXPECT_EQ(missingDividendTimeError.mSourceIndex, 0U);
        EXPECT_EQ(missingDividendTimeError.mDiagnostic.mSeverity, DiagnosticSeverity::Error);
        EXPECT_EQ(missingDividendTimeError.mDiagnostic.mCode, DiagnosticCode::MissingField);
        EXPECT_EQ(missingDividendTimeError.mDiagnostic.mField, "datetime");
        EXPECT_EQ(missingDividendTimeError.mDiagnostic.mSourceFile, "history.csv");
        EXPECT_EQ(missingDividendTimeError.mDiagnostic.mRowIndex, 12U);
        EXPECT_EQ(missingDividendTimeError.mDiagnostic.mTransactionId,
                  "synthetic-missing-dividend-time");

        ASSERT_TRUE(std::holds_alternative<SourcedParseDiagnostic>(result.mDiagnostics[3]));
        const auto& missingPrivateTimeError =
            std::get<SourcedParseDiagnostic>(result.mDiagnostics[3]);

        EXPECT_EQ(missingPrivateTimeError.mSourceIndex, 0U);
        EXPECT_EQ(missingPrivateTimeError.mDiagnostic.mSeverity, DiagnosticSeverity::Error);
        EXPECT_EQ(missingPrivateTimeError.mDiagnostic.mCode, DiagnosticCode::MissingField);
        EXPECT_EQ(missingPrivateTimeError.mDiagnostic.mField, "datetime");
        EXPECT_EQ(missingPrivateTimeError.mDiagnostic.mSourceFile, "history.csv");
        EXPECT_EQ(missingPrivateTimeError.mDiagnostic.mRowIndex, 13U);
        EXPECT_EQ(missingPrivateTimeError.mDiagnostic.mTransactionId,
                  "synthetic-missing-private-time");

        const auto& parser = std::get<SourcedParseDiagnostic>(result.mDiagnostics[4]);
        const auto partial = std::find_if(inputs.begin(), inputs.end(), [](const auto& aInput) {
            return aInput.mSourceIndex == 2;
        });

        ASSERT_EQ(partial->mParseResult.mDiagnostics.size(), 1U);
        test::expectDiagnosticsEqual(
            {result.mDiagnostics[4]},
            {SourcedParseDiagnostic{.mSourceIndex = 2,
                                    .mBroker = partial->mParseResult.mBroker,
                                    .mDiagnostic = partial->mParseResult.mDiagnostics[0]}});

        EXPECT_EQ(parser.mSourceIndex, 2U);
        EXPECT_EQ(parser.mBroker, Broker::TradeRepublic);
        EXPECT_EQ(parser.mDiagnostic.mSeverity, DiagnosticSeverity::Error);
        EXPECT_EQ(parser.mDiagnostic.mCode, DiagnosticCode::InvalidValue);
        EXPECT_EQ(parser.mDiagnostic.mSourceFile, "partial.csv");
        EXPECT_EQ(parser.mDiagnostic.mRowIndex, 2U);
        EXPECT_EQ(parser.mDiagnostic.mTransactionId, "synthetic-rejected");
        EXPECT_EQ(parser.mDiagnostic.mField, "price");

        const auto& classConflict = std::get<MergeDiagnostic>(result.mDiagnostics[5]);
        const auto& nameConflict = std::get<MergeDiagnostic>(result.mDiagnostics[6]);
        const auto& transactionConflict = std::get<MergeDiagnostic>(result.mDiagnostics[7]);

        EXPECT_EQ(classConflict.mCode, MergeDiagnosticCode::InstrumentAssetClassConflict);
        EXPECT_EQ(classConflict.mSeverity, DiagnosticSeverity::Error);
        EXPECT_EQ(nameConflict.mCode, MergeDiagnosticCode::InstrumentNameConflict);
        EXPECT_EQ(nameConflict.mSeverity, DiagnosticSeverity::Warning);
        EXPECT_EQ(transactionConflict.mCode, MergeDiagnosticCode::ConflictingDuplicate);
        EXPECT_EQ(transactionConflict.mEventKinds, std::vector{StatementEventKind::Trade});

        ASSERT_EQ(transactionConflict.mSources.size(), 2U);
        EXPECT_EQ(transactionConflict.mSources[0].mInputSequence, (StableInputSequence{0, 2}));
        EXPECT_EQ(transactionConflict.mSources[0].mSourceRow, 4U);
        EXPECT_EQ(transactionConflict.mSources[1].mInputSequence, (StableInputSequence{1, 0}));
        EXPECT_EQ(transactionConflict.mSources[1].mSourceRow, 2U);
        EXPECT_EQ(result.mStatement.mPresentation.mTradeInstruments.at(0).mName,
                  "Synthetic Merger Share");
        EXPECT_EQ(result.mStatement.mPresentation.mTradeInstruments.at(0).mAssetClass,
                  AssetClass::Unknown);

        const auto& trades = result.mStatement.mPresentation.mTradeInstruments.at(0).mTransactions;
        const auto validPartialRow =
            std::find_if(trades.begin(), trades.end(), [](const auto& aTrade) {
                return primarySource(aTrade.mMetadata).mTransactionId == "synthetic-class";
            });

        ASSERT_NE(validPartialRow, trades.end());
        EXPECT_EQ(validPartialRow->mMetadata.mSources,
                  (std::vector{SourceReference{.mBroker = Broker::TradeRepublic,
                                               .mFilename = SourceFilename::fromPath("partial.csv"),
                                               .mSourceRow = 3,
                                               .mTransactionId = "synthetic-class",
                                               .mInputSequence = {2, 1}}}));
    } while (std::next_permutation(order.begin(), order.end()));
}

TEST(StatementMergerIntegrationTest, EmptyAndFailedFilesPreserveHealthyDataAndAllFailures) {
    // We get data from history.csv
    const TemporaryHistory history;

    const auto empty = history.writeRows("empty.csv", {});
    const std::array files{empty, fixturePath("invalid.csv"), fixturePath("history.csv")};
    std::array<std::size_t, 3> order{0, 1, 2};

    const auto expected = DeterministicStatementMerger{}.merge(parseInOrder(files, order));

    do
    {
        const auto inputs = parseInOrder(files, order);
        const auto result = DeterministicStatementMerger{}.merge(inputs);

        test::expectMergeResultsEqual(result, expected);

        const auto& presentation = result.mStatement.mPresentation;

        ASSERT_EQ(presentation.mTradeInstruments.size(), 1U);

        const auto& trades = presentation.mTradeInstruments.front().mTransactions;

        ASSERT_EQ(trades.size(), 4U);

        EXPECT_EQ(trades[0].mTradeSide, TradeSide::Buy);
        EXPECT_EQ(trades[0].mUnits, 200'000'000);
        EXPECT_EQ(trades[0].mUnitPrice, 90'000);
        EXPECT_EQ(trades[0].mAmount, 180'000);
        EXPECT_EQ(trades[0].mFeePaid, 0);
        EXPECT_EQ(trades[0].mCurrency, Currency::EUR);
        EXPECT_EQ(trades[0].mExchangeRate, 100'000'000);
        EXPECT_EQ(trades[0].mMetadata.mTaxDate, parseCalendarDate("2023-12-31"));

        EXPECT_EQ(trades[1].mTradeSide, TradeSide::Buy);
        EXPECT_EQ(trades[1].mUnits, 125'000'000);
        EXPECT_EQ(trades[1].mUnitPrice, 100'000);
        EXPECT_EQ(trades[1].mAmount, 125'000);
        EXPECT_EQ(trades[1].mFeePaid, 2'500);
        EXPECT_EQ(trades[1].mCurrency, Currency::EUR);
        EXPECT_EQ(trades[1].mExchangeRate, 100'000'000);
        EXPECT_EQ(trades[1].mMetadata.mTaxDate, parseCalendarDate("2024-01-02"));

        EXPECT_EQ(trades[2].mTradeSide, TradeSide::Buy);
        EXPECT_EQ(trades[2].mUnits, 12'500'000);
        EXPECT_EQ(trades[2].mUnitPrice, 80'000);
        EXPECT_EQ(trades[2].mAmount, 10'000);
        EXPECT_EQ(trades[2].mFeePaid, 0);
        EXPECT_EQ(trades[2].mCurrency, Currency::EUR);
        EXPECT_EQ(trades[2].mExchangeRate, 100'000'000);
        EXPECT_EQ(trades[2].mMetadata.mTaxDate, parseCalendarDate("2024-01-03"));

        EXPECT_EQ(trades[3].mTradeSide, TradeSide::Buy);
        EXPECT_EQ(trades[3].mUnits, 100'000'000);
        EXPECT_EQ(trades[3].mUnitPrice, 100'000);
        EXPECT_EQ(trades[3].mAmount, 100'000);
        EXPECT_EQ(trades[3].mFeePaid, 0);
        EXPECT_EQ(trades[3].mCurrency, Currency::EUR);
        EXPECT_EQ(trades[3].mExchangeRate, 100'000'000);
        EXPECT_EQ(trades[3].mMetadata.mTaxDate, parseCalendarDate("2025-01-01"));

        ASSERT_EQ(presentation.mDividendInstruments.size(), 1U);
        ASSERT_EQ(presentation.mDividendInstruments.front().mTransactions.size(), 1U);

        const auto& dividend = presentation.mDividendInstruments.front().mTransactions.front();

        EXPECT_EQ(dividend.mGrossAmount, 123'400);
        EXPECT_EQ(dividend.mTaxPaid, 23'400);
        EXPECT_EQ(dividend.mCurrency, Currency::EUR);
        EXPECT_EQ(dividend.mTaxCurrency, Currency::EUR);
        EXPECT_EQ(dividend.mExchangeRate, 100'000'000);
        ASSERT_EQ(presentation.mInterestInstruments.size(), 1U);
        ASSERT_EQ(presentation.mInterestInstruments.front().mTransactions.size(), 1U);

        const auto& interest = presentation.mInterestInstruments.front().mTransactions.front();

        EXPECT_EQ(interest.mGrossAmount, 12'300);
        EXPECT_FALSE(interest.mTaxPaid.has_value());
        EXPECT_EQ(interest.mCurrency, Currency::EUR);
        EXPECT_EQ(interest.mTaxCurrency, Currency::EUR);
        EXPECT_EQ(interest.mExchangeRate, 100'000'000);
        ASSERT_EQ(presentation.mBenefitEvents.size(), 1U);
        EXPECT_EQ(presentation.mBenefitEvents.front().mType, BenefitType::Saveback);
        EXPECT_EQ(presentation.mBenefitEvents.front().mAmount, 25'000);
        EXPECT_EQ(presentation.mBenefitEvents.front().mCurrency, Currency::EUR);
        ASSERT_EQ(presentation.mPrivateMarketEvents.size(), 1U);

        const auto& privatePurchase = presentation.mPrivateMarketEvents.front();

        EXPECT_EQ(privatePurchase.mType, PrivateMarketEventType::Buy);
        EXPECT_EQ(privatePurchase.mAmount, -250'000);
        EXPECT_EQ(privatePurchase.mFeePaid, 5'000);
        EXPECT_EQ(privatePurchase.mCurrency, Currency::EUR);
        ASSERT_EQ(presentation.mTradeInstruments.front().mCorporateActions.size(), 1U);

        const auto& action = presentation.mTradeInstruments.front().mCorporateActions.front();

        EXPECT_EQ(action.mType, CorporateActionType::UnresolvedSplit);
        EXPECT_EQ(action.mUnitsDelta, 200'000'000);
        EXPECT_FALSE(action.mRatio.has_value());

        EXPECT_EQ(ledgerIds(result),
                  (std::vector<std::string>{"synthetic-older",
                                            "synthetic-dividend",
                                            "synthetic-interest",
                                            "synthetic-tied",
                                            "synthetic-benefit",
                                            "synthetic-action",
                                            "synthetic-private",
                                            "",
                                            "synthetic-future"}));

        EXPECT_EQ(ledgerIds(result).size(), 9U);
        ASSERT_EQ(result.mDiagnostics.size(), 5U);
        ASSERT_TRUE(std::holds_alternative<SourcedParseDiagnostic>(result.mDiagnostics[1]));
        const auto& futureDateWarning = std::get<SourcedParseDiagnostic>(result.mDiagnostics[1]);

        EXPECT_EQ(futureDateWarning.mSourceIndex, 2U);
        EXPECT_EQ(futureDateWarning.mDiagnostic.mSeverity, DiagnosticSeverity::Warning);
        EXPECT_EQ(futureDateWarning.mDiagnostic.mCode, DiagnosticCode::InconsistentValue);
        EXPECT_EQ(futureDateWarning.mDiagnostic.mField, "datetime");
        EXPECT_EQ(futureDateWarning.mDiagnostic.mSourceFile, "history.csv");
        EXPECT_EQ(futureDateWarning.mDiagnostic.mRowIndex, 2U);
        EXPECT_EQ(futureDateWarning.mDiagnostic.mTransactionId, "synthetic-future");

        ASSERT_TRUE(std::holds_alternative<SourcedParseDiagnostic>(result.mDiagnostics[2]));
        const auto& olderDateWarning = std::get<SourcedParseDiagnostic>(result.mDiagnostics[2]);

        EXPECT_EQ(olderDateWarning.mSourceIndex, 2U);
        EXPECT_EQ(olderDateWarning.mDiagnostic.mSeverity, DiagnosticSeverity::Warning);
        EXPECT_EQ(olderDateWarning.mDiagnostic.mCode, DiagnosticCode::InconsistentValue);
        EXPECT_EQ(olderDateWarning.mDiagnostic.mField, "datetime");
        EXPECT_EQ(olderDateWarning.mDiagnostic.mSourceFile, "history.csv");
        EXPECT_EQ(olderDateWarning.mDiagnostic.mRowIndex, 3U);
        EXPECT_EQ(olderDateWarning.mDiagnostic.mTransactionId, "synthetic-older");

        ASSERT_TRUE(std::holds_alternative<SourcedParseDiagnostic>(result.mDiagnostics[3]));
        const auto& missingDividendTimeError =
            std::get<SourcedParseDiagnostic>(result.mDiagnostics[3]);

        EXPECT_EQ(missingDividendTimeError.mSourceIndex, 2U);
        EXPECT_EQ(missingDividendTimeError.mDiagnostic.mSeverity, DiagnosticSeverity::Error);
        EXPECT_EQ(missingDividendTimeError.mDiagnostic.mCode, DiagnosticCode::MissingField);
        EXPECT_EQ(missingDividendTimeError.mDiagnostic.mField, "datetime");
        EXPECT_EQ(missingDividendTimeError.mDiagnostic.mSourceFile, "history.csv");
        EXPECT_EQ(missingDividendTimeError.mDiagnostic.mRowIndex, 12U);
        EXPECT_EQ(missingDividendTimeError.mDiagnostic.mTransactionId,
                  "synthetic-missing-dividend-time");

        ASSERT_TRUE(std::holds_alternative<SourcedParseDiagnostic>(result.mDiagnostics[4]));
        const auto& missingPrivateTimeError =
            std::get<SourcedParseDiagnostic>(result.mDiagnostics[4]);

        EXPECT_EQ(missingPrivateTimeError.mSourceIndex, 2U);
        EXPECT_EQ(missingPrivateTimeError.mDiagnostic.mSeverity, DiagnosticSeverity::Error);
        EXPECT_EQ(missingPrivateTimeError.mDiagnostic.mCode, DiagnosticCode::MissingField);
        EXPECT_EQ(missingPrivateTimeError.mDiagnostic.mField, "datetime");
        EXPECT_EQ(missingPrivateTimeError.mDiagnostic.mSourceFile, "history.csv");
        EXPECT_EQ(missingPrivateTimeError.mDiagnostic.mRowIndex, 13U);
        EXPECT_EQ(missingPrivateTimeError.mDiagnostic.mTransactionId,
                  "synthetic-missing-private-time");

        const auto& diagnostic = std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]);

        EXPECT_EQ(diagnostic.mSourceIndex, 1U);
        EXPECT_EQ(diagnostic.mDiagnostic.mCode, DiagnosticCode::InvalidHeader);
        EXPECT_EQ(diagnostic.mDiagnostic.mSourceFile, "invalid.csv");
        EXPECT_FALSE(diagnostic.mDiagnostic.mRowIndex.has_value());
    } while (std::next_permutation(order.begin(), order.end()));

    const std::array failedFiles{empty,
                                 fixturePath("invalid.csv"),
                                 empty.parent_path() / "missing.csv"};
    const auto failed = DeterministicStatementMerger{}.merge(parseInOrder(failedFiles, {2, 0, 1}));

    EXPECT_TRUE(failed.mStatement.mChronologicalOrder.empty());
    test::expectStatementsEqual(failed.mStatement.mPresentation, BrokerStatement{});
    ASSERT_EQ(failed.mDiagnostics.size(), 2U);
    EXPECT_EQ(std::get<SourcedParseDiagnostic>(failed.mDiagnostics[0]).mSourceIndex, 1U);
    EXPECT_EQ(std::get<SourcedParseDiagnostic>(failed.mDiagnostics[1]).mSourceIndex, 2U);
    EXPECT_EQ(std::get<SourcedParseDiagnostic>(failed.mDiagnostics[1]).mDiagnostic.mSourceFile,
              "missing.csv");

    const std::array emptyFiles{empty, empty, empty};
    const auto allEmpty = DeterministicStatementMerger{}.merge(parseInOrder(emptyFiles, {2, 1, 0}));

    test::expectStatementsEqual(allEmpty.mStatement.mPresentation, BrokerStatement{});
    EXPECT_TRUE(allEmpty.mStatement.mChronologicalOrder.empty());
    EXPECT_TRUE(allEmpty.mDiagnostics.empty());
}

TEST(StatementMergerIntegrationTest, RequestOrderBreaksEqualTimestampTiesWithoutChangingDates) {
    const TemporaryHistory history;
    const auto tradeFile = history.writeRows("trade.csv", {2}, "2024-01-02T09:00:00.000Z");
    const auto benefitFile = history.writeRows("benefit.csv", {7}, "2024-01-02T09:00:00.000Z");
    const auto olderFile = history.writeRows("older.csv", {1});
    const std::array tradeFirstFiles{tradeFile, benefitFile, olderFile};
    const std::array benefitFirstFiles{benefitFile, tradeFile, olderFile};
    const auto tradeFirstInputs = parseInOrder(tradeFirstFiles, {2, 1, 0});
    const auto benefitFirstInputs = parseInOrder(benefitFirstFiles, {1, 2, 0});

    const auto tradeFirstResult = DeterministicStatementMerger{}.merge(tradeFirstInputs);
    const auto benefitFirstResult = DeterministicStatementMerger{}.merge(benefitFirstInputs);

    EXPECT_EQ(ledgerIds(tradeFirstResult),
              (std::vector<std::string>{"synthetic-older", "synthetic-tied", "synthetic-benefit"}));
    EXPECT_EQ(ledgerIds(benefitFirstResult),
              (std::vector<std::string>{"synthetic-older", "synthetic-benefit", "synthetic-tied"}));
    test::expectStatementsEqual(tradeFirstResult.mStatement.mPresentation,
                                benefitFirstResult.mStatement.mPresentation,
                                false);
    ASSERT_EQ(tradeFirstResult.mDiagnostics.size(), 1U);
    ASSERT_EQ(benefitFirstResult.mDiagnostics.size(), 1U);

    const auto& olderDateWarning =
        std::get<SourcedParseDiagnostic>(tradeFirstResult.mDiagnostics[0]);

    EXPECT_EQ(olderDateWarning.mDiagnostic.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(olderDateWarning.mDiagnostic.mCode, DiagnosticCode::InconsistentValue);
    EXPECT_EQ(olderDateWarning.mDiagnostic.mField, "datetime");
}

TEST(StatementMergerIntegrationTest, MissingDatetimesRejectTransactionsRegardlessOfRequestOrder) {
    const TemporaryHistory history;
    const auto tradeFile = history.writeRows("trade.csv", {2}, "");
    const auto benefitFile = history.writeRows("benefit.csv", {7}, "");
    const auto olderFile = history.writeRows("older.csv", {1});
    const std::array tradeFirstFiles{tradeFile, benefitFile, olderFile};
    const std::array benefitFirstFiles{benefitFile, tradeFile, olderFile};
    const auto tradeFirstInputs = parseInOrder(tradeFirstFiles, {2, 1, 0});
    const auto benefitFirstInputs = parseInOrder(benefitFirstFiles, {1, 2, 0});

    const auto tradeFirstResult = DeterministicStatementMerger{}.merge(tradeFirstInputs);
    const auto benefitFirstResult = DeterministicStatementMerger{}.merge(benefitFirstInputs);

    EXPECT_EQ(ledgerIds(tradeFirstResult), (std::vector<std::string>{"synthetic-older"}));
    EXPECT_EQ(ledgerIds(benefitFirstResult), (std::vector<std::string>{"synthetic-older"}));
    test::expectStatementsEqual(tradeFirstResult.mStatement.mPresentation,
                                benefitFirstResult.mStatement.mPresentation,
                                false);
    ASSERT_EQ(tradeFirstResult.mDiagnostics.size(), 3U);
    ASSERT_EQ(benefitFirstResult.mDiagnostics.size(), 3U);

    const auto& missingTradeTimeError =
        std::get<SourcedParseDiagnostic>(tradeFirstResult.mDiagnostics[0]);

    EXPECT_EQ(missingTradeTimeError.mDiagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(missingTradeTimeError.mDiagnostic.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(missingTradeTimeError.mDiagnostic.mField, "datetime");

    const auto& missingBenefitTimeError =
        std::get<SourcedParseDiagnostic>(tradeFirstResult.mDiagnostics[1]);

    EXPECT_EQ(missingBenefitTimeError.mDiagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(missingBenefitTimeError.mDiagnostic.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(missingBenefitTimeError.mDiagnostic.mField, "datetime");

    const auto& olderDateWarning =
        std::get<SourcedParseDiagnostic>(tradeFirstResult.mDiagnostics[2]);

    EXPECT_EQ(olderDateWarning.mDiagnostic.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(olderDateWarning.mDiagnostic.mCode, DiagnosticCode::InconsistentValue);
    EXPECT_EQ(olderDateWarning.mDiagnostic.mField, "datetime");
}

TEST(StatementMergerIntegrationTest, InvalidDatetimeWarnsButMissingDatetimeRejectsTheTransaction) {
    const TemporaryHistory history;
    const std::array files{history.writeRows("invalid-time.csv", {2}, "invalid-time"),
                           history.writeRows("missing-time.csv", {7}, ""),
                           history.writeRows("timed.csv", {5})};
    std::array<std::size_t, 3> order{0, 1, 2};
    const auto expected = DeterministicStatementMerger{}.merge(parseInOrder(files, order));

    do
    {
        const auto inputs = parseInOrder(files, order);
        const auto result = DeterministicStatementMerger{}.merge(inputs);

        test::expectMergeResultsEqual(result, expected);
        EXPECT_EQ(ledgerIds(result),
                  (std::vector<std::string>{"synthetic-interest", "synthetic-tied"}));
        ASSERT_EQ(result.mDiagnostics.size(), 2U);

        const auto& diagnostic = std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]);
        const auto invalid = std::find_if(inputs.begin(), inputs.end(), [](const auto& aInput) {
            return aInput.mSourceIndex == 0;
        });

        ASSERT_EQ(invalid->mParseResult.mDiagnostics.size(), 1U);
        const auto missing = std::find_if(inputs.begin(), inputs.end(), [](const auto& aInput) {
            return aInput.mSourceIndex == 1;
        });

        ASSERT_NE(missing, inputs.end());
        ASSERT_EQ(missing->mParseResult.mDiagnostics.size(), 1U);
        test::expectDiagnosticsEqual(
            result.mDiagnostics,
            {SourcedParseDiagnostic{.mSourceIndex = 0,
                                    .mBroker = invalid->mParseResult.mBroker,
                                    .mDiagnostic = invalid->mParseResult.mDiagnostics[0]},
             SourcedParseDiagnostic{.mSourceIndex = 1,
                                    .mBroker = missing->mParseResult.mBroker,
                                    .mDiagnostic = missing->mParseResult.mDiagnostics[0]}});

        const auto& missingDiagnostic = std::get<SourcedParseDiagnostic>(result.mDiagnostics[1]);

        EXPECT_EQ(missingDiagnostic.mDiagnostic.mSeverity, DiagnosticSeverity::Error);
        EXPECT_EQ(missingDiagnostic.mDiagnostic.mCode, DiagnosticCode::MissingField);
        EXPECT_EQ(missingDiagnostic.mDiagnostic.mSourceFile, "missing-time.csv");
        EXPECT_EQ(missingDiagnostic.mDiagnostic.mRowIndex, 2U);
        EXPECT_EQ(missingDiagnostic.mDiagnostic.mField, "datetime");

        EXPECT_EQ(diagnostic.mSourceIndex, 0U);
        EXPECT_EQ(diagnostic.mDiagnostic.mSeverity, DiagnosticSeverity::Warning);
        EXPECT_EQ(diagnostic.mDiagnostic.mCode, DiagnosticCode::InvalidValue);
        EXPECT_EQ(diagnostic.mDiagnostic.mSourceFile, "invalid-time.csv");
        EXPECT_EQ(diagnostic.mDiagnostic.mRowIndex, 2U);
        EXPECT_EQ(diagnostic.mDiagnostic.mField, "datetime");
        EXPECT_FALSE(
            test::referencedMetadata(result.mStatement, result.mStatement.mChronologicalOrder[1])
                .mSourceTimestamp.has_value());
        EXPECT_TRUE(result.mStatement.mPresentation.mBenefitEvents.empty());
    } while (std::next_permutation(order.begin(), order.end()));
}

TEST(StatementMergerIntegrationTest, DateMismatchUsesNineOClockBetweenReliableTimes) {
    const TemporaryHistory history;
    const auto tradeAndSplitFile =
        history.writeRows("mismatch.csv", {2, 6}, "2024-01-03T23:00:00.000Z");
    const auto earlyInterestFile = history.writeRows("early.csv", {5}, "2024-01-02T08:00:00.000Z");
    const auto lateBenefitFile = history.writeRows("late.csv", {7}, "2024-01-02T10:00:00.000Z");
    const std::array files{tradeAndSplitFile, earlyInterestFile, lateBenefitFile};
    std::array<std::size_t, 3> order{0, 1, 2};

    do
    {
        const auto inputs = parseInOrder(files, order);

        const auto result = DeterministicStatementMerger{}.merge(inputs);

        EXPECT_EQ(ledgerIds(result),
                  (std::vector<std::string>{"synthetic-interest",
                                            "synthetic-tied",
                                            "synthetic-action",
                                            "synthetic-benefit"}));
        ASSERT_EQ(result.mDiagnostics.size(), 2U);
        ASSERT_TRUE(std::holds_alternative<SourcedParseDiagnostic>(result.mDiagnostics[0]));
        const auto& tradeDateWarning = std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]);

        EXPECT_EQ(tradeDateWarning.mSourceIndex, 0U);
        EXPECT_EQ(tradeDateWarning.mDiagnostic.mSeverity, DiagnosticSeverity::Warning);
        EXPECT_EQ(tradeDateWarning.mDiagnostic.mCode, DiagnosticCode::InconsistentValue);
        EXPECT_EQ(tradeDateWarning.mDiagnostic.mField, "datetime");
        EXPECT_EQ(tradeDateWarning.mDiagnostic.mSourceFile, "mismatch.csv");
        EXPECT_EQ(tradeDateWarning.mDiagnostic.mRowIndex, 2U);
        EXPECT_EQ(tradeDateWarning.mDiagnostic.mTransactionId, "synthetic-tied");

        ASSERT_TRUE(std::holds_alternative<SourcedParseDiagnostic>(result.mDiagnostics[1]));
        const auto& splitDateWarning = std::get<SourcedParseDiagnostic>(result.mDiagnostics[1]);

        EXPECT_EQ(splitDateWarning.mSourceIndex, 0U);
        EXPECT_EQ(splitDateWarning.mDiagnostic.mSeverity, DiagnosticSeverity::Warning);
        EXPECT_EQ(splitDateWarning.mDiagnostic.mCode, DiagnosticCode::InconsistentValue);
        EXPECT_EQ(splitDateWarning.mDiagnostic.mField, "datetime");
        EXPECT_EQ(splitDateWarning.mDiagnostic.mSourceFile, "mismatch.csv");
        EXPECT_EQ(splitDateWarning.mDiagnostic.mRowIndex, 3U);
        EXPECT_EQ(splitDateWarning.mDiagnostic.mTransactionId, "synthetic-action");

        const auto& tradeMetadata =
            test::referencedMetadata(result.mStatement, result.mStatement.mChronologicalOrder[1]);
        const auto& splitMetadata =
            test::referencedMetadata(result.mStatement, result.mStatement.mChronologicalOrder[2]);

        EXPECT_EQ(tradeMetadata.mTaxDate, parseCalendarDate("2024-01-02"));
        EXPECT_EQ(tradeMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-03T23:00:00Z"));
        EXPECT_EQ(tradeMetadata.mOrderingTimestamp, parseSourceTimestamp("2024-01-02T09:00:00Z"));

        EXPECT_EQ(splitMetadata.mTaxDate, parseCalendarDate("2024-01-02"));
        EXPECT_EQ(splitMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-03T23:00:00Z"));
        EXPECT_EQ(splitMetadata.mOrderingTimestamp, parseSourceTimestamp("2024-01-02T09:00:00Z"));
    } while (std::next_permutation(order.begin(), order.end()));
}

TEST(StatementMergerIntegrationTest, EqualFallbackTimesDoNotHideDifferentSourceTimestamps) {
    const TemporaryHistory history;
    const std::array files{
        history.writeRows("first.csv", {2}, "2024-01-03T08:00:00.000Z"),
        history.writeRows("second.csv", {2}, "2024-01-04T08:00:00.000Z"),
    };
    const auto inputs = parseInOrder(files, {1, 0});

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    EXPECT_TRUE(result.mStatement.mChronologicalOrder.empty());
    ASSERT_EQ(result.mDiagnostics.size(), 3U);
    const auto& conflict = std::get<MergeDiagnostic>(result.mDiagnostics[2]);

    EXPECT_EQ(conflict.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(conflict.mCode, MergeDiagnosticCode::ConflictingDuplicate);
    EXPECT_EQ(conflict.mSources.size(), 2U);
}

TEST(StatementMergerIntegrationTest, IdenticalBlankIncomeFactsDeduplicateWithoutInventingValues) {
    const std::array files{fixturePath("unknown_income.csv"), fixturePath("unknown_income.csv")};
    const auto inputs = parseInOrder(files, {1, 0});
    const auto forwardInputs = parseInOrder(files, {0, 1});

    const auto result = DeterministicStatementMerger{}.merge(inputs);
    const auto forward = DeterministicStatementMerger{}.merge(forwardInputs);

    test::expectMergeResultsEqual(result, forward);
    EXPECT_TRUE(result.mDiagnostics.empty());
    EXPECT_EQ(ledgerIds(result),
              (std::vector<std::string>{"synthetic-healthy-trade",
                                        "synthetic-optional-dividend",
                                        "synthetic-optional-deposit",
                                        "synthetic-optional-coupon",
                                        "synthetic-healthy-dividend"}));
    ASSERT_EQ(result.mStatement.mPresentation.mDividendInstruments.size(), 1U);

    const auto& dividends = result.mStatement.mPresentation.mDividendInstruments.front();

    EXPECT_EQ(dividends.mIsin, "XX9000000001");
    ASSERT_EQ(dividends.mTransactions.size(), 2U);

    const auto& dividend = dividends.mTransactions[0];
    const auto& knownDividend = dividends.mTransactions[1];

    EXPECT_EQ(dividend.mGrossAmount, 123'400);
    EXPECT_EQ(dividend.mCurrency, Currency::USD);
    EXPECT_EQ(dividend.mTaxCurrency, Currency::EUR);
    EXPECT_FALSE(dividend.mTaxPaid.has_value());
    EXPECT_FALSE(dividend.mExchangeRate.has_value());
    EXPECT_EQ(
        dividend.mMetadata.mSources,
        (std::vector<SourceReference>{{.mBroker = Broker::TradeRepublic,
                                       .mFilename = SourceFilename::fromPath("unknown_income.csv"),
                                       .mSourceRow = 2,
                                       .mTransactionId = "synthetic-optional-dividend",
                                       .mInputSequence = {0, 0}},
                                      {.mBroker = Broker::TradeRepublic,
                                       .mFilename = SourceFilename::fromPath("unknown_income.csv"),
                                       .mSourceRow = 2,
                                       .mTransactionId = "synthetic-optional-dividend",
                                       .mInputSequence = {1, 0}}}));

    EXPECT_EQ(knownDividend.mGrossAmount, 50'000);
    EXPECT_EQ(knownDividend.mTaxPaid, 0);
    EXPECT_EQ(knownDividend.mExchangeRate, 100'000'000);
    EXPECT_EQ(knownDividend.mCurrency, Currency::EUR);
    EXPECT_EQ(knownDividend.mTaxCurrency, Currency::EUR);
    ASSERT_EQ(knownDividend.mMetadata.mSources.size(), 2U);

    ASSERT_EQ(result.mStatement.mPresentation.mInterestInstruments.size(), 2U);

    const auto& bond = result.mStatement.mPresentation.mInterestInstruments[0];
    const auto& deposit = result.mStatement.mPresentation.mInterestInstruments[1];

    EXPECT_EQ(bond.mInterestType, InterestType::BondInterest);
    EXPECT_EQ(bond.mIsin, "XX9000000002");
    EXPECT_EQ(bond.mName, "Synthetic Optional Bond");
    ASSERT_EQ(bond.mTransactions.size(), 1U);

    const auto& coupon = bond.mTransactions.front();

    EXPECT_EQ(coupon.mGrossAmount, 67'800);
    EXPECT_EQ(coupon.mCurrency, Currency::USD);
    EXPECT_EQ(coupon.mTaxCurrency, Currency::EUR);
    EXPECT_FALSE(coupon.mTaxPaid.has_value());
    EXPECT_FALSE(coupon.mExchangeRate.has_value());
    EXPECT_EQ(
        coupon.mMetadata.mSources,
        (std::vector<SourceReference>{{.mBroker = Broker::TradeRepublic,
                                       .mFilename = SourceFilename::fromPath("unknown_income.csv"),
                                       .mSourceRow = 4,
                                       .mTransactionId = "synthetic-optional-coupon",
                                       .mInputSequence = {0, 2}},
                                      {.mBroker = Broker::TradeRepublic,
                                       .mFilename = SourceFilename::fromPath("unknown_income.csv"),
                                       .mSourceRow = 4,
                                       .mTransactionId = "synthetic-optional-coupon",
                                       .mInputSequence = {1, 2}}}));

    EXPECT_EQ(deposit.mInterestType, InterestType::BrokerInterest);
    EXPECT_FALSE(deposit.mIsin.has_value());
    EXPECT_EQ(deposit.mName, "Trade Republic");
    ASSERT_EQ(deposit.mTransactions.size(), 1U);

    const auto& cashInterest = deposit.mTransactions.front();

    EXPECT_EQ(cashInterest.mGrossAmount, 34'500);
    EXPECT_EQ(cashInterest.mCurrency, Currency::USD);
    EXPECT_EQ(cashInterest.mTaxCurrency, Currency::EUR);
    EXPECT_FALSE(cashInterest.mTaxPaid.has_value());
    EXPECT_FALSE(cashInterest.mExchangeRate.has_value());
    EXPECT_EQ(
        cashInterest.mMetadata.mSources,
        (std::vector<SourceReference>{{.mBroker = Broker::TradeRepublic,
                                       .mFilename = SourceFilename::fromPath("unknown_income.csv"),
                                       .mSourceRow = 3,
                                       .mTransactionId = "synthetic-optional-deposit",
                                       .mInputSequence = {0, 1}},
                                      {.mBroker = Broker::TradeRepublic,
                                       .mFilename = SourceFilename::fromPath("unknown_income.csv"),
                                       .mSourceRow = 3,
                                       .mTransactionId = "synthetic-optional-deposit",
                                       .mInputSequence = {1, 1}}}));
}

TEST(StatementMergerIntegrationTest,
     BlankIncomeTaxConflictsWithExplicitZeroWhileHealthyRowsSurvive) {
    // Blank tax is unknown; an overlapping explicit zero is a different fact.
    const std::array files{fixturePath("unknown_income.csv"),
                           fixturePath("confirmed_tax_income.csv")};
    const auto inputs = parseInOrder(files, {1, 0});
    const auto forwardInputs = parseInOrder(files, {0, 1});

    const auto result = DeterministicStatementMerger{}.merge(inputs);
    const auto forward = DeterministicStatementMerger{}.merge(forwardInputs);

    test::expectMergeResultsEqual(result, forward);
    EXPECT_EQ(ledgerIds(result),
              (std::vector<std::string>{"synthetic-healthy-trade", "synthetic-healthy-dividend"}));
    EXPECT_TRUE(result.mStatement.mPresentation.mInterestInstruments.empty());
    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments.front();

    EXPECT_EQ(instrument.mIsin, "XX9000000001");
    ASSERT_EQ(instrument.mTransactions.size(), 1U);

    const auto& trade = instrument.mTransactions.front();

    EXPECT_EQ(trade.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(trade.mUnitPrice, 80'000);
    EXPECT_EQ(trade.mUnits, 125'000'000);
    EXPECT_EQ(trade.mAmount, 100'000);
    EXPECT_EQ(trade.mFeePaid, 2'500);
    EXPECT_EQ(
        trade.mMetadata.mSources,
        (std::vector<SourceReference>{{.mBroker = Broker::TradeRepublic,
                                       .mFilename = SourceFilename::fromPath("unknown_income.csv"),
                                       .mSourceRow = 5,
                                       .mTransactionId = "synthetic-healthy-trade",
                                       .mInputSequence = {0, 3}}}));

    ASSERT_EQ(result.mStatement.mPresentation.mDividendInstruments.size(), 1U);

    const auto& dividends = result.mStatement.mPresentation.mDividendInstruments.front();

    ASSERT_EQ(dividends.mTransactions.size(), 1U);

    const auto& dividend = dividends.mTransactions.front();

    EXPECT_EQ(dividend.mGrossAmount, 50'000);
    EXPECT_EQ(dividend.mTaxPaid, 0);
    EXPECT_EQ(dividend.mExchangeRate, 100'000'000);
    EXPECT_EQ(
        dividend.mMetadata.mSources,
        (std::vector<SourceReference>{{.mBroker = Broker::TradeRepublic,
                                       .mFilename = SourceFilename::fromPath("unknown_income.csv"),
                                       .mSourceRow = 6,
                                       .mTransactionId = "synthetic-healthy-dividend",
                                       .mInputSequence = {0, 4}}}));

    ASSERT_EQ(result.mDiagnostics.size(), 3U);

    ASSERT_TRUE(std::holds_alternative<MergeDiagnostic>(result.mDiagnostics[0]));

    const auto& dividendConflict = std::get<MergeDiagnostic>(result.mDiagnostics[0]);

    EXPECT_EQ(dividendConflict.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(dividendConflict.mCode, MergeDiagnosticCode::ConflictingDuplicate);
    EXPECT_EQ(dividendConflict.mEventKinds, (std::vector{StatementEventKind::Dividend}));
    EXPECT_EQ(dividendConflict.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(dividendConflict.mIsin, "XX9000000001");
    EXPECT_EQ(dividendConflict.mSources,
              (std::vector<SourceReference>{
                  {.mBroker = Broker::TradeRepublic,
                   .mFilename = SourceFilename::fromPath("unknown_income.csv"),
                   .mSourceRow = 2,
                   .mTransactionId = "synthetic-optional-dividend",
                   .mInputSequence = {0, 0}},
                  {.mBroker = Broker::TradeRepublic,
                   .mFilename = SourceFilename::fromPath("confirmed_tax_income.csv"),
                   .mSourceRow = 2,
                   .mTransactionId = "synthetic-optional-dividend",
                   .mInputSequence = {1, 0}}}));

    ASSERT_TRUE(std::holds_alternative<MergeDiagnostic>(result.mDiagnostics[1]));

    const auto& depositConflict = std::get<MergeDiagnostic>(result.mDiagnostics[1]);

    EXPECT_EQ(depositConflict.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(depositConflict.mCode, MergeDiagnosticCode::ConflictingDuplicate);
    EXPECT_EQ(depositConflict.mEventKinds, (std::vector{StatementEventKind::Interest}));
    EXPECT_EQ(depositConflict.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(depositConflict.mIsin, std::nullopt);
    EXPECT_EQ(depositConflict.mSources,
              (std::vector<SourceReference>{
                  {.mBroker = Broker::TradeRepublic,
                   .mFilename = SourceFilename::fromPath("unknown_income.csv"),
                   .mSourceRow = 3,
                   .mTransactionId = "synthetic-optional-deposit",
                   .mInputSequence = {0, 1}},
                  {.mBroker = Broker::TradeRepublic,
                   .mFilename = SourceFilename::fromPath("confirmed_tax_income.csv"),
                   .mSourceRow = 3,
                   .mTransactionId = "synthetic-optional-deposit",
                   .mInputSequence = {1, 1}}}));

    ASSERT_TRUE(std::holds_alternative<MergeDiagnostic>(result.mDiagnostics[2]));

    const auto& couponConflict = std::get<MergeDiagnostic>(result.mDiagnostics[2]);

    EXPECT_EQ(couponConflict.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(couponConflict.mCode, MergeDiagnosticCode::ConflictingDuplicate);
    EXPECT_EQ(couponConflict.mEventKinds, (std::vector{StatementEventKind::Interest}));
    EXPECT_EQ(couponConflict.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(couponConflict.mIsin, "XX9000000002");
    EXPECT_EQ(couponConflict.mSources,
              (std::vector<SourceReference>{
                  {.mBroker = Broker::TradeRepublic,
                   .mFilename = SourceFilename::fromPath("unknown_income.csv"),
                   .mSourceRow = 4,
                   .mTransactionId = "synthetic-optional-coupon",
                   .mInputSequence = {0, 2}},
                  {.mBroker = Broker::TradeRepublic,
                   .mFilename = SourceFilename::fromPath("confirmed_tax_income.csv"),
                   .mSourceRow = 4,
                   .mTransactionId = "synthetic-optional-coupon",
                   .mInputSequence = {1, 2}}}));
}

TEST(StatementMergerIntegrationTest,
     AbsentIncomeRateConflictsWithPopulatedRateWhileHealthyRowsSurvive) {
    // An absent broker rate cannot be silently filled from an overlapping export.
    const std::array files{fixturePath("unknown_income.csv"),
                           fixturePath("broker_rate_income.csv")};
    const auto inputs = parseInOrder(files, {1, 0});
    const auto forwardInputs = parseInOrder(files, {0, 1});

    const auto result = DeterministicStatementMerger{}.merge(inputs);
    const auto forward = DeterministicStatementMerger{}.merge(forwardInputs);

    test::expectMergeResultsEqual(result, forward);
    EXPECT_EQ(ledgerIds(result),
              (std::vector<std::string>{"synthetic-healthy-trade", "synthetic-healthy-dividend"}));
    EXPECT_TRUE(result.mStatement.mPresentation.mInterestInstruments.empty());
    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments.front();

    EXPECT_EQ(instrument.mIsin, "XX9000000001");
    ASSERT_EQ(instrument.mTransactions.size(), 1U);

    const auto& trade = instrument.mTransactions.front();

    EXPECT_EQ(trade.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(trade.mUnitPrice, 80'000);
    EXPECT_EQ(trade.mUnits, 125'000'000);
    EXPECT_EQ(trade.mAmount, 100'000);
    EXPECT_EQ(trade.mFeePaid, 2'500);
    EXPECT_EQ(
        trade.mMetadata.mSources,
        (std::vector<SourceReference>{{.mBroker = Broker::TradeRepublic,
                                       .mFilename = SourceFilename::fromPath("unknown_income.csv"),
                                       .mSourceRow = 5,
                                       .mTransactionId = "synthetic-healthy-trade",
                                       .mInputSequence = {0, 3}}}));

    ASSERT_EQ(result.mStatement.mPresentation.mDividendInstruments.size(), 1U);

    const auto& dividends = result.mStatement.mPresentation.mDividendInstruments.front();

    ASSERT_EQ(dividends.mTransactions.size(), 1U);

    const auto& dividend = dividends.mTransactions.front();

    EXPECT_EQ(dividend.mGrossAmount, 50'000);
    EXPECT_EQ(dividend.mTaxPaid, 0);
    EXPECT_EQ(dividend.mExchangeRate, 100'000'000);
    EXPECT_EQ(
        dividend.mMetadata.mSources,
        (std::vector<SourceReference>{{.mBroker = Broker::TradeRepublic,
                                       .mFilename = SourceFilename::fromPath("unknown_income.csv"),
                                       .mSourceRow = 6,
                                       .mTransactionId = "synthetic-healthy-dividend",
                                       .mInputSequence = {0, 4}}}));

    ASSERT_EQ(result.mDiagnostics.size(), 3U);

    ASSERT_TRUE(std::holds_alternative<MergeDiagnostic>(result.mDiagnostics[0]));

    const auto& dividendConflict = std::get<MergeDiagnostic>(result.mDiagnostics[0]);

    EXPECT_EQ(dividendConflict.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(dividendConflict.mCode, MergeDiagnosticCode::ConflictingDuplicate);
    EXPECT_EQ(dividendConflict.mEventKinds, (std::vector{StatementEventKind::Dividend}));
    EXPECT_EQ(dividendConflict.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(dividendConflict.mIsin, "XX9000000001");
    EXPECT_EQ(dividendConflict.mSources,
              (std::vector<SourceReference>{
                  {.mBroker = Broker::TradeRepublic,
                   .mFilename = SourceFilename::fromPath("unknown_income.csv"),
                   .mSourceRow = 2,
                   .mTransactionId = "synthetic-optional-dividend",
                   .mInputSequence = {0, 0}},
                  {.mBroker = Broker::TradeRepublic,
                   .mFilename = SourceFilename::fromPath("broker_rate_income.csv"),
                   .mSourceRow = 2,
                   .mTransactionId = "synthetic-optional-dividend",
                   .mInputSequence = {1, 0}}}));

    ASSERT_TRUE(std::holds_alternative<MergeDiagnostic>(result.mDiagnostics[1]));

    const auto& depositConflict = std::get<MergeDiagnostic>(result.mDiagnostics[1]);

    EXPECT_EQ(depositConflict.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(depositConflict.mCode, MergeDiagnosticCode::ConflictingDuplicate);
    EXPECT_EQ(depositConflict.mEventKinds, (std::vector{StatementEventKind::Interest}));
    EXPECT_EQ(depositConflict.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(depositConflict.mIsin, std::nullopt);
    EXPECT_EQ(depositConflict.mSources,
              (std::vector<SourceReference>{
                  {.mBroker = Broker::TradeRepublic,
                   .mFilename = SourceFilename::fromPath("unknown_income.csv"),
                   .mSourceRow = 3,
                   .mTransactionId = "synthetic-optional-deposit",
                   .mInputSequence = {0, 1}},
                  {.mBroker = Broker::TradeRepublic,
                   .mFilename = SourceFilename::fromPath("broker_rate_income.csv"),
                   .mSourceRow = 3,
                   .mTransactionId = "synthetic-optional-deposit",
                   .mInputSequence = {1, 1}}}));

    ASSERT_TRUE(std::holds_alternative<MergeDiagnostic>(result.mDiagnostics[2]));

    const auto& couponConflict = std::get<MergeDiagnostic>(result.mDiagnostics[2]);

    EXPECT_EQ(couponConflict.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(couponConflict.mCode, MergeDiagnosticCode::ConflictingDuplicate);
    EXPECT_EQ(couponConflict.mEventKinds, (std::vector{StatementEventKind::Interest}));
    EXPECT_EQ(couponConflict.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(couponConflict.mIsin, "XX9000000002");
    EXPECT_EQ(couponConflict.mSources,
              (std::vector<SourceReference>{
                  {.mBroker = Broker::TradeRepublic,
                   .mFilename = SourceFilename::fromPath("unknown_income.csv"),
                   .mSourceRow = 4,
                   .mTransactionId = "synthetic-optional-coupon",
                   .mInputSequence = {0, 2}},
                  {.mBroker = Broker::TradeRepublic,
                   .mFilename = SourceFilename::fromPath("broker_rate_income.csv"),
                   .mSourceRow = 4,
                   .mTransactionId = "synthetic-optional-coupon",
                   .mInputSequence = {1, 2}}}));
}

TEST(StatementMergerIntegrationTest, RetainsDividendWithoutFxWhileInvalidFxIsReported) {
    const TemporaryIncomeInputs files;
    const auto first = files.write("first.csv",
                                   {{.mDatetime = "2024-01-15T10:00:00.000Z",
                                     .mDate = "2024-01-15",
                                     .mAccountType = "DEFAULT",
                                     .mCategory = "CASH",
                                     .mType = "DIVIDEND",
                                     .mAssetClass = "STOCK",
                                     .mName = "Synthetic Optional Share",
                                     .mSymbol = "XX9000000001",
                                     .mAmount = "9.00",
                                     .mTax = "",
                                     .mCurrency = "EUR",
                                     .mOriginalAmount = "10.00",
                                     .mOriginalCurrency = "USD",
                                     .mFxRate = "",
                                     .mTransactionId = "synthetic-retained-dividend"},
                                    {.mDatetime = "2024-01-15T10:00:00.000Z",
                                     .mDate = "2024-01-15",
                                     .mAccountType = "DEFAULT",
                                     .mCategory = "CASH",
                                     .mType = "DIVIDEND",
                                     .mAssetClass = "STOCK",
                                     .mName = "Synthetic Optional Share",
                                     .mSymbol = "XX9000000001",
                                     .mAmount = "9.00",
                                     .mTax = "",
                                     .mCurrency = "EUR",
                                     .mOriginalAmount = "10.00",
                                     .mOriginalCurrency = "USD",
                                     .mFxRate = "not-a-number",
                                     .mTransactionId = "synthetic-invalid-dividend"}});
    const auto second = files.write("second.csv",
                                    {{.mDatetime = "2024-01-15T10:00:00.000Z",
                                      .mDate = "2024-01-15",
                                      .mAccountType = "DEFAULT",
                                      .mCategory = "CASH",
                                      .mType = "DIVIDEND",
                                      .mAssetClass = "STOCK",
                                      .mName = "Synthetic Optional Share",
                                      .mSymbol = "XX9000000001",
                                      .mAmount = "9.00",
                                      .mTax = "",
                                      .mCurrency = "EUR",
                                      .mOriginalAmount = "10.00",
                                      .mOriginalCurrency = "USD",
                                      .mFxRate = "",
                                      .mTransactionId = "synthetic-retained-dividend"}});
    tr::TradeRepublicParser parser;
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 1, .mParseResult = parser.parse(second, 1)},
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(first, 0)}};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mDividendInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mDividendInstruments.front();

    EXPECT_EQ(instrument.mName, "Synthetic Optional Share");
    EXPECT_EQ(instrument.mIsin, "XX9000000001");
    ASSERT_EQ(instrument.mTransactions.size(), 1U);

    const auto& income = instrument.mTransactions.front();
    const auto date =
        Date{std::chrono::sys_days{std::chrono::year{2024} / 1 / 15}.time_since_epoch()};

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);
    EXPECT_FALSE(income.mTaxPaid.has_value());
    EXPECT_FALSE(income.mExchangeRate.has_value());
    EXPECT_EQ(income.mMetadata.mTaxDate, date);
    EXPECT_EQ(income.mMetadata.mSourceTimestamp,
              SourceTimestamp{date.time_since_epoch() + std::chrono::hours{10}});
    EXPECT_FALSE(income.mMetadata.mOrderingTimestamp.has_value());
    EXPECT_EQ(income.mMetadata.mSources,
              (std::vector<SourceReference>{{.mBroker = Broker::TradeRepublic,
                                             .mFilename = SourceFilename::fromPath("first.csv"),
                                             .mSourceRow = 2,
                                             .mTransactionId = "synthetic-retained-dividend",
                                             .mInputSequence = {0, 0}},
                                            {.mBroker = Broker::TradeRepublic,
                                             .mFilename = SourceFilename::fromPath("second.csv"),
                                             .mSourceRow = 2,
                                             .mTransactionId = "synthetic-retained-dividend",
                                             .mInputSequence = {1, 0}}}));
    EXPECT_EQ(
        result.mStatement.mChronologicalOrder,
        (std::vector<StatementEventReference>{
            {.mKind = StatementEventKind::Dividend, .mInstrumentIndex = 0, .mEventIndex = 0}}));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<SourcedParseDiagnostic>(result.mDiagnostics.front()));

    const auto& sourced = std::get<SourcedParseDiagnostic>(result.mDiagnostics.front());
    const auto& error = sourced.mDiagnostic;

    EXPECT_EQ(sourced.mSourceIndex, 0U);
    EXPECT_EQ(sourced.mBroker, Broker::TradeRepublic);
    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fx_rate");
    EXPECT_EQ(error.mSourceFile, "first.csv");
    EXPECT_EQ(error.mRowIndex, 3U);
    EXPECT_EQ(error.mTransactionId, "synthetic-invalid-dividend");
}

TEST(StatementMergerIntegrationTest, RetainsDepositInterestWithoutFxWhileInvalidFxIsReported) {
    const TemporaryIncomeInputs files;
    const auto first = files.write("first.csv",
                                   {{.mDatetime = "2024-01-15T10:00:00.000Z",
                                     .mDate = "2024-01-15",
                                     .mAccountType = "DEFAULT",
                                     .mCategory = "CASH",
                                     .mType = "INTEREST_PAYMENT",
                                     .mAssetClass = "",
                                     .mName = "",
                                     .mSymbol = "",
                                     .mAmount = "9.00",
                                     .mTax = "",
                                     .mCurrency = "EUR",
                                     .mOriginalAmount = "10.00",
                                     .mOriginalCurrency = "USD",
                                     .mFxRate = "",
                                     .mTransactionId = "synthetic-retained-depositinterest"},
                                    {.mDatetime = "2024-01-15T10:00:00.000Z",
                                     .mDate = "2024-01-15",
                                     .mAccountType = "DEFAULT",
                                     .mCategory = "CASH",
                                     .mType = "INTEREST_PAYMENT",
                                     .mAssetClass = "",
                                     .mName = "",
                                     .mSymbol = "",
                                     .mAmount = "9.00",
                                     .mTax = "",
                                     .mCurrency = "EUR",
                                     .mOriginalAmount = "10.00",
                                     .mOriginalCurrency = "USD",
                                     .mFxRate = "not-a-number",
                                     .mTransactionId = "synthetic-invalid-depositinterest"}});
    const auto second = files.write("second.csv",
                                    {{.mDatetime = "2024-01-15T10:00:00.000Z",
                                      .mDate = "2024-01-15",
                                      .mAccountType = "DEFAULT",
                                      .mCategory = "CASH",
                                      .mType = "INTEREST_PAYMENT",
                                      .mAssetClass = "",
                                      .mName = "",
                                      .mSymbol = "",
                                      .mAmount = "9.00",
                                      .mTax = "",
                                      .mCurrency = "EUR",
                                      .mOriginalAmount = "10.00",
                                      .mOriginalCurrency = "USD",
                                      .mFxRate = "",
                                      .mTransactionId = "synthetic-retained-depositinterest"}});
    tr::TradeRepublicParser parser;
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 1, .mParseResult = parser.parse(second, 1)},
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(first, 0)}};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mInterestInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mInterestInstruments.front();

    EXPECT_EQ(instrument.mName, "Trade Republic");
    EXPECT_EQ(instrument.mInterestType, InterestType::BrokerInterest);
    EXPECT_FALSE(instrument.mIsin.has_value());
    ASSERT_EQ(instrument.mTransactions.size(), 1U);

    const auto& income = instrument.mTransactions.front();
    const auto date =
        Date{std::chrono::sys_days{std::chrono::year{2024} / 1 / 15}.time_since_epoch()};

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);
    EXPECT_FALSE(income.mTaxPaid.has_value());
    EXPECT_FALSE(income.mExchangeRate.has_value());
    EXPECT_EQ(income.mMetadata.mTaxDate, date);
    EXPECT_EQ(income.mMetadata.mSourceTimestamp,
              SourceTimestamp{date.time_since_epoch() + std::chrono::hours{10}});
    EXPECT_FALSE(income.mMetadata.mOrderingTimestamp.has_value());
    EXPECT_EQ(income.mMetadata.mSources,
              (std::vector<SourceReference>{{.mBroker = Broker::TradeRepublic,
                                             .mFilename = SourceFilename::fromPath("first.csv"),
                                             .mSourceRow = 2,
                                             .mTransactionId = "synthetic-retained-depositinterest",
                                             .mInputSequence = {0, 0}},
                                            {.mBroker = Broker::TradeRepublic,
                                             .mFilename = SourceFilename::fromPath("second.csv"),
                                             .mSourceRow = 2,
                                             .mTransactionId = "synthetic-retained-depositinterest",
                                             .mInputSequence = {1, 0}}}));
    EXPECT_EQ(
        result.mStatement.mChronologicalOrder,
        (std::vector<StatementEventReference>{
            {.mKind = StatementEventKind::Interest, .mInstrumentIndex = 0, .mEventIndex = 0}}));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<SourcedParseDiagnostic>(result.mDiagnostics.front()));

    const auto& sourced = std::get<SourcedParseDiagnostic>(result.mDiagnostics.front());
    const auto& error = sourced.mDiagnostic;

    EXPECT_EQ(sourced.mSourceIndex, 0U);
    EXPECT_EQ(sourced.mBroker, Broker::TradeRepublic);
    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fx_rate");
    EXPECT_EQ(error.mSourceFile, "first.csv");
    EXPECT_EQ(error.mRowIndex, 3U);
    EXPECT_EQ(error.mTransactionId, "synthetic-invalid-depositinterest");
}

TEST(StatementMergerIntegrationTest, RetainsBondInterestWithoutFxWhileInvalidFxIsReported) {
    const TemporaryIncomeInputs files;
    const auto first = files.write("first.csv",
                                   {{.mDatetime = "2024-01-15T10:00:00.000Z",
                                     .mDate = "2024-01-15",
                                     .mAccountType = "DEFAULT",
                                     .mCategory = "CASH",
                                     .mType = "INTEREST_PAYMENT",
                                     .mAssetClass = "BOND",
                                     .mName = "Synthetic Optional Bond",
                                     .mSymbol = "XX9000000002",
                                     .mAmount = "9.00",
                                     .mTax = "",
                                     .mCurrency = "EUR",
                                     .mOriginalAmount = "10.00",
                                     .mOriginalCurrency = "USD",
                                     .mFxRate = "",
                                     .mTransactionId = "synthetic-retained-bondinterest"},
                                    {.mDatetime = "2024-01-15T10:00:00.000Z",
                                     .mDate = "2024-01-15",
                                     .mAccountType = "DEFAULT",
                                     .mCategory = "CASH",
                                     .mType = "INTEREST_PAYMENT",
                                     .mAssetClass = "BOND",
                                     .mName = "Synthetic Optional Bond",
                                     .mSymbol = "XX9000000002",
                                     .mAmount = "9.00",
                                     .mTax = "",
                                     .mCurrency = "EUR",
                                     .mOriginalAmount = "10.00",
                                     .mOriginalCurrency = "USD",
                                     .mFxRate = "not-a-number",
                                     .mTransactionId = "synthetic-invalid-bondinterest"}});
    const auto second = files.write("second.csv",
                                    {{.mDatetime = "2024-01-15T10:00:00.000Z",
                                      .mDate = "2024-01-15",
                                      .mAccountType = "DEFAULT",
                                      .mCategory = "CASH",
                                      .mType = "INTEREST_PAYMENT",
                                      .mAssetClass = "BOND",
                                      .mName = "Synthetic Optional Bond",
                                      .mSymbol = "XX9000000002",
                                      .mAmount = "9.00",
                                      .mTax = "",
                                      .mCurrency = "EUR",
                                      .mOriginalAmount = "10.00",
                                      .mOriginalCurrency = "USD",
                                      .mFxRate = "",
                                      .mTransactionId = "synthetic-retained-bondinterest"}});
    tr::TradeRepublicParser parser;
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 1, .mParseResult = parser.parse(second, 1)},
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(first, 0)}};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mInterestInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mInterestInstruments.front();

    EXPECT_EQ(instrument.mName, "Synthetic Optional Bond");
    EXPECT_EQ(instrument.mInterestType, InterestType::BondInterest);
    EXPECT_EQ(instrument.mIsin, "XX9000000002");
    ASSERT_EQ(instrument.mTransactions.size(), 1U);

    const auto& income = instrument.mTransactions.front();
    const auto date =
        Date{std::chrono::sys_days{std::chrono::year{2024} / 1 / 15}.time_since_epoch()};

    EXPECT_EQ(income.mGrossAmount, 100'000);
    EXPECT_EQ(income.mCurrency, Currency::USD);
    EXPECT_EQ(income.mTaxCurrency, Currency::EUR);
    EXPECT_FALSE(income.mTaxPaid.has_value());
    EXPECT_FALSE(income.mExchangeRate.has_value());
    EXPECT_EQ(income.mMetadata.mTaxDate, date);
    EXPECT_EQ(income.mMetadata.mSourceTimestamp,
              SourceTimestamp{date.time_since_epoch() + std::chrono::hours{10}});
    EXPECT_FALSE(income.mMetadata.mOrderingTimestamp.has_value());
    EXPECT_EQ(income.mMetadata.mSources,
              (std::vector<SourceReference>{{.mBroker = Broker::TradeRepublic,
                                             .mFilename = SourceFilename::fromPath("first.csv"),
                                             .mSourceRow = 2,
                                             .mTransactionId = "synthetic-retained-bondinterest",
                                             .mInputSequence = {0, 0}},
                                            {.mBroker = Broker::TradeRepublic,
                                             .mFilename = SourceFilename::fromPath("second.csv"),
                                             .mSourceRow = 2,
                                             .mTransactionId = "synthetic-retained-bondinterest",
                                             .mInputSequence = {1, 0}}}));
    EXPECT_EQ(
        result.mStatement.mChronologicalOrder,
        (std::vector<StatementEventReference>{
            {.mKind = StatementEventKind::Interest, .mInstrumentIndex = 0, .mEventIndex = 0}}));

    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<SourcedParseDiagnostic>(result.mDiagnostics.front()));

    const auto& sourced = std::get<SourcedParseDiagnostic>(result.mDiagnostics.front());
    const auto& error = sourced.mDiagnostic;

    EXPECT_EQ(sourced.mSourceIndex, 0U);
    EXPECT_EQ(sourced.mBroker, Broker::TradeRepublic);
    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mField, "fx_rate");
    EXPECT_EQ(error.mSourceFile, "first.csv");
    EXPECT_EQ(error.mRowIndex, 3U);
    EXPECT_EQ(error.mTransactionId, "synthetic-invalid-bondinterest");
}

TEST(StatementMergerIntegrationTest, RetainsTradeClassConflictInOneFile) {
    const IncomeCsvRow purchaseRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };

    const IncomeCsvRow otherRow{
        .mDatetime = "2024-01-16T10:00:00.000Z",
        .mDate = "2024-01-16",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "SELL",
        .mAssetClass = "FUND",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "-1.000000000",
        .mPrice = "12.0000",
        .mAmount = "12.0000",
        .mFee = "-0.2000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-sale",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto history = files.write("history.csv", {purchaseRow, otherRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(history, 0)}};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Synthetic Evidence Share");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Unknown);
    ASSERT_EQ(instrument.mTransactions.size(), 2U);
    ASSERT_EQ(instrument.mCorporateActions.size(), 0U);
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 2U);

    const auto& purchase = instrument.mTransactions[0];
    const auto& other = instrument.mTransactions[1];

    EXPECT_EQ(purchase.mMetadata.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(purchase.mMetadata.mSourceTimestamp,
              parseSourceTimestamp("2024-01-15T10:00:00.000Z"));
    EXPECT_FALSE(purchase.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(purchase.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(purchase.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(purchase.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(purchase.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mSource, purchase.mMetadata.mSources[0]);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(purchase.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(purchase.mUnits, 200000000);  // Source 2.000000000 units at eight decimal places.
    EXPECT_EQ(purchase.mUnitPrice, 100000); // Source EUR 10.0000 at four decimal places.
    EXPECT_EQ(purchase.mAmount, 200000);
    EXPECT_EQ(purchase.mFeePaid, 1000);
    EXPECT_EQ(purchase.mCurrency, Currency::EUR);
    EXPECT_EQ(purchase.mExchangeRate, 100'000'000);
    ASSERT_EQ(purchase.mUnitEvidence.size(), 1U);
    EXPECT_EQ(purchase.mUnitEvidence[0].mSourceText, "2.000000000");
    EXPECT_EQ(purchase.mUnitEvidence[0].mSource, purchase.mMetadata.mSources[0]);

    EXPECT_EQ(other.mMetadata.mTaxDate, parseCalendarDate("2024-01-16"));
    EXPECT_EQ(other.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-16T10:00:00.000Z"));
    EXPECT_FALSE(other.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(other.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(other.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(other.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(other.mMetadata.mSources[0].mSourceRow, 3U);
    EXPECT_EQ(other.mMetadata.mSources[0].mTransactionId, "synthetic-sale");
    EXPECT_EQ(other.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 1}));
    ASSERT_EQ(other.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(other.mInstrumentEvidence[0].mSource, other.mMetadata.mSources[0]);
    EXPECT_EQ(other.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(other.mInstrumentEvidence[0].mAssetClass, AssetClass::Fund);
    EXPECT_EQ(other.mInstrumentEvidence[0].mAssetClassLabel, "FUND");
    EXPECT_EQ(other.mTradeSide, TradeSide::Sell);
    EXPECT_EQ(other.mUnits, 100000000);  // Source 1.000000000 units at eight decimal places.
    EXPECT_EQ(other.mUnitPrice, 120000); // Source EUR 12.0000 at four decimal places.
    EXPECT_EQ(other.mAmount, 120000);
    EXPECT_EQ(other.mFeePaid, 2000);
    EXPECT_EQ(other.mCurrency, Currency::EUR);
    EXPECT_EQ(other.mExchangeRate, 100'000'000);
    ASSERT_EQ(other.mUnitEvidence.size(), 1U);
    EXPECT_EQ(other.mUnitEvidence[0].mSourceText, "-1.000000000");
    EXPECT_EQ(other.mUnitEvidence[0].mSource, other.mMetadata.mSources[0]);

    const auto conflict = std::find_if(
        result.mDiagnostics.begin(),
        result.mDiagnostics.end(),
        [](const auto& diagnostic) {
            const auto* merge = std::get_if<MergeDiagnostic>(&diagnostic);

            return merge && merge->mCode == MergeDiagnosticCode::InstrumentAssetClassConflict;
        });

    ASSERT_NE(conflict, result.mDiagnostics.end());

    const auto& error = std::get<MergeDiagnostic>(*conflict);

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mIsin, "XX9000000248");
    ASSERT_EQ(error.mAssetClassVariants.size(), 2U);
    EXPECT_EQ(error.mAssetClassVariants[0].mAssetClass, AssetClass::Stock);
    ASSERT_EQ(error.mAssetClassVariants[0].mSources.size(), 1U);
    EXPECT_EQ(error.mAssetClassVariants[1].mAssetClass, AssetClass::Fund);
    ASSERT_EQ(error.mAssetClassVariants[1].mSources.size(), 1U);
    EXPECT_EQ(error.mAssetClassVariants[0].mSources[0], purchase.mMetadata.mSources[0]);
    EXPECT_EQ(error.mAssetClassVariants[1].mSources[0], other.mMetadata.mSources[0]);
}

TEST(StatementMergerIntegrationTest, RetainsTradeClassConflictInTwoFiles) {
    const IncomeCsvRow purchaseRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow otherRow{
        .mDatetime = "2024-01-16T10:00:00.000Z",
        .mDate = "2024-01-16",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "SELL",
        .mAssetClass = "FUND",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "-1.000000000",
        .mPrice = "12.0000",
        .mAmount = "12.0000",
        .mFee = "-0.2000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-sale",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto file0 = files.write("part-0.csv", {purchaseRow});
    const auto file1 = files.write("part-1.csv", {otherRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 1, .mParseResult = parser.parse(file1, 1)},
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(file0, 0)},
    };

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Synthetic Evidence Share");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Unknown);
    ASSERT_EQ(instrument.mTransactions.size(), 2U);
    ASSERT_EQ(instrument.mCorporateActions.size(), 0U);
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 2U);

    const auto& purchase = instrument.mTransactions[0];
    const auto& other = instrument.mTransactions[1];

    EXPECT_EQ(purchase.mMetadata.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(purchase.mMetadata.mSourceTimestamp,
              parseSourceTimestamp("2024-01-15T10:00:00.000Z"));
    EXPECT_FALSE(purchase.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(purchase.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mFilename.value(), "part-0.csv");
    EXPECT_EQ(purchase.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(purchase.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(purchase.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mSource, purchase.mMetadata.mSources[0]);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(purchase.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(purchase.mUnits, 200000000);  // Source 2.000000000 units at eight decimal places.
    EXPECT_EQ(purchase.mUnitPrice, 100000); // Source EUR 10.0000 at four decimal places.
    EXPECT_EQ(purchase.mAmount, 200000);
    EXPECT_EQ(purchase.mFeePaid, 1000);
    EXPECT_EQ(purchase.mCurrency, Currency::EUR);
    EXPECT_EQ(purchase.mExchangeRate, 100'000'000);
    ASSERT_EQ(purchase.mUnitEvidence.size(), 1U);
    EXPECT_EQ(purchase.mUnitEvidence[0].mSourceText, "2.000000000");
    EXPECT_EQ(purchase.mUnitEvidence[0].mSource, purchase.mMetadata.mSources[0]);

    EXPECT_EQ(other.mMetadata.mTaxDate, parseCalendarDate("2024-01-16"));
    EXPECT_EQ(other.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-16T10:00:00.000Z"));
    EXPECT_FALSE(other.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(other.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(other.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(other.mMetadata.mSources[0].mFilename.value(), "part-1.csv");
    EXPECT_EQ(other.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(other.mMetadata.mSources[0].mTransactionId, "synthetic-sale");
    EXPECT_EQ(other.mMetadata.mSources[0].mInputSequence, (StableInputSequence{1, 0}));
    ASSERT_EQ(other.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(other.mInstrumentEvidence[0].mSource, other.mMetadata.mSources[0]);
    EXPECT_EQ(other.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(other.mInstrumentEvidence[0].mAssetClass, AssetClass::Fund);
    EXPECT_EQ(other.mInstrumentEvidence[0].mAssetClassLabel, "FUND");
    EXPECT_EQ(other.mTradeSide, TradeSide::Sell);
    EXPECT_EQ(other.mUnits, 100000000);  // Source 1.000000000 units at eight decimal places.
    EXPECT_EQ(other.mUnitPrice, 120000); // Source EUR 12.0000 at four decimal places.
    EXPECT_EQ(other.mAmount, 120000);
    EXPECT_EQ(other.mFeePaid, 2000);
    EXPECT_EQ(other.mCurrency, Currency::EUR);
    EXPECT_EQ(other.mExchangeRate, 100'000'000);
    ASSERT_EQ(other.mUnitEvidence.size(), 1U);
    EXPECT_EQ(other.mUnitEvidence[0].mSourceText, "-1.000000000");
    EXPECT_EQ(other.mUnitEvidence[0].mSource, other.mMetadata.mSources[0]);

    const auto conflict = std::find_if(
        result.mDiagnostics.begin(),
        result.mDiagnostics.end(),
        [](const auto& diagnostic) {
            const auto* merge = std::get_if<MergeDiagnostic>(&diagnostic);

            return merge && merge->mCode == MergeDiagnosticCode::InstrumentAssetClassConflict;
        });

    ASSERT_NE(conflict, result.mDiagnostics.end());

    const auto& error = std::get<MergeDiagnostic>(*conflict);

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mIsin, "XX9000000248");
    ASSERT_EQ(error.mAssetClassVariants.size(), 2U);
    EXPECT_EQ(error.mAssetClassVariants[0].mAssetClass, AssetClass::Stock);
    ASSERT_EQ(error.mAssetClassVariants[0].mSources.size(), 1U);
    EXPECT_EQ(error.mAssetClassVariants[1].mAssetClass, AssetClass::Fund);
    ASSERT_EQ(error.mAssetClassVariants[1].mSources.size(), 1U);
    EXPECT_EQ(error.mAssetClassVariants[0].mSources[0], purchase.mMetadata.mSources[0]);
    EXPECT_EQ(error.mAssetClassVariants[1].mSources[0], other.mMetadata.mSources[0]);
}

TEST(StatementMergerIntegrationTest, RetainsTradeClassConflictInReversedRows) {
    const IncomeCsvRow purchaseRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow otherRow{
        .mDatetime = "2024-01-16T10:00:00.000Z",
        .mDate = "2024-01-16",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "SELL",
        .mAssetClass = "FUND",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "-1.000000000",
        .mPrice = "12.0000",
        .mAmount = "12.0000",
        .mFee = "-0.2000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-sale",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto history = files.write("history.csv", {otherRow, purchaseRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(history, 0)}};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Synthetic Evidence Share");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Unknown);
    ASSERT_EQ(instrument.mTransactions.size(), 2U);
    ASSERT_EQ(instrument.mCorporateActions.size(), 0U);
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 2U);

    const auto& purchase = instrument.mTransactions[0];
    const auto& other = instrument.mTransactions[1];

    EXPECT_EQ(purchase.mMetadata.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(purchase.mMetadata.mSourceTimestamp,
              parseSourceTimestamp("2024-01-15T10:00:00.000Z"));
    EXPECT_FALSE(purchase.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(purchase.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(purchase.mMetadata.mSources[0].mSourceRow, 3U);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(purchase.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 1}));
    ASSERT_EQ(purchase.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mSource, purchase.mMetadata.mSources[0]);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(purchase.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(purchase.mUnits, 200000000);  // Source 2.000000000 units at eight decimal places.
    EXPECT_EQ(purchase.mUnitPrice, 100000); // Source EUR 10.0000 at four decimal places.
    EXPECT_EQ(purchase.mAmount, 200000);
    EXPECT_EQ(purchase.mFeePaid, 1000);
    EXPECT_EQ(purchase.mCurrency, Currency::EUR);
    EXPECT_EQ(purchase.mExchangeRate, 100'000'000);
    ASSERT_EQ(purchase.mUnitEvidence.size(), 1U);
    EXPECT_EQ(purchase.mUnitEvidence[0].mSourceText, "2.000000000");
    EXPECT_EQ(purchase.mUnitEvidence[0].mSource, purchase.mMetadata.mSources[0]);

    EXPECT_EQ(other.mMetadata.mTaxDate, parseCalendarDate("2024-01-16"));
    EXPECT_EQ(other.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-16T10:00:00.000Z"));
    EXPECT_FALSE(other.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(other.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(other.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(other.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(other.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(other.mMetadata.mSources[0].mTransactionId, "synthetic-sale");
    EXPECT_EQ(other.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(other.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(other.mInstrumentEvidence[0].mSource, other.mMetadata.mSources[0]);
    EXPECT_EQ(other.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(other.mInstrumentEvidence[0].mAssetClass, AssetClass::Fund);
    EXPECT_EQ(other.mInstrumentEvidence[0].mAssetClassLabel, "FUND");
    EXPECT_EQ(other.mTradeSide, TradeSide::Sell);
    EXPECT_EQ(other.mUnits, 100000000);  // Source 1.000000000 units at eight decimal places.
    EXPECT_EQ(other.mUnitPrice, 120000); // Source EUR 12.0000 at four decimal places.
    EXPECT_EQ(other.mAmount, 120000);
    EXPECT_EQ(other.mFeePaid, 2000);
    EXPECT_EQ(other.mCurrency, Currency::EUR);
    EXPECT_EQ(other.mExchangeRate, 100'000'000);
    ASSERT_EQ(other.mUnitEvidence.size(), 1U);
    EXPECT_EQ(other.mUnitEvidence[0].mSourceText, "-1.000000000");
    EXPECT_EQ(other.mUnitEvidence[0].mSource, other.mMetadata.mSources[0]);

    const auto conflict = std::find_if(
        result.mDiagnostics.begin(),
        result.mDiagnostics.end(),
        [](const auto& diagnostic) {
            const auto* merge = std::get_if<MergeDiagnostic>(&diagnostic);

            return merge && merge->mCode == MergeDiagnosticCode::InstrumentAssetClassConflict;
        });

    ASSERT_NE(conflict, result.mDiagnostics.end());

    const auto& error = std::get<MergeDiagnostic>(*conflict);

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mIsin, "XX9000000248");
    ASSERT_EQ(error.mAssetClassVariants.size(), 2U);
    EXPECT_EQ(error.mAssetClassVariants[0].mAssetClass, AssetClass::Stock);
    ASSERT_EQ(error.mAssetClassVariants[0].mSources.size(), 1U);
    EXPECT_EQ(error.mAssetClassVariants[1].mAssetClass, AssetClass::Fund);
    ASSERT_EQ(error.mAssetClassVariants[1].mSources.size(), 1U);
    EXPECT_EQ(error.mAssetClassVariants[0].mSources[0], purchase.mMetadata.mSources[0]);
    EXPECT_EQ(error.mAssetClassVariants[1].mSources[0], other.mMetadata.mSources[0]);
}

TEST(StatementMergerIntegrationTest, RetainsActionClassConflictInOneFile) {
    const IncomeCsvRow purchaseRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow otherRow{
        .mDatetime = "2024-01-16T10:00:00.000Z",
        .mDate = "2024-01-16",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "FUND",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "4.000000000",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-action",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto history = files.write("history.csv", {purchaseRow, otherRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(history, 0)}};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Synthetic Evidence Share");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Unknown);
    ASSERT_EQ(instrument.mTransactions.size(), 1U);
    ASSERT_EQ(instrument.mCorporateActions.size(), 1U);
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 2U);

    const auto& purchase = instrument.mTransactions[0];
    const auto& other = instrument.mCorporateActions[0];

    EXPECT_EQ(purchase.mMetadata.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(purchase.mMetadata.mSourceTimestamp,
              parseSourceTimestamp("2024-01-15T10:00:00.000Z"));
    EXPECT_FALSE(purchase.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(purchase.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(purchase.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(purchase.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(purchase.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mSource, purchase.mMetadata.mSources[0]);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(purchase.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(purchase.mUnits, 200000000);  // Source 2.000000000 units at eight decimal places.
    EXPECT_EQ(purchase.mUnitPrice, 100000); // Source EUR 10.0000 at four decimal places.
    EXPECT_EQ(purchase.mAmount, 200000);
    EXPECT_EQ(purchase.mFeePaid, 1000);
    EXPECT_EQ(purchase.mCurrency, Currency::EUR);
    EXPECT_EQ(purchase.mExchangeRate, 100'000'000);
    ASSERT_EQ(purchase.mUnitEvidence.size(), 1U);
    EXPECT_EQ(purchase.mUnitEvidence[0].mSourceText, "2.000000000");
    EXPECT_EQ(purchase.mUnitEvidence[0].mSource, purchase.mMetadata.mSources[0]);

    EXPECT_EQ(other.mMetadata.mTaxDate, parseCalendarDate("2024-01-16"));
    EXPECT_EQ(other.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-16T10:00:00.000Z"));
    EXPECT_FALSE(other.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(other.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(other.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(other.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(other.mMetadata.mSources[0].mSourceRow, 3U);
    EXPECT_EQ(other.mMetadata.mSources[0].mTransactionId, "synthetic-action");
    EXPECT_EQ(other.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 1}));
    ASSERT_EQ(other.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(other.mInstrumentEvidence[0].mSource, other.mMetadata.mSources[0]);
    EXPECT_EQ(other.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(other.mInstrumentEvidence[0].mAssetClass, AssetClass::Fund);
    EXPECT_EQ(other.mInstrumentEvidence[0].mAssetClassLabel, "FUND");
    EXPECT_EQ(other.mType, CorporateActionType::UnresolvedSplit);
    EXPECT_EQ(other.mUnitsDelta, 400'000'000); // Source 4 units, not a verified split change.
    EXPECT_FALSE(other.mRatio.has_value());
    ASSERT_EQ(other.mUnitEvidence.size(), 1U);
    EXPECT_EQ(other.mUnitEvidence[0].mSourceText, "4.000000000");
    EXPECT_EQ(other.mUnitEvidence[0].mSource, other.mMetadata.mSources[0]);

    const auto conflict = std::find_if(
        result.mDiagnostics.begin(),
        result.mDiagnostics.end(),
        [](const auto& diagnostic) {
            const auto* merge = std::get_if<MergeDiagnostic>(&diagnostic);

            return merge && merge->mCode == MergeDiagnosticCode::InstrumentAssetClassConflict;
        });

    ASSERT_NE(conflict, result.mDiagnostics.end());

    const auto& error = std::get<MergeDiagnostic>(*conflict);

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mIsin, "XX9000000248");
    ASSERT_EQ(error.mAssetClassVariants.size(), 2U);
    EXPECT_EQ(error.mAssetClassVariants[0].mAssetClass, AssetClass::Stock);
    ASSERT_EQ(error.mAssetClassVariants[0].mSources.size(), 1U);
    EXPECT_EQ(error.mAssetClassVariants[1].mAssetClass, AssetClass::Fund);
    ASSERT_EQ(error.mAssetClassVariants[1].mSources.size(), 1U);
    EXPECT_EQ(error.mAssetClassVariants[0].mSources[0], purchase.mMetadata.mSources[0]);
    EXPECT_EQ(error.mAssetClassVariants[1].mSources[0], other.mMetadata.mSources[0]);
}

TEST(StatementMergerIntegrationTest, RetainsActionClassConflictInTwoFiles) {
    const IncomeCsvRow purchaseRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow otherRow{
        .mDatetime = "2024-01-16T10:00:00.000Z",
        .mDate = "2024-01-16",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "FUND",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "4.000000000",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-action",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto file0 = files.write("part-0.csv", {purchaseRow});
    const auto file1 = files.write("part-1.csv", {otherRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 1, .mParseResult = parser.parse(file1, 1)},
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(file0, 0)},
    };

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Synthetic Evidence Share");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Unknown);
    ASSERT_EQ(instrument.mTransactions.size(), 1U);
    ASSERT_EQ(instrument.mCorporateActions.size(), 1U);
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 2U);

    const auto& purchase = instrument.mTransactions[0];
    const auto& other = instrument.mCorporateActions[0];

    EXPECT_EQ(purchase.mMetadata.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(purchase.mMetadata.mSourceTimestamp,
              parseSourceTimestamp("2024-01-15T10:00:00.000Z"));
    EXPECT_FALSE(purchase.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(purchase.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mFilename.value(), "part-0.csv");
    EXPECT_EQ(purchase.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(purchase.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(purchase.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mSource, purchase.mMetadata.mSources[0]);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(purchase.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(purchase.mUnits, 200000000);  // Source 2.000000000 units at eight decimal places.
    EXPECT_EQ(purchase.mUnitPrice, 100000); // Source EUR 10.0000 at four decimal places.
    EXPECT_EQ(purchase.mAmount, 200000);
    EXPECT_EQ(purchase.mFeePaid, 1000);
    EXPECT_EQ(purchase.mCurrency, Currency::EUR);
    EXPECT_EQ(purchase.mExchangeRate, 100'000'000);
    ASSERT_EQ(purchase.mUnitEvidence.size(), 1U);
    EXPECT_EQ(purchase.mUnitEvidence[0].mSourceText, "2.000000000");
    EXPECT_EQ(purchase.mUnitEvidence[0].mSource, purchase.mMetadata.mSources[0]);

    EXPECT_EQ(other.mMetadata.mTaxDate, parseCalendarDate("2024-01-16"));
    EXPECT_EQ(other.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-16T10:00:00.000Z"));
    EXPECT_FALSE(other.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(other.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(other.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(other.mMetadata.mSources[0].mFilename.value(), "part-1.csv");
    EXPECT_EQ(other.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(other.mMetadata.mSources[0].mTransactionId, "synthetic-action");
    EXPECT_EQ(other.mMetadata.mSources[0].mInputSequence, (StableInputSequence{1, 0}));
    ASSERT_EQ(other.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(other.mInstrumentEvidence[0].mSource, other.mMetadata.mSources[0]);
    EXPECT_EQ(other.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(other.mInstrumentEvidence[0].mAssetClass, AssetClass::Fund);
    EXPECT_EQ(other.mInstrumentEvidence[0].mAssetClassLabel, "FUND");
    EXPECT_EQ(other.mType, CorporateActionType::UnresolvedSplit);
    EXPECT_EQ(other.mUnitsDelta, 400'000'000); // Source 4 units, not a verified split change.
    EXPECT_FALSE(other.mRatio.has_value());
    ASSERT_EQ(other.mUnitEvidence.size(), 1U);
    EXPECT_EQ(other.mUnitEvidence[0].mSourceText, "4.000000000");
    EXPECT_EQ(other.mUnitEvidence[0].mSource, other.mMetadata.mSources[0]);

    const auto conflict = std::find_if(
        result.mDiagnostics.begin(),
        result.mDiagnostics.end(),
        [](const auto& diagnostic) {
            const auto* merge = std::get_if<MergeDiagnostic>(&diagnostic);

            return merge && merge->mCode == MergeDiagnosticCode::InstrumentAssetClassConflict;
        });

    ASSERT_NE(conflict, result.mDiagnostics.end());

    const auto& error = std::get<MergeDiagnostic>(*conflict);

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mIsin, "XX9000000248");
    ASSERT_EQ(error.mAssetClassVariants.size(), 2U);
    EXPECT_EQ(error.mAssetClassVariants[0].mAssetClass, AssetClass::Stock);
    ASSERT_EQ(error.mAssetClassVariants[0].mSources.size(), 1U);
    EXPECT_EQ(error.mAssetClassVariants[1].mAssetClass, AssetClass::Fund);
    ASSERT_EQ(error.mAssetClassVariants[1].mSources.size(), 1U);
    EXPECT_EQ(error.mAssetClassVariants[0].mSources[0], purchase.mMetadata.mSources[0]);
    EXPECT_EQ(error.mAssetClassVariants[1].mSources[0], other.mMetadata.mSources[0]);
}

TEST(StatementMergerIntegrationTest, RetainsActionClassConflictInReversedRows) {
    const IncomeCsvRow purchaseRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow otherRow{
        .mDatetime = "2024-01-16T10:00:00.000Z",
        .mDate = "2024-01-16",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "FUND",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "4.000000000",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-action",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto history = files.write("history.csv", {otherRow, purchaseRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(history, 0)}};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Synthetic Evidence Share");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Unknown);
    ASSERT_EQ(instrument.mTransactions.size(), 1U);
    ASSERT_EQ(instrument.mCorporateActions.size(), 1U);
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 2U);

    const auto& purchase = instrument.mTransactions[0];
    const auto& other = instrument.mCorporateActions[0];

    EXPECT_EQ(purchase.mMetadata.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(purchase.mMetadata.mSourceTimestamp,
              parseSourceTimestamp("2024-01-15T10:00:00.000Z"));
    EXPECT_FALSE(purchase.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(purchase.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(purchase.mMetadata.mSources[0].mSourceRow, 3U);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(purchase.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 1}));
    ASSERT_EQ(purchase.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mSource, purchase.mMetadata.mSources[0]);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(purchase.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(purchase.mUnits, 200000000);  // Source 2.000000000 units at eight decimal places.
    EXPECT_EQ(purchase.mUnitPrice, 100000); // Source EUR 10.0000 at four decimal places.
    EXPECT_EQ(purchase.mAmount, 200000);
    EXPECT_EQ(purchase.mFeePaid, 1000);
    EXPECT_EQ(purchase.mCurrency, Currency::EUR);
    EXPECT_EQ(purchase.mExchangeRate, 100'000'000);
    ASSERT_EQ(purchase.mUnitEvidence.size(), 1U);
    EXPECT_EQ(purchase.mUnitEvidence[0].mSourceText, "2.000000000");
    EXPECT_EQ(purchase.mUnitEvidence[0].mSource, purchase.mMetadata.mSources[0]);

    EXPECT_EQ(other.mMetadata.mTaxDate, parseCalendarDate("2024-01-16"));
    EXPECT_EQ(other.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-16T10:00:00.000Z"));
    EXPECT_FALSE(other.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(other.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(other.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(other.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(other.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(other.mMetadata.mSources[0].mTransactionId, "synthetic-action");
    EXPECT_EQ(other.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(other.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(other.mInstrumentEvidence[0].mSource, other.mMetadata.mSources[0]);
    EXPECT_EQ(other.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(other.mInstrumentEvidence[0].mAssetClass, AssetClass::Fund);
    EXPECT_EQ(other.mInstrumentEvidence[0].mAssetClassLabel, "FUND");
    EXPECT_EQ(other.mType, CorporateActionType::UnresolvedSplit);
    EXPECT_EQ(other.mUnitsDelta, 400'000'000); // Source 4 units, not a verified split change.
    EXPECT_FALSE(other.mRatio.has_value());
    ASSERT_EQ(other.mUnitEvidence.size(), 1U);
    EXPECT_EQ(other.mUnitEvidence[0].mSourceText, "4.000000000");
    EXPECT_EQ(other.mUnitEvidence[0].mSource, other.mMetadata.mSources[0]);

    const auto conflict = std::find_if(
        result.mDiagnostics.begin(),
        result.mDiagnostics.end(),
        [](const auto& diagnostic) {
            const auto* merge = std::get_if<MergeDiagnostic>(&diagnostic);

            return merge && merge->mCode == MergeDiagnosticCode::InstrumentAssetClassConflict;
        });

    ASSERT_NE(conflict, result.mDiagnostics.end());

    const auto& error = std::get<MergeDiagnostic>(*conflict);

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mIsin, "XX9000000248");
    ASSERT_EQ(error.mAssetClassVariants.size(), 2U);
    EXPECT_EQ(error.mAssetClassVariants[0].mAssetClass, AssetClass::Stock);
    ASSERT_EQ(error.mAssetClassVariants[0].mSources.size(), 1U);
    EXPECT_EQ(error.mAssetClassVariants[1].mAssetClass, AssetClass::Fund);
    ASSERT_EQ(error.mAssetClassVariants[1].mSources.size(), 1U);
    EXPECT_EQ(error.mAssetClassVariants[0].mSources[0], purchase.mMetadata.mSources[0]);
    EXPECT_EQ(error.mAssetClassVariants[1].mSources[0], other.mMetadata.mSources[0]);
}

TEST(StatementMergerIntegrationTest, LaterStockDoesNotClearEarlierFundConflict) {
    const IncomeCsvRow purchaseRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow saleRow{
        .mDatetime = "2024-01-16T10:00:00.000Z",
        .mDate = "2024-01-16",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "SELL",
        .mAssetClass = "FUND",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "-1.000000000",
        .mPrice = "12.0000",
        .mAmount = "12.0000",
        .mFee = "-0.2000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-sale",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow thirdRow{
        .mDatetime = "2024-01-17T10:00:00.000Z",
        .mDate = "2024-01-17",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "3.000000000",
        .mPrice = "8.0000",
        .mAmount = "-24.0000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-third",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto history = files.write("history.csv", {purchaseRow, saleRow, thirdRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(history, 0)}};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Synthetic Evidence Share");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Unknown);
    ASSERT_EQ(instrument.mTransactions.size(), 3U);
    ASSERT_EQ(instrument.mCorporateActions.size(), 0U);
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 3U);

    const auto& purchase = instrument.mTransactions[0];
    const auto& sale = instrument.mTransactions[1];
    const auto& laterPurchase = instrument.mTransactions[2];

    EXPECT_EQ(purchase.mMetadata.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(purchase.mMetadata.mSourceTimestamp,
              parseSourceTimestamp("2024-01-15T10:00:00.000Z"));
    EXPECT_FALSE(purchase.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(purchase.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(purchase.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(purchase.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(purchase.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mSource, purchase.mMetadata.mSources[0]);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(purchase.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(purchase.mUnits, 200000000);  // Source 2.000000000 units at eight decimal places.
    EXPECT_EQ(purchase.mUnitPrice, 100000); // Source EUR 10.0000 at four decimal places.
    EXPECT_EQ(purchase.mAmount, 200000);
    EXPECT_EQ(purchase.mFeePaid, 1000);
    EXPECT_EQ(purchase.mCurrency, Currency::EUR);
    EXPECT_EQ(purchase.mExchangeRate, 100'000'000);
    ASSERT_EQ(purchase.mUnitEvidence.size(), 1U);
    EXPECT_EQ(purchase.mUnitEvidence[0].mSourceText, "2.000000000");
    EXPECT_EQ(purchase.mUnitEvidence[0].mSource, purchase.mMetadata.mSources[0]);

    EXPECT_EQ(sale.mMetadata.mTaxDate, parseCalendarDate("2024-01-16"));
    EXPECT_EQ(sale.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-16T10:00:00.000Z"));
    EXPECT_FALSE(sale.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(sale.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(sale.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(sale.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(sale.mMetadata.mSources[0].mSourceRow, 3U);
    EXPECT_EQ(sale.mMetadata.mSources[0].mTransactionId, "synthetic-sale");
    EXPECT_EQ(sale.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 1}));
    ASSERT_EQ(sale.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(sale.mInstrumentEvidence[0].mSource, sale.mMetadata.mSources[0]);
    EXPECT_EQ(sale.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(sale.mInstrumentEvidence[0].mAssetClass, AssetClass::Fund);
    EXPECT_EQ(sale.mInstrumentEvidence[0].mAssetClassLabel, "FUND");
    EXPECT_EQ(sale.mTradeSide, TradeSide::Sell);
    EXPECT_EQ(sale.mUnits, 100000000);  // Source 1.000000000 units at eight decimal places.
    EXPECT_EQ(sale.mUnitPrice, 120000); // Source EUR 12.0000 at four decimal places.
    EXPECT_EQ(sale.mAmount, 120000);
    EXPECT_EQ(sale.mFeePaid, 2000);
    EXPECT_EQ(sale.mCurrency, Currency::EUR);
    EXPECT_EQ(sale.mExchangeRate, 100'000'000);
    ASSERT_EQ(sale.mUnitEvidence.size(), 1U);
    EXPECT_EQ(sale.mUnitEvidence[0].mSourceText, "-1.000000000");
    EXPECT_EQ(sale.mUnitEvidence[0].mSource, sale.mMetadata.mSources[0]);

    EXPECT_EQ(laterPurchase.mMetadata.mTaxDate, parseCalendarDate("2024-01-17"));
    EXPECT_EQ(laterPurchase.mMetadata.mSourceTimestamp,
              parseSourceTimestamp("2024-01-17T10:00:00.000Z"));
    EXPECT_FALSE(laterPurchase.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(laterPurchase.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(laterPurchase.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(laterPurchase.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(laterPurchase.mMetadata.mSources[0].mSourceRow, 4U);
    EXPECT_EQ(laterPurchase.mMetadata.mSources[0].mTransactionId, "synthetic-third");
    EXPECT_EQ(laterPurchase.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 2}));
    ASSERT_EQ(laterPurchase.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(laterPurchase.mInstrumentEvidence[0].mSource, laterPurchase.mMetadata.mSources[0]);
    EXPECT_EQ(laterPurchase.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(laterPurchase.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(laterPurchase.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(laterPurchase.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(laterPurchase.mUnits, 300000000); // Source 3.000000000 units at eight decimal places.
    EXPECT_EQ(laterPurchase.mUnitPrice, 80000); // Source EUR 8.0000 at four decimal places.
    EXPECT_EQ(laterPurchase.mAmount, 240000);
    EXPECT_EQ(laterPurchase.mFeePaid, 0);
    EXPECT_EQ(laterPurchase.mCurrency, Currency::EUR);
    EXPECT_EQ(laterPurchase.mExchangeRate, 100'000'000);
    ASSERT_EQ(laterPurchase.mUnitEvidence.size(), 1U);
    EXPECT_EQ(laterPurchase.mUnitEvidence[0].mSourceText, "3.000000000");
    EXPECT_EQ(laterPurchase.mUnitEvidence[0].mSource, laterPurchase.mMetadata.mSources[0]);

    const auto conflict = std::find_if(
        result.mDiagnostics.begin(),
        result.mDiagnostics.end(),
        [](const auto& diagnostic) {
            const auto* merge = std::get_if<MergeDiagnostic>(&diagnostic);

            return merge && merge->mCode == MergeDiagnosticCode::InstrumentAssetClassConflict;
        });

    ASSERT_NE(conflict, result.mDiagnostics.end());

    const auto& error = std::get<MergeDiagnostic>(*conflict);

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mIsin, "XX9000000248");
    ASSERT_EQ(error.mAssetClassVariants.size(), 2U);
    EXPECT_EQ(error.mAssetClassVariants[0].mAssetClass, AssetClass::Stock);
    ASSERT_EQ(error.mAssetClassVariants[0].mSources.size(), 2U);
    EXPECT_EQ(error.mAssetClassVariants[1].mAssetClass, AssetClass::Fund);
    ASSERT_EQ(error.mAssetClassVariants[1].mSources.size(), 1U);
    EXPECT_EQ(error.mAssetClassVariants[0].mSources[0], purchase.mMetadata.mSources[0]);
    EXPECT_EQ(error.mAssetClassVariants[0].mSources[1], laterPurchase.mMetadata.mSources[0]);
    EXPECT_EQ(error.mAssetClassVariants[1].mSources[0], sale.mMetadata.mSources[0]);
}

TEST(StatementMergerIntegrationTest, MissingClassStaysUnresolvedWithStockLast) {
    const IncomeCsvRow unknownRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow knownRow{
        .mDatetime = "2024-01-16T10:00:00.000Z",
        .mDate = "2024-01-16",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "SELL",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "-1.000000000",
        .mPrice = "12.0000",
        .mAmount = "12.0000",
        .mFee = "-0.2000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-sale",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto history = files.write("history.csv", {unknownRow, knownRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(history, 0)}};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Synthetic Evidence Share");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Unknown);
    ASSERT_EQ(instrument.mTransactions.size(), 2U);
    ASSERT_EQ(instrument.mCorporateActions.size(), 0U);
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 2U);

    const auto& unknown = instrument.mTransactions[0];
    const auto& known = instrument.mTransactions[1];

    EXPECT_EQ(unknown.mMetadata.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(unknown.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-15T10:00:00.000Z"));
    EXPECT_FALSE(unknown.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(unknown.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(unknown.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(unknown.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(unknown.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(unknown.mMetadata.mSources[0].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(unknown.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(unknown.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(unknown.mInstrumentEvidence[0].mSource, unknown.mMetadata.mSources[0]);
    EXPECT_EQ(unknown.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(unknown.mInstrumentEvidence[0].mAssetClass, AssetClass::Unknown);
    EXPECT_EQ(unknown.mInstrumentEvidence[0].mAssetClassLabel, "");
    EXPECT_EQ(unknown.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(unknown.mUnits, 200000000);  // Source 2.000000000 units at eight decimal places.
    EXPECT_EQ(unknown.mUnitPrice, 100000); // Source EUR 10.0000 at four decimal places.
    EXPECT_EQ(unknown.mAmount, 200000);
    EXPECT_EQ(unknown.mFeePaid, 1000);
    EXPECT_EQ(unknown.mCurrency, Currency::EUR);
    EXPECT_EQ(unknown.mExchangeRate, 100'000'000);
    ASSERT_EQ(unknown.mUnitEvidence.size(), 1U);
    EXPECT_EQ(unknown.mUnitEvidence[0].mSourceText, "2.000000000");
    EXPECT_EQ(unknown.mUnitEvidence[0].mSource, unknown.mMetadata.mSources[0]);

    EXPECT_EQ(known.mMetadata.mTaxDate, parseCalendarDate("2024-01-16"));
    EXPECT_EQ(known.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-16T10:00:00.000Z"));
    EXPECT_FALSE(known.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(known.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(known.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(known.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(known.mMetadata.mSources[0].mSourceRow, 3U);
    EXPECT_EQ(known.mMetadata.mSources[0].mTransactionId, "synthetic-sale");
    EXPECT_EQ(known.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 1}));
    ASSERT_EQ(known.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(known.mInstrumentEvidence[0].mSource, known.mMetadata.mSources[0]);
    EXPECT_EQ(known.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(known.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(known.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(known.mTradeSide, TradeSide::Sell);
    EXPECT_EQ(known.mUnits, 100000000);  // Source 1.000000000 units at eight decimal places.
    EXPECT_EQ(known.mUnitPrice, 120000); // Source EUR 12.0000 at four decimal places.
    EXPECT_EQ(known.mAmount, 120000);
    EXPECT_EQ(known.mFeePaid, 2000);
    EXPECT_EQ(known.mCurrency, Currency::EUR);
    EXPECT_EQ(known.mExchangeRate, 100'000'000);
    ASSERT_EQ(known.mUnitEvidence.size(), 1U);
    EXPECT_EQ(known.mUnitEvidence[0].mSourceText, "-1.000000000");
    EXPECT_EQ(known.mUnitEvidence[0].mSource, known.mMetadata.mSources[0]);

    const auto conflict = std::find_if(
        result.mDiagnostics.begin(),
        result.mDiagnostics.end(),
        [](const auto& diagnostic) {
            const auto* merge = std::get_if<MergeDiagnostic>(&diagnostic);

            return merge && merge->mCode == MergeDiagnosticCode::InstrumentAssetClassConflict;
        });

    ASSERT_NE(conflict, result.mDiagnostics.end());

    const auto& error = std::get<MergeDiagnostic>(*conflict);

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mIsin, "XX9000000248");
    ASSERT_EQ(error.mAssetClassVariants.size(), 2U);
    EXPECT_EQ(error.mAssetClassVariants[0].mAssetClass, AssetClass::Stock);
    ASSERT_EQ(error.mAssetClassVariants[0].mSources.size(), 1U);
    EXPECT_EQ(error.mAssetClassVariants[1].mAssetClass, AssetClass::Unknown);
    ASSERT_EQ(error.mAssetClassVariants[1].mSources.size(), 1U);
    EXPECT_EQ(error.mAssetClassVariants[0].mSources[0], known.mMetadata.mSources[0]);
    EXPECT_EQ(error.mAssetClassVariants[1].mSources[0], unknown.mMetadata.mSources[0]);
    EXPECT_EQ(error.mAssetClassVariants[1].mUnrecognizedLabel, "");
    ASSERT_EQ(result.mDiagnostics.size(), 2U);

    const auto& parsed = std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]);

    EXPECT_EQ(parsed.mDiagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(parsed.mDiagnostic.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(parsed.mDiagnostic.mField, "asset_class");
    EXPECT_EQ(parsed.mDiagnostic.mRowIndex, 2U);
    EXPECT_EQ(parsed.mDiagnostic.mTransactionId, "synthetic-purchase");
}

TEST(StatementMergerIntegrationTest, MissingClassStaysUnresolvedWithStockFirst) {
    const IncomeCsvRow unknownRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow knownRow{
        .mDatetime = "2024-01-16T10:00:00.000Z",
        .mDate = "2024-01-16",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "SELL",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "-1.000000000",
        .mPrice = "12.0000",
        .mAmount = "12.0000",
        .mFee = "-0.2000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-sale",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto history = files.write("history.csv", {knownRow, unknownRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(history, 0)}};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Synthetic Evidence Share");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Unknown);
    ASSERT_EQ(instrument.mTransactions.size(), 2U);
    ASSERT_EQ(instrument.mCorporateActions.size(), 0U);
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 2U);

    const auto& unknown = instrument.mTransactions[0];
    const auto& known = instrument.mTransactions[1];

    EXPECT_EQ(unknown.mMetadata.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(unknown.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-15T10:00:00.000Z"));
    EXPECT_FALSE(unknown.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(unknown.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(unknown.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(unknown.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(unknown.mMetadata.mSources[0].mSourceRow, 3U);
    EXPECT_EQ(unknown.mMetadata.mSources[0].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(unknown.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 1}));
    ASSERT_EQ(unknown.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(unknown.mInstrumentEvidence[0].mSource, unknown.mMetadata.mSources[0]);
    EXPECT_EQ(unknown.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(unknown.mInstrumentEvidence[0].mAssetClass, AssetClass::Unknown);
    EXPECT_EQ(unknown.mInstrumentEvidence[0].mAssetClassLabel, "");
    EXPECT_EQ(unknown.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(unknown.mUnits, 200000000);  // Source 2.000000000 units at eight decimal places.
    EXPECT_EQ(unknown.mUnitPrice, 100000); // Source EUR 10.0000 at four decimal places.
    EXPECT_EQ(unknown.mAmount, 200000);
    EXPECT_EQ(unknown.mFeePaid, 1000);
    EXPECT_EQ(unknown.mCurrency, Currency::EUR);
    EXPECT_EQ(unknown.mExchangeRate, 100'000'000);
    ASSERT_EQ(unknown.mUnitEvidence.size(), 1U);
    EXPECT_EQ(unknown.mUnitEvidence[0].mSourceText, "2.000000000");
    EXPECT_EQ(unknown.mUnitEvidence[0].mSource, unknown.mMetadata.mSources[0]);

    EXPECT_EQ(known.mMetadata.mTaxDate, parseCalendarDate("2024-01-16"));
    EXPECT_EQ(known.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-16T10:00:00.000Z"));
    EXPECT_FALSE(known.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(known.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(known.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(known.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(known.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(known.mMetadata.mSources[0].mTransactionId, "synthetic-sale");
    EXPECT_EQ(known.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(known.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(known.mInstrumentEvidence[0].mSource, known.mMetadata.mSources[0]);
    EXPECT_EQ(known.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(known.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(known.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(known.mTradeSide, TradeSide::Sell);
    EXPECT_EQ(known.mUnits, 100000000);  // Source 1.000000000 units at eight decimal places.
    EXPECT_EQ(known.mUnitPrice, 120000); // Source EUR 12.0000 at four decimal places.
    EXPECT_EQ(known.mAmount, 120000);
    EXPECT_EQ(known.mFeePaid, 2000);
    EXPECT_EQ(known.mCurrency, Currency::EUR);
    EXPECT_EQ(known.mExchangeRate, 100'000'000);
    ASSERT_EQ(known.mUnitEvidence.size(), 1U);
    EXPECT_EQ(known.mUnitEvidence[0].mSourceText, "-1.000000000");
    EXPECT_EQ(known.mUnitEvidence[0].mSource, known.mMetadata.mSources[0]);

    const auto conflict = std::find_if(
        result.mDiagnostics.begin(),
        result.mDiagnostics.end(),
        [](const auto& diagnostic) {
            const auto* merge = std::get_if<MergeDiagnostic>(&diagnostic);

            return merge && merge->mCode == MergeDiagnosticCode::InstrumentAssetClassConflict;
        });

    ASSERT_NE(conflict, result.mDiagnostics.end());

    const auto& error = std::get<MergeDiagnostic>(*conflict);

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mIsin, "XX9000000248");
    ASSERT_EQ(error.mAssetClassVariants.size(), 2U);
    EXPECT_EQ(error.mAssetClassVariants[0].mAssetClass, AssetClass::Stock);
    ASSERT_EQ(error.mAssetClassVariants[0].mSources.size(), 1U);
    EXPECT_EQ(error.mAssetClassVariants[1].mAssetClass, AssetClass::Unknown);
    ASSERT_EQ(error.mAssetClassVariants[1].mSources.size(), 1U);
    EXPECT_EQ(error.mAssetClassVariants[0].mSources[0], known.mMetadata.mSources[0]);
    EXPECT_EQ(error.mAssetClassVariants[1].mSources[0], unknown.mMetadata.mSources[0]);
    EXPECT_EQ(error.mAssetClassVariants[1].mUnrecognizedLabel, "");
    ASSERT_EQ(result.mDiagnostics.size(), 2U);

    const auto& parsed = std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]);

    EXPECT_EQ(parsed.mDiagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(parsed.mDiagnostic.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(parsed.mDiagnostic.mField, "asset_class");
    EXPECT_EQ(parsed.mDiagnostic.mRowIndex, 3U);
    EXPECT_EQ(parsed.mDiagnostic.mTransactionId, "synthetic-purchase");
}

TEST(StatementMergerIntegrationTest, UnrecognizedClassStaysUnresolvedWithStockLast) {
    const IncomeCsvRow unknownRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "COMMODITY",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow knownRow{
        .mDatetime = "2024-01-16T10:00:00.000Z",
        .mDate = "2024-01-16",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "SELL",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "-1.000000000",
        .mPrice = "12.0000",
        .mAmount = "12.0000",
        .mFee = "-0.2000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-sale",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto history = files.write("history.csv", {unknownRow, knownRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(history, 0)}};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Synthetic Evidence Share");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Unknown);
    ASSERT_EQ(instrument.mTransactions.size(), 2U);
    ASSERT_EQ(instrument.mCorporateActions.size(), 0U);
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 2U);

    const auto& unknown = instrument.mTransactions[0];
    const auto& known = instrument.mTransactions[1];

    EXPECT_EQ(unknown.mMetadata.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(unknown.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-15T10:00:00.000Z"));
    EXPECT_FALSE(unknown.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(unknown.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(unknown.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(unknown.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(unknown.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(unknown.mMetadata.mSources[0].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(unknown.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(unknown.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(unknown.mInstrumentEvidence[0].mSource, unknown.mMetadata.mSources[0]);
    EXPECT_EQ(unknown.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(unknown.mInstrumentEvidence[0].mAssetClass, AssetClass::Unknown);
    EXPECT_EQ(unknown.mInstrumentEvidence[0].mAssetClassLabel, "COMMODITY");
    EXPECT_EQ(unknown.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(unknown.mUnits, 200000000);  // Source 2.000000000 units at eight decimal places.
    EXPECT_EQ(unknown.mUnitPrice, 100000); // Source EUR 10.0000 at four decimal places.
    EXPECT_EQ(unknown.mAmount, 200000);
    EXPECT_EQ(unknown.mFeePaid, 1000);
    EXPECT_EQ(unknown.mCurrency, Currency::EUR);
    EXPECT_EQ(unknown.mExchangeRate, 100'000'000);
    ASSERT_EQ(unknown.mUnitEvidence.size(), 1U);
    EXPECT_EQ(unknown.mUnitEvidence[0].mSourceText, "2.000000000");
    EXPECT_EQ(unknown.mUnitEvidence[0].mSource, unknown.mMetadata.mSources[0]);

    EXPECT_EQ(known.mMetadata.mTaxDate, parseCalendarDate("2024-01-16"));
    EXPECT_EQ(known.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-16T10:00:00.000Z"));
    EXPECT_FALSE(known.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(known.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(known.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(known.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(known.mMetadata.mSources[0].mSourceRow, 3U);
    EXPECT_EQ(known.mMetadata.mSources[0].mTransactionId, "synthetic-sale");
    EXPECT_EQ(known.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 1}));
    ASSERT_EQ(known.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(known.mInstrumentEvidence[0].mSource, known.mMetadata.mSources[0]);
    EXPECT_EQ(known.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(known.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(known.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(known.mTradeSide, TradeSide::Sell);
    EXPECT_EQ(known.mUnits, 100000000);  // Source 1.000000000 units at eight decimal places.
    EXPECT_EQ(known.mUnitPrice, 120000); // Source EUR 12.0000 at four decimal places.
    EXPECT_EQ(known.mAmount, 120000);
    EXPECT_EQ(known.mFeePaid, 2000);
    EXPECT_EQ(known.mCurrency, Currency::EUR);
    EXPECT_EQ(known.mExchangeRate, 100'000'000);
    ASSERT_EQ(known.mUnitEvidence.size(), 1U);
    EXPECT_EQ(known.mUnitEvidence[0].mSourceText, "-1.000000000");
    EXPECT_EQ(known.mUnitEvidence[0].mSource, known.mMetadata.mSources[0]);

    const auto conflict = std::find_if(
        result.mDiagnostics.begin(),
        result.mDiagnostics.end(),
        [](const auto& diagnostic) {
            const auto* merge = std::get_if<MergeDiagnostic>(&diagnostic);

            return merge && merge->mCode == MergeDiagnosticCode::InstrumentAssetClassConflict;
        });

    ASSERT_NE(conflict, result.mDiagnostics.end());

    const auto& error = std::get<MergeDiagnostic>(*conflict);

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mIsin, "XX9000000248");
    ASSERT_EQ(error.mAssetClassVariants.size(), 2U);
    EXPECT_EQ(error.mAssetClassVariants[0].mAssetClass, AssetClass::Stock);
    ASSERT_EQ(error.mAssetClassVariants[0].mSources.size(), 1U);
    EXPECT_EQ(error.mAssetClassVariants[1].mAssetClass, AssetClass::Unknown);
    ASSERT_EQ(error.mAssetClassVariants[1].mSources.size(), 1U);
    EXPECT_EQ(error.mAssetClassVariants[0].mSources[0], known.mMetadata.mSources[0]);
    EXPECT_EQ(error.mAssetClassVariants[1].mSources[0], unknown.mMetadata.mSources[0]);
    EXPECT_EQ(error.mAssetClassVariants[1].mUnrecognizedLabel, "COMMODITY");
    ASSERT_EQ(result.mDiagnostics.size(), 2U);

    const auto& parsed = std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]);

    EXPECT_EQ(parsed.mDiagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(parsed.mDiagnostic.mCode, DiagnosticCode::UnsupportedAssetClass);
    EXPECT_EQ(parsed.mDiagnostic.mField, "asset_class");
    EXPECT_EQ(parsed.mDiagnostic.mRowIndex, 2U);
    EXPECT_EQ(parsed.mDiagnostic.mTransactionId, "synthetic-purchase");
}

TEST(StatementMergerIntegrationTest, UnrecognizedClassStaysUnresolvedWithStockFirst) {
    const IncomeCsvRow unknownRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "COMMODITY",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow knownRow{
        .mDatetime = "2024-01-16T10:00:00.000Z",
        .mDate = "2024-01-16",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "SELL",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "-1.000000000",
        .mPrice = "12.0000",
        .mAmount = "12.0000",
        .mFee = "-0.2000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-sale",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto history = files.write("history.csv", {knownRow, unknownRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(history, 0)}};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Synthetic Evidence Share");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Unknown);
    ASSERT_EQ(instrument.mTransactions.size(), 2U);
    ASSERT_EQ(instrument.mCorporateActions.size(), 0U);
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 2U);

    const auto& unknown = instrument.mTransactions[0];
    const auto& known = instrument.mTransactions[1];

    EXPECT_EQ(unknown.mMetadata.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(unknown.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-15T10:00:00.000Z"));
    EXPECT_FALSE(unknown.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(unknown.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(unknown.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(unknown.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(unknown.mMetadata.mSources[0].mSourceRow, 3U);
    EXPECT_EQ(unknown.mMetadata.mSources[0].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(unknown.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 1}));
    ASSERT_EQ(unknown.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(unknown.mInstrumentEvidence[0].mSource, unknown.mMetadata.mSources[0]);
    EXPECT_EQ(unknown.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(unknown.mInstrumentEvidence[0].mAssetClass, AssetClass::Unknown);
    EXPECT_EQ(unknown.mInstrumentEvidence[0].mAssetClassLabel, "COMMODITY");
    EXPECT_EQ(unknown.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(unknown.mUnits, 200000000);  // Source 2.000000000 units at eight decimal places.
    EXPECT_EQ(unknown.mUnitPrice, 100000); // Source EUR 10.0000 at four decimal places.
    EXPECT_EQ(unknown.mAmount, 200000);
    EXPECT_EQ(unknown.mFeePaid, 1000);
    EXPECT_EQ(unknown.mCurrency, Currency::EUR);
    EXPECT_EQ(unknown.mExchangeRate, 100'000'000);
    ASSERT_EQ(unknown.mUnitEvidence.size(), 1U);
    EXPECT_EQ(unknown.mUnitEvidence[0].mSourceText, "2.000000000");
    EXPECT_EQ(unknown.mUnitEvidence[0].mSource, unknown.mMetadata.mSources[0]);

    EXPECT_EQ(known.mMetadata.mTaxDate, parseCalendarDate("2024-01-16"));
    EXPECT_EQ(known.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-16T10:00:00.000Z"));
    EXPECT_FALSE(known.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(known.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(known.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(known.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(known.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(known.mMetadata.mSources[0].mTransactionId, "synthetic-sale");
    EXPECT_EQ(known.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(known.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(known.mInstrumentEvidence[0].mSource, known.mMetadata.mSources[0]);
    EXPECT_EQ(known.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(known.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(known.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(known.mTradeSide, TradeSide::Sell);
    EXPECT_EQ(known.mUnits, 100000000);  // Source 1.000000000 units at eight decimal places.
    EXPECT_EQ(known.mUnitPrice, 120000); // Source EUR 12.0000 at four decimal places.
    EXPECT_EQ(known.mAmount, 120000);
    EXPECT_EQ(known.mFeePaid, 2000);
    EXPECT_EQ(known.mCurrency, Currency::EUR);
    EXPECT_EQ(known.mExchangeRate, 100'000'000);
    ASSERT_EQ(known.mUnitEvidence.size(), 1U);
    EXPECT_EQ(known.mUnitEvidence[0].mSourceText, "-1.000000000");
    EXPECT_EQ(known.mUnitEvidence[0].mSource, known.mMetadata.mSources[0]);

    const auto conflict = std::find_if(
        result.mDiagnostics.begin(),
        result.mDiagnostics.end(),
        [](const auto& diagnostic) {
            const auto* merge = std::get_if<MergeDiagnostic>(&diagnostic);

            return merge && merge->mCode == MergeDiagnosticCode::InstrumentAssetClassConflict;
        });

    ASSERT_NE(conflict, result.mDiagnostics.end());

    const auto& error = std::get<MergeDiagnostic>(*conflict);

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mIsin, "XX9000000248");
    ASSERT_EQ(error.mAssetClassVariants.size(), 2U);
    EXPECT_EQ(error.mAssetClassVariants[0].mAssetClass, AssetClass::Stock);
    ASSERT_EQ(error.mAssetClassVariants[0].mSources.size(), 1U);
    EXPECT_EQ(error.mAssetClassVariants[1].mAssetClass, AssetClass::Unknown);
    ASSERT_EQ(error.mAssetClassVariants[1].mSources.size(), 1U);
    EXPECT_EQ(error.mAssetClassVariants[0].mSources[0], known.mMetadata.mSources[0]);
    EXPECT_EQ(error.mAssetClassVariants[1].mSources[0], unknown.mMetadata.mSources[0]);
    EXPECT_EQ(error.mAssetClassVariants[1].mUnrecognizedLabel, "COMMODITY");
    ASSERT_EQ(result.mDiagnostics.size(), 2U);

    const auto& parsed = std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]);

    EXPECT_EQ(parsed.mDiagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(parsed.mDiagnostic.mCode, DiagnosticCode::UnsupportedAssetClass);
    EXPECT_EQ(parsed.mDiagnostic.mField, "asset_class");
    EXPECT_EQ(parsed.mDiagnostic.mRowIndex, 3U);
    EXPECT_EQ(parsed.mDiagnostic.mTransactionId, "synthetic-purchase");
}

TEST(StatementMergerIntegrationTest, RetainsDistinctUnrecognizedClassLabels) {
    const IncomeCsvRow firstRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "COMMODITY",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow secondRow{
        .mDatetime = "2024-01-16T10:00:00.000Z",
        .mDate = "2024-01-16",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "SELL",
        .mAssetClass = "UNLISTED",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "-1.000000000",
        .mPrice = "12.0000",
        .mAmount = "12.0000",
        .mFee = "-0.2000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-sale",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto history = files.write("history.csv", {firstRow, secondRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(history, 0)}};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Synthetic Evidence Share");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Unknown);
    ASSERT_EQ(instrument.mTransactions.size(), 2U);
    ASSERT_EQ(instrument.mCorporateActions.size(), 0U);
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 2U);

    const auto& first = instrument.mTransactions[0];
    const auto& second = instrument.mTransactions[1];

    EXPECT_EQ(first.mMetadata.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(first.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-15T10:00:00.000Z"));
    EXPECT_FALSE(first.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(first.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(first.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(first.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(first.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(first.mMetadata.mSources[0].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(first.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(first.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(first.mInstrumentEvidence[0].mSource, first.mMetadata.mSources[0]);
    EXPECT_EQ(first.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(first.mInstrumentEvidence[0].mAssetClass, AssetClass::Unknown);
    EXPECT_EQ(first.mInstrumentEvidence[0].mAssetClassLabel, "COMMODITY");
    EXPECT_EQ(first.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(first.mUnits, 200000000);  // Source 2.000000000 units at eight decimal places.
    EXPECT_EQ(first.mUnitPrice, 100000); // Source EUR 10.0000 at four decimal places.
    EXPECT_EQ(first.mAmount, 200000);
    EXPECT_EQ(first.mFeePaid, 1000);
    EXPECT_EQ(first.mCurrency, Currency::EUR);
    EXPECT_EQ(first.mExchangeRate, 100'000'000);
    ASSERT_EQ(first.mUnitEvidence.size(), 1U);
    EXPECT_EQ(first.mUnitEvidence[0].mSourceText, "2.000000000");
    EXPECT_EQ(first.mUnitEvidence[0].mSource, first.mMetadata.mSources[0]);

    EXPECT_EQ(second.mMetadata.mTaxDate, parseCalendarDate("2024-01-16"));
    EXPECT_EQ(second.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-16T10:00:00.000Z"));
    EXPECT_FALSE(second.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(second.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(second.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(second.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(second.mMetadata.mSources[0].mSourceRow, 3U);
    EXPECT_EQ(second.mMetadata.mSources[0].mTransactionId, "synthetic-sale");
    EXPECT_EQ(second.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 1}));
    ASSERT_EQ(second.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(second.mInstrumentEvidence[0].mSource, second.mMetadata.mSources[0]);
    EXPECT_EQ(second.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(second.mInstrumentEvidence[0].mAssetClass, AssetClass::Unknown);
    EXPECT_EQ(second.mInstrumentEvidence[0].mAssetClassLabel, "UNLISTED");
    EXPECT_EQ(second.mTradeSide, TradeSide::Sell);
    EXPECT_EQ(second.mUnits, 100000000);  // Source 1.000000000 units at eight decimal places.
    EXPECT_EQ(second.mUnitPrice, 120000); // Source EUR 12.0000 at four decimal places.
    EXPECT_EQ(second.mAmount, 120000);
    EXPECT_EQ(second.mFeePaid, 2000);
    EXPECT_EQ(second.mCurrency, Currency::EUR);
    EXPECT_EQ(second.mExchangeRate, 100'000'000);
    ASSERT_EQ(second.mUnitEvidence.size(), 1U);
    EXPECT_EQ(second.mUnitEvidence[0].mSourceText, "-1.000000000");
    EXPECT_EQ(second.mUnitEvidence[0].mSource, second.mMetadata.mSources[0]);

    const auto conflict = std::find_if(
        result.mDiagnostics.begin(),
        result.mDiagnostics.end(),
        [](const auto& diagnostic) {
            const auto* merge = std::get_if<MergeDiagnostic>(&diagnostic);

            return merge && merge->mCode == MergeDiagnosticCode::InstrumentAssetClassConflict;
        });

    ASSERT_NE(conflict, result.mDiagnostics.end());

    const auto& error = std::get<MergeDiagnostic>(*conflict);

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mIsin, "XX9000000248");
    ASSERT_EQ(error.mAssetClassVariants.size(), 2U);
    EXPECT_EQ(error.mAssetClassVariants[0].mAssetClass, AssetClass::Unknown);
    ASSERT_EQ(error.mAssetClassVariants[0].mSources.size(), 1U);
    EXPECT_EQ(error.mAssetClassVariants[1].mAssetClass, AssetClass::Unknown);
    ASSERT_EQ(error.mAssetClassVariants[1].mSources.size(), 1U);
    EXPECT_EQ(error.mAssetClassVariants[0].mUnrecognizedLabel, "COMMODITY");
    EXPECT_EQ(error.mAssetClassVariants[1].mUnrecognizedLabel, "UNLISTED");
    EXPECT_EQ(error.mAssetClassVariants[0].mSources[0], first.mMetadata.mSources[0]);
    EXPECT_EQ(error.mAssetClassVariants[1].mSources[0], second.mMetadata.mSources[0]);
}

TEST(StatementMergerIntegrationTest, RejectsWholeSameIdGroupWithKnownClassesInOneFileLayout) {
    const IncomeCsvRow firstRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow secondRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "FUND",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow healthyRow{
        .mDatetime = "2024-01-17T10:00:00.000Z",
        .mDate = "2024-01-17",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "3.000000000",
        .mPrice = "8.0000",
        .mAmount = "-24.0000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-third",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto history = files.write("history.csv", {firstRow, secondRow, healthyRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(history, 0)}};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Synthetic Evidence Share");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Stock);
    ASSERT_EQ(instrument.mTransactions.size(), 1U);
    ASSERT_EQ(instrument.mCorporateActions.size(), 0U);
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 1U);

    const auto& survivor = instrument.mTransactions[0];

    EXPECT_EQ(survivor.mMetadata.mTaxDate, parseCalendarDate("2024-01-17"));
    EXPECT_EQ(survivor.mMetadata.mSourceTimestamp,
              parseSourceTimestamp("2024-01-17T10:00:00.000Z"));
    EXPECT_FALSE(survivor.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(survivor.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(survivor.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(survivor.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(survivor.mMetadata.mSources[0].mSourceRow, 4U);
    EXPECT_EQ(survivor.mMetadata.mSources[0].mTransactionId, "synthetic-third");
    EXPECT_EQ(survivor.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 2}));
    ASSERT_EQ(survivor.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(survivor.mInstrumentEvidence[0].mSource, survivor.mMetadata.mSources[0]);
    EXPECT_EQ(survivor.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(survivor.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(survivor.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(survivor.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(survivor.mUnits, 300000000); // Source 3.000000000 units at eight decimal places.
    EXPECT_EQ(survivor.mUnitPrice, 80000); // Source EUR 8.0000 at four decimal places.
    EXPECT_EQ(survivor.mAmount, 240000);
    EXPECT_EQ(survivor.mFeePaid, 0);
    EXPECT_EQ(survivor.mCurrency, Currency::EUR);
    EXPECT_EQ(survivor.mExchangeRate, 100'000'000);
    ASSERT_EQ(survivor.mUnitEvidence.size(), 1U);
    EXPECT_EQ(survivor.mUnitEvidence[0].mSourceText, "3.000000000");
    EXPECT_EQ(survivor.mUnitEvidence[0].mSource, survivor.mMetadata.mSources[0]);

    const auto conflict =
        std::find_if(result.mDiagnostics.begin(),
                     result.mDiagnostics.end(),
                     [](const auto& diagnostic) {
                         const auto* merge = std::get_if<MergeDiagnostic>(&diagnostic);

                         return merge && merge->mCode == MergeDiagnosticCode::ConflictingDuplicate;
                     });

    ASSERT_NE(conflict, result.mDiagnostics.end());

    const auto& error = std::get<MergeDiagnostic>(*conflict);

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    ASSERT_EQ(error.mSources.size(), 2U);
    EXPECT_EQ(error.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(error.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(error.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    EXPECT_EQ(error.mSources[0].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(error.mSources[1].mFilename.value(), "history.csv");
    EXPECT_EQ(error.mSources[1].mSourceRow, 3U);
    EXPECT_EQ(error.mSources[1].mInputSequence, (StableInputSequence{0, 1}));
    EXPECT_EQ(error.mSources[1].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(error.mEventKinds, (std::vector{StatementEventKind::Trade}));
}

TEST(StatementMergerIntegrationTest, RejectsWholeSameIdGroupWithKnownClassesInTwoFileLayout) {
    const IncomeCsvRow firstRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow secondRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "FUND",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow healthyRow{
        .mDatetime = "2024-01-17T10:00:00.000Z",
        .mDate = "2024-01-17",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "3.000000000",
        .mPrice = "8.0000",
        .mAmount = "-24.0000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-third",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto file0 = files.write("part-0.csv", {firstRow});
    const auto file1 = files.write("part-1.csv", {secondRow});
    const auto file2 = files.write("part-2.csv", {healthyRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 2, .mParseResult = parser.parse(file2, 2)},
        StatementMergeInput{.mSourceIndex = 1, .mParseResult = parser.parse(file1, 1)},
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(file0, 0)},
    };

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Synthetic Evidence Share");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Stock);
    ASSERT_EQ(instrument.mTransactions.size(), 1U);
    ASSERT_EQ(instrument.mCorporateActions.size(), 0U);
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 1U);

    const auto& survivor = instrument.mTransactions[0];

    EXPECT_EQ(survivor.mMetadata.mTaxDate, parseCalendarDate("2024-01-17"));
    EXPECT_EQ(survivor.mMetadata.mSourceTimestamp,
              parseSourceTimestamp("2024-01-17T10:00:00.000Z"));
    EXPECT_FALSE(survivor.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(survivor.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(survivor.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(survivor.mMetadata.mSources[0].mFilename.value(), "part-2.csv");
    EXPECT_EQ(survivor.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(survivor.mMetadata.mSources[0].mTransactionId, "synthetic-third");
    EXPECT_EQ(survivor.mMetadata.mSources[0].mInputSequence, (StableInputSequence{2, 0}));
    ASSERT_EQ(survivor.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(survivor.mInstrumentEvidence[0].mSource, survivor.mMetadata.mSources[0]);
    EXPECT_EQ(survivor.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(survivor.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(survivor.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(survivor.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(survivor.mUnits, 300000000); // Source 3.000000000 units at eight decimal places.
    EXPECT_EQ(survivor.mUnitPrice, 80000); // Source EUR 8.0000 at four decimal places.
    EXPECT_EQ(survivor.mAmount, 240000);
    EXPECT_EQ(survivor.mFeePaid, 0);
    EXPECT_EQ(survivor.mCurrency, Currency::EUR);
    EXPECT_EQ(survivor.mExchangeRate, 100'000'000);
    ASSERT_EQ(survivor.mUnitEvidence.size(), 1U);
    EXPECT_EQ(survivor.mUnitEvidence[0].mSourceText, "3.000000000");
    EXPECT_EQ(survivor.mUnitEvidence[0].mSource, survivor.mMetadata.mSources[0]);

    const auto conflict =
        std::find_if(result.mDiagnostics.begin(),
                     result.mDiagnostics.end(),
                     [](const auto& diagnostic) {
                         const auto* merge = std::get_if<MergeDiagnostic>(&diagnostic);

                         return merge && merge->mCode == MergeDiagnosticCode::ConflictingDuplicate;
                     });

    ASSERT_NE(conflict, result.mDiagnostics.end());

    const auto& error = std::get<MergeDiagnostic>(*conflict);

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    ASSERT_EQ(error.mSources.size(), 2U);
    EXPECT_EQ(error.mSources[0].mFilename.value(), "part-0.csv");
    EXPECT_EQ(error.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(error.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    EXPECT_EQ(error.mSources[0].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(error.mSources[1].mFilename.value(), "part-1.csv");
    EXPECT_EQ(error.mSources[1].mSourceRow, 2U);
    EXPECT_EQ(error.mSources[1].mInputSequence, (StableInputSequence{1, 0}));
    EXPECT_EQ(error.mSources[1].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(error.mEventKinds, (std::vector{StatementEventKind::Trade}));
}

TEST(StatementMergerIntegrationTest, RejectsWholeSameIdGroupWithUnrecognizedLabelsInOneFileLayout) {
    const IncomeCsvRow firstRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "COMMODITY",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow secondRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "UNLISTED",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow healthyRow{
        .mDatetime = "2024-01-17T10:00:00.000Z",
        .mDate = "2024-01-17",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "3.000000000",
        .mPrice = "8.0000",
        .mAmount = "-24.0000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-third",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto history = files.write("history.csv", {firstRow, secondRow, healthyRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(history, 0)}};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Synthetic Evidence Share");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Stock);
    ASSERT_EQ(instrument.mTransactions.size(), 1U);
    ASSERT_EQ(instrument.mCorporateActions.size(), 0U);
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 1U);

    const auto& survivor = instrument.mTransactions[0];

    EXPECT_EQ(survivor.mMetadata.mTaxDate, parseCalendarDate("2024-01-17"));
    EXPECT_EQ(survivor.mMetadata.mSourceTimestamp,
              parseSourceTimestamp("2024-01-17T10:00:00.000Z"));
    EXPECT_FALSE(survivor.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(survivor.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(survivor.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(survivor.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(survivor.mMetadata.mSources[0].mSourceRow, 4U);
    EXPECT_EQ(survivor.mMetadata.mSources[0].mTransactionId, "synthetic-third");
    EXPECT_EQ(survivor.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 2}));
    ASSERT_EQ(survivor.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(survivor.mInstrumentEvidence[0].mSource, survivor.mMetadata.mSources[0]);
    EXPECT_EQ(survivor.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(survivor.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(survivor.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(survivor.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(survivor.mUnits, 300000000); // Source 3.000000000 units at eight decimal places.
    EXPECT_EQ(survivor.mUnitPrice, 80000); // Source EUR 8.0000 at four decimal places.
    EXPECT_EQ(survivor.mAmount, 240000);
    EXPECT_EQ(survivor.mFeePaid, 0);
    EXPECT_EQ(survivor.mCurrency, Currency::EUR);
    EXPECT_EQ(survivor.mExchangeRate, 100'000'000);
    ASSERT_EQ(survivor.mUnitEvidence.size(), 1U);
    EXPECT_EQ(survivor.mUnitEvidence[0].mSourceText, "3.000000000");
    EXPECT_EQ(survivor.mUnitEvidence[0].mSource, survivor.mMetadata.mSources[0]);

    const auto conflict =
        std::find_if(result.mDiagnostics.begin(),
                     result.mDiagnostics.end(),
                     [](const auto& diagnostic) {
                         const auto* merge = std::get_if<MergeDiagnostic>(&diagnostic);

                         return merge && merge->mCode == MergeDiagnosticCode::ConflictingDuplicate;
                     });

    ASSERT_NE(conflict, result.mDiagnostics.end());

    const auto& error = std::get<MergeDiagnostic>(*conflict);

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    ASSERT_EQ(error.mSources.size(), 2U);
    EXPECT_EQ(error.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(error.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(error.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    EXPECT_EQ(error.mSources[0].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(error.mSources[1].mFilename.value(), "history.csv");
    EXPECT_EQ(error.mSources[1].mSourceRow, 3U);
    EXPECT_EQ(error.mSources[1].mInputSequence, (StableInputSequence{0, 1}));
    EXPECT_EQ(error.mSources[1].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(error.mEventKinds, (std::vector{StatementEventKind::Trade}));
}

TEST(StatementMergerIntegrationTest, RejectsWholeSameIdGroupWithUnrecognizedLabelsInTwoFileLayout) {
    const IncomeCsvRow firstRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "COMMODITY",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow secondRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "UNLISTED",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow healthyRow{
        .mDatetime = "2024-01-17T10:00:00.000Z",
        .mDate = "2024-01-17",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "3.000000000",
        .mPrice = "8.0000",
        .mAmount = "-24.0000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-third",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto file0 = files.write("part-0.csv", {firstRow});
    const auto file1 = files.write("part-1.csv", {secondRow});
    const auto file2 = files.write("part-2.csv", {healthyRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 2, .mParseResult = parser.parse(file2, 2)},
        StatementMergeInput{.mSourceIndex = 1, .mParseResult = parser.parse(file1, 1)},
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(file0, 0)},
    };

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Synthetic Evidence Share");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Stock);
    ASSERT_EQ(instrument.mTransactions.size(), 1U);
    ASSERT_EQ(instrument.mCorporateActions.size(), 0U);
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 1U);

    const auto& survivor = instrument.mTransactions[0];

    EXPECT_EQ(survivor.mMetadata.mTaxDate, parseCalendarDate("2024-01-17"));
    EXPECT_EQ(survivor.mMetadata.mSourceTimestamp,
              parseSourceTimestamp("2024-01-17T10:00:00.000Z"));
    EXPECT_FALSE(survivor.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(survivor.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(survivor.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(survivor.mMetadata.mSources[0].mFilename.value(), "part-2.csv");
    EXPECT_EQ(survivor.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(survivor.mMetadata.mSources[0].mTransactionId, "synthetic-third");
    EXPECT_EQ(survivor.mMetadata.mSources[0].mInputSequence, (StableInputSequence{2, 0}));
    ASSERT_EQ(survivor.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(survivor.mInstrumentEvidence[0].mSource, survivor.mMetadata.mSources[0]);
    EXPECT_EQ(survivor.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(survivor.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(survivor.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(survivor.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(survivor.mUnits, 300000000); // Source 3.000000000 units at eight decimal places.
    EXPECT_EQ(survivor.mUnitPrice, 80000); // Source EUR 8.0000 at four decimal places.
    EXPECT_EQ(survivor.mAmount, 240000);
    EXPECT_EQ(survivor.mFeePaid, 0);
    EXPECT_EQ(survivor.mCurrency, Currency::EUR);
    EXPECT_EQ(survivor.mExchangeRate, 100'000'000);
    ASSERT_EQ(survivor.mUnitEvidence.size(), 1U);
    EXPECT_EQ(survivor.mUnitEvidence[0].mSourceText, "3.000000000");
    EXPECT_EQ(survivor.mUnitEvidence[0].mSource, survivor.mMetadata.mSources[0]);

    const auto conflict =
        std::find_if(result.mDiagnostics.begin(),
                     result.mDiagnostics.end(),
                     [](const auto& diagnostic) {
                         const auto* merge = std::get_if<MergeDiagnostic>(&diagnostic);

                         return merge && merge->mCode == MergeDiagnosticCode::ConflictingDuplicate;
                     });

    ASSERT_NE(conflict, result.mDiagnostics.end());

    const auto& error = std::get<MergeDiagnostic>(*conflict);

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    ASSERT_EQ(error.mSources.size(), 2U);
    EXPECT_EQ(error.mSources[0].mFilename.value(), "part-0.csv");
    EXPECT_EQ(error.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(error.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    EXPECT_EQ(error.mSources[0].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(error.mSources[1].mFilename.value(), "part-1.csv");
    EXPECT_EQ(error.mSources[1].mSourceRow, 2U);
    EXPECT_EQ(error.mSources[1].mInputSequence, (StableInputSequence{1, 0}));
    EXPECT_EQ(error.mSources[1].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(error.mEventKinds, (std::vector{StatementEventKind::Trade}));
}

TEST(StatementMergerIntegrationTest, RejectsWholeSameIdGroupWithTradeAndActionInOneFileLayout) {
    const IncomeCsvRow firstRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow secondRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "FUND",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "4.000000000",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow healthyRow{
        .mDatetime = "2024-01-17T10:00:00.000Z",
        .mDate = "2024-01-17",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "3.000000000",
        .mPrice = "8.0000",
        .mAmount = "-24.0000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-third",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto history = files.write("history.csv", {firstRow, secondRow, healthyRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(history, 0)}};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Synthetic Evidence Share");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Stock);
    ASSERT_EQ(instrument.mTransactions.size(), 1U);
    ASSERT_EQ(instrument.mCorporateActions.size(), 0U);
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 1U);

    const auto& survivor = instrument.mTransactions[0];

    EXPECT_EQ(survivor.mMetadata.mTaxDate, parseCalendarDate("2024-01-17"));
    EXPECT_EQ(survivor.mMetadata.mSourceTimestamp,
              parseSourceTimestamp("2024-01-17T10:00:00.000Z"));
    EXPECT_FALSE(survivor.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(survivor.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(survivor.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(survivor.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(survivor.mMetadata.mSources[0].mSourceRow, 4U);
    EXPECT_EQ(survivor.mMetadata.mSources[0].mTransactionId, "synthetic-third");
    EXPECT_EQ(survivor.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 2}));
    ASSERT_EQ(survivor.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(survivor.mInstrumentEvidence[0].mSource, survivor.mMetadata.mSources[0]);
    EXPECT_EQ(survivor.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(survivor.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(survivor.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(survivor.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(survivor.mUnits, 300000000); // Source 3.000000000 units at eight decimal places.
    EXPECT_EQ(survivor.mUnitPrice, 80000); // Source EUR 8.0000 at four decimal places.
    EXPECT_EQ(survivor.mAmount, 240000);
    EXPECT_EQ(survivor.mFeePaid, 0);
    EXPECT_EQ(survivor.mCurrency, Currency::EUR);
    EXPECT_EQ(survivor.mExchangeRate, 100'000'000);
    ASSERT_EQ(survivor.mUnitEvidence.size(), 1U);
    EXPECT_EQ(survivor.mUnitEvidence[0].mSourceText, "3.000000000");
    EXPECT_EQ(survivor.mUnitEvidence[0].mSource, survivor.mMetadata.mSources[0]);

    const auto conflict =
        std::find_if(result.mDiagnostics.begin(),
                     result.mDiagnostics.end(),
                     [](const auto& diagnostic) {
                         const auto* merge = std::get_if<MergeDiagnostic>(&diagnostic);

                         return merge && merge->mCode == MergeDiagnosticCode::ConflictingDuplicate;
                     });

    ASSERT_NE(conflict, result.mDiagnostics.end());

    const auto& error = std::get<MergeDiagnostic>(*conflict);

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    ASSERT_EQ(error.mSources.size(), 2U);
    EXPECT_EQ(error.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(error.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(error.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    EXPECT_EQ(error.mSources[0].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(error.mSources[1].mFilename.value(), "history.csv");
    EXPECT_EQ(error.mSources[1].mSourceRow, 3U);
    EXPECT_EQ(error.mSources[1].mInputSequence, (StableInputSequence{0, 1}));
    EXPECT_EQ(error.mSources[1].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(error.mEventKinds,
              (std::vector{StatementEventKind::Trade, StatementEventKind::CorporateAction}));
}

TEST(StatementMergerIntegrationTest, RejectsWholeSameIdGroupWithTradeAndActionInTwoFileLayout) {
    const IncomeCsvRow firstRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow secondRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "FUND",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "4.000000000",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow healthyRow{
        .mDatetime = "2024-01-17T10:00:00.000Z",
        .mDate = "2024-01-17",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "3.000000000",
        .mPrice = "8.0000",
        .mAmount = "-24.0000",
        .mFee = "",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-third",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto file0 = files.write("part-0.csv", {firstRow});
    const auto file1 = files.write("part-1.csv", {secondRow});
    const auto file2 = files.write("part-2.csv", {healthyRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 2, .mParseResult = parser.parse(file2, 2)},
        StatementMergeInput{.mSourceIndex = 1, .mParseResult = parser.parse(file1, 1)},
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(file0, 0)},
    };

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Synthetic Evidence Share");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Stock);
    ASSERT_EQ(instrument.mTransactions.size(), 1U);
    ASSERT_EQ(instrument.mCorporateActions.size(), 0U);
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 1U);

    const auto& survivor = instrument.mTransactions[0];

    EXPECT_EQ(survivor.mMetadata.mTaxDate, parseCalendarDate("2024-01-17"));
    EXPECT_EQ(survivor.mMetadata.mSourceTimestamp,
              parseSourceTimestamp("2024-01-17T10:00:00.000Z"));
    EXPECT_FALSE(survivor.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(survivor.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(survivor.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(survivor.mMetadata.mSources[0].mFilename.value(), "part-2.csv");
    EXPECT_EQ(survivor.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(survivor.mMetadata.mSources[0].mTransactionId, "synthetic-third");
    EXPECT_EQ(survivor.mMetadata.mSources[0].mInputSequence, (StableInputSequence{2, 0}));
    ASSERT_EQ(survivor.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(survivor.mInstrumentEvidence[0].mSource, survivor.mMetadata.mSources[0]);
    EXPECT_EQ(survivor.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(survivor.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(survivor.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(survivor.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(survivor.mUnits, 300000000); // Source 3.000000000 units at eight decimal places.
    EXPECT_EQ(survivor.mUnitPrice, 80000); // Source EUR 8.0000 at four decimal places.
    EXPECT_EQ(survivor.mAmount, 240000);
    EXPECT_EQ(survivor.mFeePaid, 0);
    EXPECT_EQ(survivor.mCurrency, Currency::EUR);
    EXPECT_EQ(survivor.mExchangeRate, 100'000'000);
    ASSERT_EQ(survivor.mUnitEvidence.size(), 1U);
    EXPECT_EQ(survivor.mUnitEvidence[0].mSourceText, "3.000000000");
    EXPECT_EQ(survivor.mUnitEvidence[0].mSource, survivor.mMetadata.mSources[0]);

    const auto conflict =
        std::find_if(result.mDiagnostics.begin(),
                     result.mDiagnostics.end(),
                     [](const auto& diagnostic) {
                         const auto* merge = std::get_if<MergeDiagnostic>(&diagnostic);

                         return merge && merge->mCode == MergeDiagnosticCode::ConflictingDuplicate;
                     });

    ASSERT_NE(conflict, result.mDiagnostics.end());

    const auto& error = std::get<MergeDiagnostic>(*conflict);

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    ASSERT_EQ(error.mSources.size(), 2U);
    EXPECT_EQ(error.mSources[0].mFilename.value(), "part-0.csv");
    EXPECT_EQ(error.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(error.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    EXPECT_EQ(error.mSources[0].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(error.mSources[1].mFilename.value(), "part-1.csv");
    EXPECT_EQ(error.mSources[1].mSourceRow, 2U);
    EXPECT_EQ(error.mSources[1].mInputSequence, (StableInputSequence{1, 0}));
    EXPECT_EQ(error.mSources[1].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(error.mEventKinds,
              (std::vector{StatementEventKind::Trade, StatementEventKind::CorporateAction}));
}

TEST(StatementMergerIntegrationTest, RetainsTwoClassifiedEventsWithoutTransactionIds) {
    const IncomeCsvRow firstRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow secondRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto file0 = files.write("part-0.csv", {firstRow});
    const auto file1 = files.write("part-1.csv", {secondRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 1, .mParseResult = parser.parse(file1, 1)},
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(file0, 0)},
    };

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Synthetic Evidence Share");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Stock);
    ASSERT_EQ(instrument.mTransactions.size(), 2U);
    ASSERT_EQ(instrument.mCorporateActions.size(), 0U);
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 2U);

    EXPECT_TRUE(result.mDiagnostics.empty());
    const auto& first = instrument.mTransactions[0];
    const auto& second = instrument.mTransactions[1];

    EXPECT_EQ(first.mMetadata.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(first.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-15T10:00:00.000Z"));
    EXPECT_FALSE(first.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(first.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(first.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(first.mMetadata.mSources[0].mFilename.value(), "part-0.csv");
    EXPECT_EQ(first.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_FALSE(first.mMetadata.mSources[0].mTransactionId.has_value());
    EXPECT_EQ(first.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(first.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(first.mInstrumentEvidence[0].mSource, first.mMetadata.mSources[0]);
    EXPECT_EQ(first.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(first.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(first.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(first.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(first.mUnits, 200000000);  // Source 2.000000000 units at eight decimal places.
    EXPECT_EQ(first.mUnitPrice, 100000); // Source EUR 10.0000 at four decimal places.
    EXPECT_EQ(first.mAmount, 200000);
    EXPECT_EQ(first.mFeePaid, 1000);
    EXPECT_EQ(first.mCurrency, Currency::EUR);
    EXPECT_EQ(first.mExchangeRate, 100'000'000);
    ASSERT_EQ(first.mUnitEvidence.size(), 1U);
    EXPECT_EQ(first.mUnitEvidence[0].mSourceText, "2.000000000");
    EXPECT_EQ(first.mUnitEvidence[0].mSource, first.mMetadata.mSources[0]);

    EXPECT_EQ(second.mMetadata.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(second.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-15T10:00:00.000Z"));
    EXPECT_FALSE(second.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(second.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(second.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(second.mMetadata.mSources[0].mFilename.value(), "part-1.csv");
    EXPECT_EQ(second.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_FALSE(second.mMetadata.mSources[0].mTransactionId.has_value());
    EXPECT_EQ(second.mMetadata.mSources[0].mInputSequence, (StableInputSequence{1, 0}));
    ASSERT_EQ(second.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(second.mInstrumentEvidence[0].mSource, second.mMetadata.mSources[0]);
    EXPECT_EQ(second.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(second.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(second.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(second.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(second.mUnits, 200000000);  // Source 2.000000000 units at eight decimal places.
    EXPECT_EQ(second.mUnitPrice, 100000); // Source EUR 10.0000 at four decimal places.
    EXPECT_EQ(second.mAmount, 200000);
    EXPECT_EQ(second.mFeePaid, 1000);
    EXPECT_EQ(second.mCurrency, Currency::EUR);
    EXPECT_EQ(second.mExchangeRate, 100'000'000);
    ASSERT_EQ(second.mUnitEvidence.size(), 1U);
    EXPECT_EQ(second.mUnitEvidence[0].mSourceText, "2.000000000");
    EXPECT_EQ(second.mUnitEvidence[0].mSource, second.mMetadata.mSources[0]);
}

TEST(StatementMergerIntegrationTest, CompletesMissingTradeNameInOneFileLayout) {
    const IncomeCsvRow unnamedRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow namedRow{
        .mDatetime = "2024-01-16T10:00:00.000Z",
        .mDate = "2024-01-16",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "SELL",
        .mAssetClass = "STOCK",
        .mName = "Supplied Evidence Name",
        .mSymbol = "XX9000000248",
        .mShares = "-1.000000000",
        .mPrice = "12.0000",
        .mAmount = "12.0000",
        .mFee = "-0.2000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-sale",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto history = files.write("history.csv", {unnamedRow, namedRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(history, 0)}};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Supplied Evidence Name");
    ASSERT_EQ(instrument.mTransactions.size(), 2U);

    const auto& unnamed = instrument.mTransactions[0];
    const auto& named = instrument.mTransactions[1];

    EXPECT_EQ(instrument.mAssetClass, AssetClass::Stock);
    EXPECT_EQ(unnamed.mMetadata.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(unnamed.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-15T10:00:00.000Z"));
    EXPECT_FALSE(unnamed.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(unnamed.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(unnamed.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mSource, unnamed.mMetadata.mSources[0]);
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mName, "");
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(unnamed.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(unnamed.mUnits, 200000000);  // Source 2.000000000 units at eight decimal places.
    EXPECT_EQ(unnamed.mUnitPrice, 100000); // Source EUR 10.0000 at four decimal places.
    EXPECT_EQ(unnamed.mAmount, 200000);
    EXPECT_EQ(unnamed.mFeePaid, 1000);
    EXPECT_EQ(unnamed.mCurrency, Currency::EUR);
    EXPECT_EQ(unnamed.mExchangeRate, 100'000'000);
    ASSERT_EQ(unnamed.mUnitEvidence.size(), 1U);
    EXPECT_EQ(unnamed.mUnitEvidence[0].mSourceText, "2.000000000");
    EXPECT_EQ(unnamed.mUnitEvidence[0].mSource, unnamed.mMetadata.mSources[0]);

    EXPECT_EQ(named.mMetadata.mTaxDate, parseCalendarDate("2024-01-16"));
    EXPECT_EQ(named.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-16T10:00:00.000Z"));
    EXPECT_FALSE(named.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(named.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(named.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(named.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(named.mMetadata.mSources[0].mSourceRow, 3U);
    EXPECT_EQ(named.mMetadata.mSources[0].mTransactionId, "synthetic-sale");
    EXPECT_EQ(named.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 1}));
    ASSERT_EQ(named.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(named.mInstrumentEvidence[0].mSource, named.mMetadata.mSources[0]);
    EXPECT_EQ(named.mInstrumentEvidence[0].mName, "Supplied Evidence Name");
    EXPECT_EQ(named.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(named.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(named.mTradeSide, TradeSide::Sell);
    EXPECT_EQ(named.mUnits, 100000000);  // Source 1.000000000 units at eight decimal places.
    EXPECT_EQ(named.mUnitPrice, 120000); // Source EUR 12.0000 at four decimal places.
    EXPECT_EQ(named.mAmount, 120000);
    EXPECT_EQ(named.mFeePaid, 2000);
    EXPECT_EQ(named.mCurrency, Currency::EUR);
    EXPECT_EQ(named.mExchangeRate, 100'000'000);
    ASSERT_EQ(named.mUnitEvidence.size(), 1U);
    EXPECT_EQ(named.mUnitEvidence[0].mSourceText, "-1.000000000");
    EXPECT_EQ(named.mUnitEvidence[0].mSource, named.mMetadata.mSources[0]);

    ASSERT_EQ(result.mDiagnostics.size(), 2U);

    const auto& missing = std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]);
    const auto& names = std::get<MergeDiagnostic>(result.mDiagnostics[1]);

    EXPECT_EQ(missing.mDiagnostic.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(missing.mDiagnostic.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(missing.mDiagnostic.mField, "name");
    EXPECT_EQ(missing.mDiagnostic.mRowIndex, 2U);
    EXPECT_EQ(names.mCode, MergeDiagnosticCode::InstrumentNameConflict);
    EXPECT_EQ(names.mSeverity, DiagnosticSeverity::Warning);
    ASSERT_EQ(names.mNameVariants.size(), 2U);
    EXPECT_EQ(names.mNameVariants[0].mName, "");
    EXPECT_EQ(names.mNameVariants[1].mName, "Supplied Evidence Name");
    EXPECT_EQ(names.mNameVariants[0].mSources, unnamed.mMetadata.mSources);
    EXPECT_EQ(names.mNameVariants[1].mSources, named.mMetadata.mSources);
}

TEST(StatementMergerIntegrationTest, CompletesMissingTradeNameInTwoFileLayout) {
    const IncomeCsvRow unnamedRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow namedRow{
        .mDatetime = "2024-01-16T10:00:00.000Z",
        .mDate = "2024-01-16",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "SELL",
        .mAssetClass = "STOCK",
        .mName = "Supplied Evidence Name",
        .mSymbol = "XX9000000248",
        .mShares = "-1.000000000",
        .mPrice = "12.0000",
        .mAmount = "12.0000",
        .mFee = "-0.2000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-sale",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto file0 = files.write("part-0.csv", {unnamedRow});
    const auto file1 = files.write("part-1.csv", {namedRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 1, .mParseResult = parser.parse(file1, 1)},
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(file0, 0)},
    };

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Supplied Evidence Name");
    ASSERT_EQ(instrument.mTransactions.size(), 2U);

    const auto& unnamed = instrument.mTransactions[0];
    const auto& named = instrument.mTransactions[1];

    EXPECT_EQ(instrument.mAssetClass, AssetClass::Stock);
    EXPECT_EQ(unnamed.mMetadata.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(unnamed.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-15T10:00:00.000Z"));
    EXPECT_FALSE(unnamed.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(unnamed.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mFilename.value(), "part-0.csv");
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(unnamed.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mSource, unnamed.mMetadata.mSources[0]);
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mName, "");
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(unnamed.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(unnamed.mUnits, 200000000);  // Source 2.000000000 units at eight decimal places.
    EXPECT_EQ(unnamed.mUnitPrice, 100000); // Source EUR 10.0000 at four decimal places.
    EXPECT_EQ(unnamed.mAmount, 200000);
    EXPECT_EQ(unnamed.mFeePaid, 1000);
    EXPECT_EQ(unnamed.mCurrency, Currency::EUR);
    EXPECT_EQ(unnamed.mExchangeRate, 100'000'000);
    ASSERT_EQ(unnamed.mUnitEvidence.size(), 1U);
    EXPECT_EQ(unnamed.mUnitEvidence[0].mSourceText, "2.000000000");
    EXPECT_EQ(unnamed.mUnitEvidence[0].mSource, unnamed.mMetadata.mSources[0]);

    EXPECT_EQ(named.mMetadata.mTaxDate, parseCalendarDate("2024-01-16"));
    EXPECT_EQ(named.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-16T10:00:00.000Z"));
    EXPECT_FALSE(named.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(named.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(named.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(named.mMetadata.mSources[0].mFilename.value(), "part-1.csv");
    EXPECT_EQ(named.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(named.mMetadata.mSources[0].mTransactionId, "synthetic-sale");
    EXPECT_EQ(named.mMetadata.mSources[0].mInputSequence, (StableInputSequence{1, 0}));
    ASSERT_EQ(named.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(named.mInstrumentEvidence[0].mSource, named.mMetadata.mSources[0]);
    EXPECT_EQ(named.mInstrumentEvidence[0].mName, "Supplied Evidence Name");
    EXPECT_EQ(named.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(named.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(named.mTradeSide, TradeSide::Sell);
    EXPECT_EQ(named.mUnits, 100000000);  // Source 1.000000000 units at eight decimal places.
    EXPECT_EQ(named.mUnitPrice, 120000); // Source EUR 12.0000 at four decimal places.
    EXPECT_EQ(named.mAmount, 120000);
    EXPECT_EQ(named.mFeePaid, 2000);
    EXPECT_EQ(named.mCurrency, Currency::EUR);
    EXPECT_EQ(named.mExchangeRate, 100'000'000);
    ASSERT_EQ(named.mUnitEvidence.size(), 1U);
    EXPECT_EQ(named.mUnitEvidence[0].mSourceText, "-1.000000000");
    EXPECT_EQ(named.mUnitEvidence[0].mSource, named.mMetadata.mSources[0]);

    ASSERT_EQ(result.mDiagnostics.size(), 2U);

    const auto& missing = std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]);
    const auto& names = std::get<MergeDiagnostic>(result.mDiagnostics[1]);

    EXPECT_EQ(missing.mDiagnostic.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(missing.mDiagnostic.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(missing.mDiagnostic.mField, "name");
    EXPECT_EQ(missing.mDiagnostic.mRowIndex, 2U);
    EXPECT_EQ(names.mCode, MergeDiagnosticCode::InstrumentNameConflict);
    EXPECT_EQ(names.mSeverity, DiagnosticSeverity::Warning);
    ASSERT_EQ(names.mNameVariants.size(), 2U);
    EXPECT_EQ(names.mNameVariants[0].mName, "");
    EXPECT_EQ(names.mNameVariants[1].mName, "Supplied Evidence Name");
    EXPECT_EQ(names.mNameVariants[0].mSources, unnamed.mMetadata.mSources);
    EXPECT_EQ(names.mNameVariants[1].mSources, named.mMetadata.mSources);
}

TEST(StatementMergerIntegrationTest, CompletesMissingActionNameInOneFileLayout) {
    const IncomeCsvRow unnamedRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "",
        .mSymbol = "XX9000000248",
        .mShares = "4.000000000",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-action-one",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow namedRow{
        .mDatetime = "2024-01-16T10:00:00.000Z",
        .mDate = "2024-01-16",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "Supplied Evidence Name",
        .mSymbol = "XX9000000248",
        .mShares = "4.000000000",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-action-two",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto history = files.write("history.csv", {unnamedRow, namedRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(history, 0)}};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Supplied Evidence Name");
    ASSERT_EQ(instrument.mCorporateActions.size(), 2U);

    const auto& unnamed = instrument.mCorporateActions[0];
    const auto& named = instrument.mCorporateActions[1];

    EXPECT_EQ(unnamed.mMetadata.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(unnamed.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-15T10:00:00.000Z"));
    EXPECT_FALSE(unnamed.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(unnamed.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mTransactionId, "synthetic-action-one");
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(unnamed.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mSource, unnamed.mMetadata.mSources[0]);
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mName, "");
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(unnamed.mType, CorporateActionType::UnresolvedSplit);
    EXPECT_EQ(unnamed.mUnitsDelta, 400'000'000); // Source 4 units, not a verified split change.
    EXPECT_FALSE(unnamed.mRatio.has_value());
    ASSERT_EQ(unnamed.mUnitEvidence.size(), 1U);
    EXPECT_EQ(unnamed.mUnitEvidence[0].mSourceText, "4.000000000");
    EXPECT_EQ(unnamed.mUnitEvidence[0].mSource, unnamed.mMetadata.mSources[0]);

    EXPECT_EQ(named.mMetadata.mTaxDate, parseCalendarDate("2024-01-16"));
    EXPECT_EQ(named.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-16T10:00:00.000Z"));
    EXPECT_FALSE(named.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(named.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(named.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(named.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(named.mMetadata.mSources[0].mSourceRow, 3U);
    EXPECT_EQ(named.mMetadata.mSources[0].mTransactionId, "synthetic-action-two");
    EXPECT_EQ(named.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 1}));
    ASSERT_EQ(named.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(named.mInstrumentEvidence[0].mSource, named.mMetadata.mSources[0]);
    EXPECT_EQ(named.mInstrumentEvidence[0].mName, "Supplied Evidence Name");
    EXPECT_EQ(named.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(named.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(named.mType, CorporateActionType::UnresolvedSplit);
    EXPECT_EQ(named.mUnitsDelta, 400'000'000); // Source 4 units, not a verified split change.
    EXPECT_FALSE(named.mRatio.has_value());
    ASSERT_EQ(named.mUnitEvidence.size(), 1U);
    EXPECT_EQ(named.mUnitEvidence[0].mSourceText, "4.000000000");
    EXPECT_EQ(named.mUnitEvidence[0].mSource, named.mMetadata.mSources[0]);

    ASSERT_EQ(result.mDiagnostics.size(), 2U);

    const auto& missing = std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]);
    const auto& names = std::get<MergeDiagnostic>(result.mDiagnostics[1]);

    EXPECT_EQ(missing.mDiagnostic.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(missing.mDiagnostic.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(missing.mDiagnostic.mField, "name");
    EXPECT_EQ(missing.mDiagnostic.mRowIndex, 2U);
    EXPECT_EQ(names.mCode, MergeDiagnosticCode::InstrumentNameConflict);
    EXPECT_EQ(names.mSeverity, DiagnosticSeverity::Warning);
    ASSERT_EQ(names.mNameVariants.size(), 2U);
    EXPECT_EQ(names.mNameVariants[0].mName, "");
    EXPECT_EQ(names.mNameVariants[1].mName, "Supplied Evidence Name");
    EXPECT_EQ(names.mNameVariants[0].mSources, unnamed.mMetadata.mSources);
    EXPECT_EQ(names.mNameVariants[1].mSources, named.mMetadata.mSources);
}

TEST(StatementMergerIntegrationTest, CompletesMissingActionNameInTwoFileLayout) {
    const IncomeCsvRow unnamedRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "",
        .mSymbol = "XX9000000248",
        .mShares = "4.000000000",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-action-one",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow namedRow{
        .mDatetime = "2024-01-16T10:00:00.000Z",
        .mDate = "2024-01-16",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "STOCK",
        .mName = "Supplied Evidence Name",
        .mSymbol = "XX9000000248",
        .mShares = "4.000000000",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-action-two",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto file0 = files.write("part-0.csv", {unnamedRow});
    const auto file1 = files.write("part-1.csv", {namedRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 1, .mParseResult = parser.parse(file1, 1)},
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(file0, 0)},
    };

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Supplied Evidence Name");
    ASSERT_EQ(instrument.mCorporateActions.size(), 2U);

    const auto& unnamed = instrument.mCorporateActions[0];
    const auto& named = instrument.mCorporateActions[1];

    EXPECT_EQ(unnamed.mMetadata.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(unnamed.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-15T10:00:00.000Z"));
    EXPECT_FALSE(unnamed.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(unnamed.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mFilename.value(), "part-0.csv");
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mTransactionId, "synthetic-action-one");
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(unnamed.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mSource, unnamed.mMetadata.mSources[0]);
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mName, "");
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(unnamed.mType, CorporateActionType::UnresolvedSplit);
    EXPECT_EQ(unnamed.mUnitsDelta, 400'000'000); // Source 4 units, not a verified split change.
    EXPECT_FALSE(unnamed.mRatio.has_value());
    ASSERT_EQ(unnamed.mUnitEvidence.size(), 1U);
    EXPECT_EQ(unnamed.mUnitEvidence[0].mSourceText, "4.000000000");
    EXPECT_EQ(unnamed.mUnitEvidence[0].mSource, unnamed.mMetadata.mSources[0]);

    EXPECT_EQ(named.mMetadata.mTaxDate, parseCalendarDate("2024-01-16"));
    EXPECT_EQ(named.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-16T10:00:00.000Z"));
    EXPECT_FALSE(named.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(named.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(named.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(named.mMetadata.mSources[0].mFilename.value(), "part-1.csv");
    EXPECT_EQ(named.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(named.mMetadata.mSources[0].mTransactionId, "synthetic-action-two");
    EXPECT_EQ(named.mMetadata.mSources[0].mInputSequence, (StableInputSequence{1, 0}));
    ASSERT_EQ(named.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(named.mInstrumentEvidence[0].mSource, named.mMetadata.mSources[0]);
    EXPECT_EQ(named.mInstrumentEvidence[0].mName, "Supplied Evidence Name");
    EXPECT_EQ(named.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(named.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(named.mType, CorporateActionType::UnresolvedSplit);
    EXPECT_EQ(named.mUnitsDelta, 400'000'000); // Source 4 units, not a verified split change.
    EXPECT_FALSE(named.mRatio.has_value());
    ASSERT_EQ(named.mUnitEvidence.size(), 1U);
    EXPECT_EQ(named.mUnitEvidence[0].mSourceText, "4.000000000");
    EXPECT_EQ(named.mUnitEvidence[0].mSource, named.mMetadata.mSources[0]);

    ASSERT_EQ(result.mDiagnostics.size(), 2U);

    const auto& missing = std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]);
    const auto& names = std::get<MergeDiagnostic>(result.mDiagnostics[1]);

    EXPECT_EQ(missing.mDiagnostic.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(missing.mDiagnostic.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(missing.mDiagnostic.mField, "name");
    EXPECT_EQ(missing.mDiagnostic.mRowIndex, 2U);
    EXPECT_EQ(names.mCode, MergeDiagnosticCode::InstrumentNameConflict);
    EXPECT_EQ(names.mSeverity, DiagnosticSeverity::Warning);
    ASSERT_EQ(names.mNameVariants.size(), 2U);
    EXPECT_EQ(names.mNameVariants[0].mName, "");
    EXPECT_EQ(names.mNameVariants[1].mName, "Supplied Evidence Name");
    EXPECT_EQ(names.mNameVariants[0].mSources, unnamed.mMetadata.mSources);
    EXPECT_EQ(names.mNameVariants[1].mSources, named.mMetadata.mSources);
}

TEST(StatementMergerIntegrationTest, CompletesMissingDividendNameInOneFileLayout) {
    const IncomeCsvRow unnamedRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "DIVIDEND",
        .mAssetClass = "STOCK",
        .mName = "",
        .mSymbol = "XX9000000248",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.0000",
        .mFee = "",
        .mTax = "-1.0000",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-income-one",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow namedRow{
        .mDatetime = "2024-01-16T10:00:00.000Z",
        .mDate = "2024-01-16",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "DIVIDEND",
        .mAssetClass = "STOCK",
        .mName = "Supplied Evidence Name",
        .mSymbol = "XX9000000248",
        .mShares = "",
        .mPrice = "",
        .mAmount = "12.0000",
        .mFee = "",
        .mTax = "-2.0000",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-income-two",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto history = files.write("history.csv", {unnamedRow, namedRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(history, 0)}};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mDividendInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mDividendInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Supplied Evidence Name");
    ASSERT_EQ(instrument.mTransactions.size(), 2U);

    const auto& unnamed = instrument.mTransactions[0];
    const auto& named = instrument.mTransactions[1];

    EXPECT_EQ(unnamed.mMetadata.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(unnamed.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-15T10:00:00.000Z"));
    EXPECT_FALSE(unnamed.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(unnamed.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mTransactionId, "synthetic-income-one");
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(unnamed.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mSource, unnamed.mMetadata.mSources[0]);
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mName, "");
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");

    EXPECT_EQ(named.mMetadata.mTaxDate, parseCalendarDate("2024-01-16"));
    EXPECT_EQ(named.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-16T10:00:00.000Z"));
    EXPECT_FALSE(named.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(named.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(named.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(named.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(named.mMetadata.mSources[0].mSourceRow, 3U);
    EXPECT_EQ(named.mMetadata.mSources[0].mTransactionId, "synthetic-income-two");
    EXPECT_EQ(named.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 1}));
    ASSERT_EQ(named.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(named.mInstrumentEvidence[0].mSource, named.mMetadata.mSources[0]);
    EXPECT_EQ(named.mInstrumentEvidence[0].mName, "Supplied Evidence Name");
    EXPECT_EQ(named.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(named.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(unnamed.mGrossAmount, 100'000); // EUR 10 at four decimal places.
    EXPECT_EQ(named.mGrossAmount, 120'000);   // EUR 12 at four decimal places.
    EXPECT_EQ(unnamed.mTaxPaid, 10'000);
    EXPECT_EQ(named.mTaxPaid, 20'000);

    ASSERT_EQ(result.mDiagnostics.size(), 2U);

    const auto& missing = std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]);
    const auto& names = std::get<MergeDiagnostic>(result.mDiagnostics[1]);

    EXPECT_EQ(missing.mDiagnostic.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(missing.mDiagnostic.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(missing.mDiagnostic.mField, "name");
    EXPECT_EQ(missing.mDiagnostic.mRowIndex, 2U);
    EXPECT_EQ(names.mCode, MergeDiagnosticCode::InstrumentNameConflict);
    EXPECT_EQ(names.mSeverity, DiagnosticSeverity::Warning);
    ASSERT_EQ(names.mNameVariants.size(), 2U);
    EXPECT_EQ(names.mNameVariants[0].mName, "");
    EXPECT_EQ(names.mNameVariants[1].mName, "Supplied Evidence Name");
    EXPECT_EQ(names.mNameVariants[0].mSources, unnamed.mMetadata.mSources);
    EXPECT_EQ(names.mNameVariants[1].mSources, named.mMetadata.mSources);
}

TEST(StatementMergerIntegrationTest, CompletesMissingDividendNameInTwoFileLayout) {
    const IncomeCsvRow unnamedRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "DIVIDEND",
        .mAssetClass = "STOCK",
        .mName = "",
        .mSymbol = "XX9000000248",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.0000",
        .mFee = "",
        .mTax = "-1.0000",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-income-one",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow namedRow{
        .mDatetime = "2024-01-16T10:00:00.000Z",
        .mDate = "2024-01-16",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "DIVIDEND",
        .mAssetClass = "STOCK",
        .mName = "Supplied Evidence Name",
        .mSymbol = "XX9000000248",
        .mShares = "",
        .mPrice = "",
        .mAmount = "12.0000",
        .mFee = "",
        .mTax = "-2.0000",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-income-two",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto file0 = files.write("part-0.csv", {unnamedRow});
    const auto file1 = files.write("part-1.csv", {namedRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 1, .mParseResult = parser.parse(file1, 1)},
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(file0, 0)},
    };

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mDividendInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mDividendInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Supplied Evidence Name");
    ASSERT_EQ(instrument.mTransactions.size(), 2U);

    const auto& unnamed = instrument.mTransactions[0];
    const auto& named = instrument.mTransactions[1];

    EXPECT_EQ(unnamed.mMetadata.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(unnamed.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-15T10:00:00.000Z"));
    EXPECT_FALSE(unnamed.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(unnamed.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mFilename.value(), "part-0.csv");
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mTransactionId, "synthetic-income-one");
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(unnamed.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mSource, unnamed.mMetadata.mSources[0]);
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mName, "");
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");

    EXPECT_EQ(named.mMetadata.mTaxDate, parseCalendarDate("2024-01-16"));
    EXPECT_EQ(named.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-16T10:00:00.000Z"));
    EXPECT_FALSE(named.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(named.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(named.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(named.mMetadata.mSources[0].mFilename.value(), "part-1.csv");
    EXPECT_EQ(named.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(named.mMetadata.mSources[0].mTransactionId, "synthetic-income-two");
    EXPECT_EQ(named.mMetadata.mSources[0].mInputSequence, (StableInputSequence{1, 0}));
    ASSERT_EQ(named.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(named.mInstrumentEvidence[0].mSource, named.mMetadata.mSources[0]);
    EXPECT_EQ(named.mInstrumentEvidence[0].mName, "Supplied Evidence Name");
    EXPECT_EQ(named.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(named.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(unnamed.mGrossAmount, 100'000); // EUR 10 at four decimal places.
    EXPECT_EQ(named.mGrossAmount, 120'000);   // EUR 12 at four decimal places.
    EXPECT_EQ(unnamed.mTaxPaid, 10'000);
    EXPECT_EQ(named.mTaxPaid, 20'000);

    ASSERT_EQ(result.mDiagnostics.size(), 2U);

    const auto& missing = std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]);
    const auto& names = std::get<MergeDiagnostic>(result.mDiagnostics[1]);

    EXPECT_EQ(missing.mDiagnostic.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(missing.mDiagnostic.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(missing.mDiagnostic.mField, "name");
    EXPECT_EQ(missing.mDiagnostic.mRowIndex, 2U);
    EXPECT_EQ(names.mCode, MergeDiagnosticCode::InstrumentNameConflict);
    EXPECT_EQ(names.mSeverity, DiagnosticSeverity::Warning);
    ASSERT_EQ(names.mNameVariants.size(), 2U);
    EXPECT_EQ(names.mNameVariants[0].mName, "");
    EXPECT_EQ(names.mNameVariants[1].mName, "Supplied Evidence Name");
    EXPECT_EQ(names.mNameVariants[0].mSources, unnamed.mMetadata.mSources);
    EXPECT_EQ(names.mNameVariants[1].mSources, named.mMetadata.mSources);
}

TEST(StatementMergerIntegrationTest, CompletesMissingBondInterestNameInOneFileLayout) {
    const IncomeCsvRow unnamedRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "BOND",
        .mName = "",
        .mSymbol = "XX9000000248",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.0000",
        .mFee = "",
        .mTax = "-1.0000",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-income-one",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow namedRow{
        .mDatetime = "2024-01-16T10:00:00.000Z",
        .mDate = "2024-01-16",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "BOND",
        .mName = "Supplied Evidence Name",
        .mSymbol = "XX9000000248",
        .mShares = "",
        .mPrice = "",
        .mAmount = "12.0000",
        .mFee = "",
        .mTax = "-2.0000",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-income-two",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto history = files.write("history.csv", {unnamedRow, namedRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(history, 0)}};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mInterestInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mInterestInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Supplied Evidence Name");
    ASSERT_EQ(instrument.mTransactions.size(), 2U);

    const auto& unnamed = instrument.mTransactions[0];
    const auto& named = instrument.mTransactions[1];

    EXPECT_EQ(instrument.mInterestType, InterestType::BondInterest);
    EXPECT_EQ(unnamed.mMetadata.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(unnamed.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-15T10:00:00.000Z"));
    EXPECT_FALSE(unnamed.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(unnamed.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mTransactionId, "synthetic-income-one");
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(unnamed.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mSource, unnamed.mMetadata.mSources[0]);
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mName, "");
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mAssetClass, AssetClass::Bond);
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mAssetClassLabel, "BOND");

    EXPECT_EQ(named.mMetadata.mTaxDate, parseCalendarDate("2024-01-16"));
    EXPECT_EQ(named.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-16T10:00:00.000Z"));
    EXPECT_FALSE(named.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(named.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(named.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(named.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(named.mMetadata.mSources[0].mSourceRow, 3U);
    EXPECT_EQ(named.mMetadata.mSources[0].mTransactionId, "synthetic-income-two");
    EXPECT_EQ(named.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 1}));
    ASSERT_EQ(named.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(named.mInstrumentEvidence[0].mSource, named.mMetadata.mSources[0]);
    EXPECT_EQ(named.mInstrumentEvidence[0].mName, "Supplied Evidence Name");
    EXPECT_EQ(named.mInstrumentEvidence[0].mAssetClass, AssetClass::Bond);
    EXPECT_EQ(named.mInstrumentEvidence[0].mAssetClassLabel, "BOND");
    EXPECT_EQ(unnamed.mGrossAmount, 100'000); // EUR 10 at four decimal places.
    EXPECT_EQ(named.mGrossAmount, 120'000);   // EUR 12 at four decimal places.
    EXPECT_EQ(unnamed.mTaxPaid, 10'000);
    EXPECT_EQ(named.mTaxPaid, 20'000);

    ASSERT_EQ(result.mDiagnostics.size(), 2U);

    const auto& missing = std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]);
    const auto& names = std::get<MergeDiagnostic>(result.mDiagnostics[1]);

    EXPECT_EQ(missing.mDiagnostic.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(missing.mDiagnostic.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(missing.mDiagnostic.mField, "name");
    EXPECT_EQ(missing.mDiagnostic.mRowIndex, 2U);
    EXPECT_EQ(names.mCode, MergeDiagnosticCode::InstrumentNameConflict);
    EXPECT_EQ(names.mSeverity, DiagnosticSeverity::Warning);
    ASSERT_EQ(names.mNameVariants.size(), 2U);
    EXPECT_EQ(names.mNameVariants[0].mName, "");
    EXPECT_EQ(names.mNameVariants[1].mName, "Supplied Evidence Name");
    EXPECT_EQ(names.mNameVariants[0].mSources, unnamed.mMetadata.mSources);
    EXPECT_EQ(names.mNameVariants[1].mSources, named.mMetadata.mSources);
}

TEST(StatementMergerIntegrationTest, CompletesMissingBondInterestNameInTwoFileLayout) {
    const IncomeCsvRow unnamedRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "BOND",
        .mName = "",
        .mSymbol = "XX9000000248",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.0000",
        .mFee = "",
        .mTax = "-1.0000",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-income-one",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow namedRow{
        .mDatetime = "2024-01-16T10:00:00.000Z",
        .mDate = "2024-01-16",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "BOND",
        .mName = "Supplied Evidence Name",
        .mSymbol = "XX9000000248",
        .mShares = "",
        .mPrice = "",
        .mAmount = "12.0000",
        .mFee = "",
        .mTax = "-2.0000",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-income-two",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto file0 = files.write("part-0.csv", {unnamedRow});
    const auto file1 = files.write("part-1.csv", {namedRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 1, .mParseResult = parser.parse(file1, 1)},
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(file0, 0)},
    };

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mInterestInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mInterestInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Supplied Evidence Name");
    ASSERT_EQ(instrument.mTransactions.size(), 2U);

    const auto& unnamed = instrument.mTransactions[0];
    const auto& named = instrument.mTransactions[1];

    EXPECT_EQ(instrument.mInterestType, InterestType::BondInterest);
    EXPECT_EQ(unnamed.mMetadata.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(unnamed.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-15T10:00:00.000Z"));
    EXPECT_FALSE(unnamed.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(unnamed.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mFilename.value(), "part-0.csv");
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mTransactionId, "synthetic-income-one");
    EXPECT_EQ(unnamed.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(unnamed.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mSource, unnamed.mMetadata.mSources[0]);
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mName, "");
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mAssetClass, AssetClass::Bond);
    EXPECT_EQ(unnamed.mInstrumentEvidence[0].mAssetClassLabel, "BOND");

    EXPECT_EQ(named.mMetadata.mTaxDate, parseCalendarDate("2024-01-16"));
    EXPECT_EQ(named.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-16T10:00:00.000Z"));
    EXPECT_FALSE(named.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(named.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(named.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(named.mMetadata.mSources[0].mFilename.value(), "part-1.csv");
    EXPECT_EQ(named.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(named.mMetadata.mSources[0].mTransactionId, "synthetic-income-two");
    EXPECT_EQ(named.mMetadata.mSources[0].mInputSequence, (StableInputSequence{1, 0}));
    ASSERT_EQ(named.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(named.mInstrumentEvidence[0].mSource, named.mMetadata.mSources[0]);
    EXPECT_EQ(named.mInstrumentEvidence[0].mName, "Supplied Evidence Name");
    EXPECT_EQ(named.mInstrumentEvidence[0].mAssetClass, AssetClass::Bond);
    EXPECT_EQ(named.mInstrumentEvidence[0].mAssetClassLabel, "BOND");
    EXPECT_EQ(unnamed.mGrossAmount, 100'000); // EUR 10 at four decimal places.
    EXPECT_EQ(named.mGrossAmount, 120'000);   // EUR 12 at four decimal places.
    EXPECT_EQ(unnamed.mTaxPaid, 10'000);
    EXPECT_EQ(named.mTaxPaid, 20'000);

    ASSERT_EQ(result.mDiagnostics.size(), 2U);

    const auto& missing = std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]);
    const auto& names = std::get<MergeDiagnostic>(result.mDiagnostics[1]);

    EXPECT_EQ(missing.mDiagnostic.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(missing.mDiagnostic.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(missing.mDiagnostic.mField, "name");
    EXPECT_EQ(missing.mDiagnostic.mRowIndex, 2U);
    EXPECT_EQ(names.mCode, MergeDiagnosticCode::InstrumentNameConflict);
    EXPECT_EQ(names.mSeverity, DiagnosticSeverity::Warning);
    ASSERT_EQ(names.mNameVariants.size(), 2U);
    EXPECT_EQ(names.mNameVariants[0].mName, "");
    EXPECT_EQ(names.mNameVariants[1].mName, "Supplied Evidence Name");
    EXPECT_EQ(names.mNameVariants[0].mSources, unnamed.mMetadata.mSources);
    EXPECT_EQ(names.mNameVariants[1].mSources, named.mMetadata.mSources);
}

TEST(StatementMergerIntegrationTest, DoesNotUseAnotherIsinsNameForAnUnnamedTrade) {
    const IncomeCsvRow unnamedRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow otherIsinRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Other Instrument",
        .mSymbol = "XX9000000249",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-other-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto history = files.write("history.csv", {unnamedRow, otherIsinRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(history, 0)}};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 2U);

    const auto& unnamedInstrument = result.mStatement.mPresentation.mTradeInstruments[0];

    EXPECT_EQ(unnamedInstrument.mIsin, "XX9000000248");
    EXPECT_EQ(unnamedInstrument.mName, "");
    ASSERT_EQ(unnamedInstrument.mTransactions.size(), 1U);
    EXPECT_EQ(unnamedInstrument.mTransactions[0].mUnits, 200'000'000); // 2 units at eight decimals.
    EXPECT_EQ(unnamedInstrument.mTransactions[0].mAmount, 200'000);    // EUR 20 at four decimals.
    ASSERT_EQ(unnamedInstrument.mTransactions[0].mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(unnamedInstrument.mTransactions[0].mInstrumentEvidence[0].mName, "");
    EXPECT_EQ(unnamedInstrument.mTransactions[0].mInstrumentEvidence[0].mSource.mSourceRow, 2U);

    const auto& namedInstrument = result.mStatement.mPresentation.mTradeInstruments[1];

    EXPECT_EQ(namedInstrument.mIsin, "XX9000000249");
    EXPECT_EQ(namedInstrument.mName, "Other Instrument");
    ASSERT_EQ(namedInstrument.mTransactions.size(), 1U);
    ASSERT_EQ(namedInstrument.mTransactions[0].mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(namedInstrument.mTransactions[0].mInstrumentEvidence[0].mName, "Other Instrument");
    EXPECT_EQ(namedInstrument.mTransactions[0].mInstrumentEvidence[0].mSource.mSourceRow, 3U);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& diagnostic = std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]).mDiagnostic;

    EXPECT_EQ(diagnostic.mSeverity, DiagnosticSeverity::Warning);
    EXPECT_EQ(diagnostic.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(diagnostic.mField, "name");
    EXPECT_EQ(diagnostic.mRowIndex, 2U);
}

TEST(StatementMergerIntegrationTest, LeavesNameEmptyWhenEveryTradeNameIsMissing) {
    const IncomeCsvRow unnamedRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto history = files.write("history.csv", {unnamedRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(history, 0)}};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments[0];

    EXPECT_EQ(instrument.mName, "");
    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    ASSERT_EQ(instrument.mTransactions.size(), 1U);
    ASSERT_EQ(instrument.mTransactions[0].mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(instrument.mTransactions[0].mInstrumentEvidence[0].mName, "");
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]).mDiagnostic.mSeverity,
              DiagnosticSeverity::Warning);
}

TEST(StatementMergerIntegrationTest, LeavesNameEmptyWhenEveryDividendNameIsMissing) {
    const IncomeCsvRow unnamedRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "DIVIDEND",
        .mAssetClass = "STOCK",
        .mName = "",
        .mSymbol = "XX9000000248",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.0000",
        .mFee = "",
        .mTax = "-1.0000",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto history = files.write("history.csv", {unnamedRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(history, 0)}};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mDividendInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mDividendInstruments[0];

    EXPECT_EQ(instrument.mName, "");
    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    ASSERT_EQ(instrument.mTransactions.size(), 1U);
    ASSERT_EQ(instrument.mTransactions[0].mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(instrument.mTransactions[0].mInstrumentEvidence[0].mName, "");
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]).mDiagnostic.mSeverity,
              DiagnosticSeverity::Warning);
}

TEST(StatementMergerIntegrationTest, LeavesNameEmptyWhenEveryBondInterestNameIsMissing) {
    const IncomeCsvRow unnamedRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "CASH",
        .mType = "INTEREST_PAYMENT",
        .mAssetClass = "BOND",
        .mName = "",
        .mSymbol = "XX9000000248",
        .mShares = "",
        .mPrice = "",
        .mAmount = "10.0000",
        .mFee = "",
        .mTax = "-1.0000",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto history = files.write("history.csv", {unnamedRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(history, 0)}};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mInterestInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mInterestInstruments[0];

    EXPECT_EQ(instrument.mName, "");
    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    ASSERT_EQ(instrument.mTransactions.size(), 1U);
    ASSERT_EQ(instrument.mTransactions[0].mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(instrument.mTransactions[0].mInstrumentEvidence[0].mName, "");
    ASSERT_EQ(result.mDiagnostics.size(), 1U);
    EXPECT_EQ(std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]).mDiagnostic.mSeverity,
              DiagnosticSeverity::Warning);
}

TEST(StatementMergerIntegrationTest, PreservesEveryNameVariantWithinOneFile) {
    const IncomeCsvRow firstRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "First Source Name",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow secondRow{
        .mDatetime = "2024-01-16T10:00:00.000Z",
        .mDate = "2024-01-16",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "SELL",
        .mAssetClass = "STOCK",
        .mName = "Second Source Name",
        .mSymbol = "XX9000000248",
        .mShares = "-1.000000000",
        .mPrice = "12.0000",
        .mAmount = "12.0000",
        .mFee = "-0.2000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-sale",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto history = files.write("history.csv", {firstRow, secondRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(history, 0)}};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "First Source Name");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Stock);
    ASSERT_EQ(instrument.mTransactions.size(), 2U);
    ASSERT_EQ(instrument.mCorporateActions.size(), 0U);
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 2U);

    const auto& first = instrument.mTransactions[0];
    const auto& second = instrument.mTransactions[1];

    EXPECT_EQ(first.mMetadata.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(first.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-15T10:00:00.000Z"));
    EXPECT_FALSE(first.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(first.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(first.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(first.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(first.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(first.mMetadata.mSources[0].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(first.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(first.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(first.mInstrumentEvidence[0].mSource, first.mMetadata.mSources[0]);
    EXPECT_EQ(first.mInstrumentEvidence[0].mName, "First Source Name");
    EXPECT_EQ(first.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(first.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(first.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(first.mUnits, 200000000);  // Source 2.000000000 units at eight decimal places.
    EXPECT_EQ(first.mUnitPrice, 100000); // Source EUR 10.0000 at four decimal places.
    EXPECT_EQ(first.mAmount, 200000);
    EXPECT_EQ(first.mFeePaid, 1000);
    EXPECT_EQ(first.mCurrency, Currency::EUR);
    EXPECT_EQ(first.mExchangeRate, 100'000'000);
    ASSERT_EQ(first.mUnitEvidence.size(), 1U);
    EXPECT_EQ(first.mUnitEvidence[0].mSourceText, "2.000000000");
    EXPECT_EQ(first.mUnitEvidence[0].mSource, first.mMetadata.mSources[0]);

    EXPECT_EQ(second.mMetadata.mTaxDate, parseCalendarDate("2024-01-16"));
    EXPECT_EQ(second.mMetadata.mSourceTimestamp, parseSourceTimestamp("2024-01-16T10:00:00.000Z"));
    EXPECT_FALSE(second.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(second.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(second.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(second.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(second.mMetadata.mSources[0].mSourceRow, 3U);
    EXPECT_EQ(second.mMetadata.mSources[0].mTransactionId, "synthetic-sale");
    EXPECT_EQ(second.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 1}));
    ASSERT_EQ(second.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(second.mInstrumentEvidence[0].mSource, second.mMetadata.mSources[0]);
    EXPECT_EQ(second.mInstrumentEvidence[0].mName, "Second Source Name");
    EXPECT_EQ(second.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(second.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(second.mTradeSide, TradeSide::Sell);
    EXPECT_EQ(second.mUnits, 100000000);  // Source 1.000000000 units at eight decimal places.
    EXPECT_EQ(second.mUnitPrice, 120000); // Source EUR 12.0000 at four decimal places.
    EXPECT_EQ(second.mAmount, 120000);
    EXPECT_EQ(second.mFeePaid, 2000);
    EXPECT_EQ(second.mCurrency, Currency::EUR);
    EXPECT_EQ(second.mExchangeRate, 100'000'000);
    ASSERT_EQ(second.mUnitEvidence.size(), 1U);
    EXPECT_EQ(second.mUnitEvidence[0].mSourceText, "-1.000000000");
    EXPECT_EQ(second.mUnitEvidence[0].mSource, second.mMetadata.mSources[0]);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& names = std::get<MergeDiagnostic>(result.mDiagnostics[0]);

    EXPECT_EQ(names.mCode, MergeDiagnosticCode::InstrumentNameConflict);
    EXPECT_EQ(names.mSeverity, DiagnosticSeverity::Warning);
    ASSERT_EQ(names.mNameVariants.size(), 2U);
    EXPECT_EQ(names.mNameVariants[0].mName, "First Source Name");
    EXPECT_EQ(names.mNameVariants[1].mName, "Second Source Name");
    EXPECT_EQ(names.mNameVariants[0].mSources, first.mMetadata.mSources);
    EXPECT_EQ(names.mNameVariants[1].mSources, second.mMetadata.mSources);
}

TEST(StatementMergerIntegrationTest, OverlappingExportsRetainAllOriginalClassSources) {
    const IncomeCsvRow purchaseRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow saleRow{
        .mDatetime = "2024-01-16T10:00:00.000Z",
        .mDate = "2024-01-16",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "SELL",
        .mAssetClass = "FUND",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "-1.000000000",
        .mPrice = "12.0000",
        .mAmount = "12.0000",
        .mFee = "-0.2000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-sale",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;
    const auto first = files.write("first.csv", {purchaseRow, saleRow});
    const auto second = files.write("second.csv", {purchaseRow, saleRow});
    const auto third = files.write("third.csv", {purchaseRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 2, .mParseResult = parser.parse(third, 2)},
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(first, 0)},
        StatementMergeInput{.mSourceIndex = 1, .mParseResult = parser.parse(second, 1)},
    };

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Synthetic Evidence Share");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Unknown);
    ASSERT_EQ(instrument.mTransactions.size(), 2U);
    ASSERT_EQ(instrument.mCorporateActions.size(), 0U);
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 2U);

    const auto& purchase = instrument.mTransactions[0];
    const auto& sale = instrument.mTransactions[1];

    EXPECT_EQ(purchase.mUnits, 200'000'000); // Source 2 units at eight decimal places.
    EXPECT_EQ(purchase.mUnitPrice, 100'000); // EUR 10 at four decimal places.
    EXPECT_EQ(purchase.mAmount, 200'000);
    EXPECT_EQ(sale.mUnits, 100'000'000);
    EXPECT_EQ(sale.mUnitPrice, 120'000);
    EXPECT_EQ(sale.mAmount, 120'000);
    ASSERT_EQ(purchase.mMetadata.mSources.size(), 3U);
    ASSERT_EQ(purchase.mInstrumentEvidence.size(), 3U);
    ASSERT_EQ(purchase.mUnitEvidence.size(), 3U);
    ASSERT_EQ(sale.mMetadata.mSources.size(), 2U);
    ASSERT_EQ(sale.mInstrumentEvidence.size(), 2U);
    ASSERT_EQ(sale.mUnitEvidence.size(), 2U);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mFilename.value(), "first.csv");
    EXPECT_EQ(purchase.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mSource, purchase.mMetadata.mSources[0]);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(purchase.mUnitEvidence[0].mSource, purchase.mMetadata.mSources[0]);
    EXPECT_EQ(purchase.mMetadata.mSources[1].mFilename.value(), "second.csv");
    EXPECT_EQ(purchase.mMetadata.mSources[1].mSourceRow, 2U);
    EXPECT_EQ(purchase.mMetadata.mSources[1].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(purchase.mInstrumentEvidence[1].mSource, purchase.mMetadata.mSources[1]);
    EXPECT_EQ(purchase.mInstrumentEvidence[1].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(purchase.mInstrumentEvidence[1].mAssetClassLabel, "STOCK");
    EXPECT_EQ(purchase.mUnitEvidence[1].mSource, purchase.mMetadata.mSources[1]);
    EXPECT_EQ(purchase.mMetadata.mSources[2].mFilename.value(), "third.csv");
    EXPECT_EQ(purchase.mMetadata.mSources[2].mSourceRow, 2U);
    EXPECT_EQ(purchase.mMetadata.mSources[2].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(purchase.mInstrumentEvidence[2].mSource, purchase.mMetadata.mSources[2]);
    EXPECT_EQ(purchase.mInstrumentEvidence[2].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(purchase.mInstrumentEvidence[2].mAssetClassLabel, "STOCK");
    EXPECT_EQ(purchase.mUnitEvidence[2].mSource, purchase.mMetadata.mSources[2]);
    EXPECT_EQ(sale.mMetadata.mSources[0].mFilename.value(), "first.csv");
    EXPECT_EQ(sale.mMetadata.mSources[0].mSourceRow, 3U);
    EXPECT_EQ(sale.mMetadata.mSources[0].mTransactionId, "synthetic-sale");
    EXPECT_EQ(sale.mInstrumentEvidence[0].mSource, sale.mMetadata.mSources[0]);
    EXPECT_EQ(sale.mInstrumentEvidence[0].mAssetClass, AssetClass::Fund);
    EXPECT_EQ(sale.mInstrumentEvidence[0].mAssetClassLabel, "FUND");
    EXPECT_EQ(sale.mUnitEvidence[0].mSource, sale.mMetadata.mSources[0]);
    EXPECT_EQ(sale.mMetadata.mSources[1].mFilename.value(), "second.csv");
    EXPECT_EQ(sale.mMetadata.mSources[1].mSourceRow, 3U);
    EXPECT_EQ(sale.mMetadata.mSources[1].mTransactionId, "synthetic-sale");
    EXPECT_EQ(sale.mInstrumentEvidence[1].mSource, sale.mMetadata.mSources[1]);
    EXPECT_EQ(sale.mInstrumentEvidence[1].mAssetClass, AssetClass::Fund);
    EXPECT_EQ(sale.mInstrumentEvidence[1].mAssetClassLabel, "FUND");
    EXPECT_EQ(sale.mUnitEvidence[1].mSource, sale.mMetadata.mSources[1]);

    const auto conflict = std::find_if(
        result.mDiagnostics.begin(),
        result.mDiagnostics.end(),
        [](const auto& diagnostic) {
            const auto* merge = std::get_if<MergeDiagnostic>(&diagnostic);

            return merge && merge->mCode == MergeDiagnosticCode::InstrumentAssetClassConflict;
        });

    ASSERT_NE(conflict, result.mDiagnostics.end());

    const auto& error = std::get<MergeDiagnostic>(*conflict);

    EXPECT_EQ(error.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mIsin, "XX9000000248");
    ASSERT_EQ(error.mAssetClassVariants.size(), 2U);
    EXPECT_EQ(error.mAssetClassVariants[0].mAssetClass, AssetClass::Stock);
    ASSERT_EQ(error.mAssetClassVariants[0].mSources.size(), 3U);
    EXPECT_EQ(error.mAssetClassVariants[1].mAssetClass, AssetClass::Fund);
    ASSERT_EQ(error.mAssetClassVariants[1].mSources.size(), 2U);
    EXPECT_EQ(error.mAssetClassVariants[0].mSources, purchase.mMetadata.mSources);
    EXPECT_EQ(error.mAssetClassVariants[1].mSources, sale.mMetadata.mSources);
}

TEST(StatementMergerIntegrationTest,
     InvalidUnrecognizedTradeDoesNotContaminateHealthyClassification) {
    const IncomeCsvRow healthyRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow invalidRow{
        .mDatetime = "2024-01-16T10:00:00.000Z",
        .mDate = "2024-01-16",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "SELL",
        .mAssetClass = "UNLISTED",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "-1.000000000",
        .mPrice = "not-a-number",
        .mAmount = "12.0000",
        .mFee = "-0.2000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-sale",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto history = files.write("history.csv", {healthyRow, invalidRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(history, 0)}};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Synthetic Evidence Share");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Stock);
    ASSERT_EQ(instrument.mTransactions.size(), 1U);
    ASSERT_EQ(instrument.mCorporateActions.size(), 0U);
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 1U);

    const auto& purchase = instrument.mTransactions[0];

    EXPECT_EQ(purchase.mMetadata.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(purchase.mMetadata.mSourceTimestamp,
              parseSourceTimestamp("2024-01-15T10:00:00.000Z"));
    EXPECT_FALSE(purchase.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(purchase.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(purchase.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(purchase.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(purchase.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mSource, purchase.mMetadata.mSources[0]);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(purchase.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(purchase.mUnits, 200000000);  // Source 2.000000000 units at eight decimal places.
    EXPECT_EQ(purchase.mUnitPrice, 100000); // Source EUR 10.0000 at four decimal places.
    EXPECT_EQ(purchase.mAmount, 200000);
    EXPECT_EQ(purchase.mFeePaid, 1000);
    EXPECT_EQ(purchase.mCurrency, Currency::EUR);
    EXPECT_EQ(purchase.mExchangeRate, 100'000'000);
    ASSERT_EQ(purchase.mUnitEvidence.size(), 1U);
    EXPECT_EQ(purchase.mUnitEvidence[0].mSourceText, "2.000000000");
    EXPECT_EQ(purchase.mUnitEvidence[0].mSource, purchase.mMetadata.mSources[0]);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]);

    EXPECT_EQ(error.mDiagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mDiagnostic.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mDiagnostic.mField, "price");
    EXPECT_EQ(error.mDiagnostic.mSourceFile, "history.csv");
    EXPECT_EQ(error.mDiagnostic.mRowIndex, 3U);
    EXPECT_EQ(error.mDiagnostic.mTransactionId, "synthetic-sale");
}

TEST(StatementMergerIntegrationTest,
     InvalidConflictingActionDoesNotContaminateHealthyClassification) {
    const IncomeCsvRow healthyRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow invalidRow{
        .mDatetime = "2024-01-16T10:00:00.000Z",
        .mDate = "2024-01-16",
        .mAccountType = "DEFAULT",
        .mCategory = "CORPORATE_ACTION",
        .mType = "SPLIT",
        .mAssetClass = "FUND",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "not-a-number",
        .mPrice = "",
        .mAmount = "",
        .mFee = "",
        .mTax = "",
        .mCurrency = "",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-action",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto history = files.write("history.csv", {healthyRow, invalidRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(history, 0)}};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Synthetic Evidence Share");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Stock);
    ASSERT_EQ(instrument.mTransactions.size(), 1U);
    ASSERT_EQ(instrument.mCorporateActions.size(), 0U);
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 1U);

    const auto& purchase = instrument.mTransactions[0];

    EXPECT_EQ(purchase.mMetadata.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(purchase.mMetadata.mSourceTimestamp,
              parseSourceTimestamp("2024-01-15T10:00:00.000Z"));
    EXPECT_FALSE(purchase.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(purchase.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(purchase.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(purchase.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(purchase.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mSource, purchase.mMetadata.mSources[0]);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(purchase.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(purchase.mUnits, 200000000);  // Source 2.000000000 units at eight decimal places.
    EXPECT_EQ(purchase.mUnitPrice, 100000); // Source EUR 10.0000 at four decimal places.
    EXPECT_EQ(purchase.mAmount, 200000);
    EXPECT_EQ(purchase.mFeePaid, 1000);
    EXPECT_EQ(purchase.mCurrency, Currency::EUR);
    EXPECT_EQ(purchase.mExchangeRate, 100'000'000);
    ASSERT_EQ(purchase.mUnitEvidence.size(), 1U);
    EXPECT_EQ(purchase.mUnitEvidence[0].mSourceText, "2.000000000");
    EXPECT_EQ(purchase.mUnitEvidence[0].mSource, purchase.mMetadata.mSources[0]);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]);

    EXPECT_EQ(error.mDiagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mDiagnostic.mCode, DiagnosticCode::InvalidValue);
    EXPECT_EQ(error.mDiagnostic.mField, "shares");
    EXPECT_EQ(error.mDiagnostic.mSourceFile, "history.csv");
    EXPECT_EQ(error.mDiagnostic.mRowIndex, 3U);
    EXPECT_EQ(error.mDiagnostic.mTransactionId, "synthetic-action");
}

TEST(StatementMergerIntegrationTest, InvalidNamelessTradeDoesNotContaminateHealthyClassification) {
    const IncomeCsvRow healthyRow{
        .mDatetime = "2024-01-15T10:00:00.000Z",
        .mDate = "2024-01-15",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "BUY",
        .mAssetClass = "STOCK",
        .mName = "Synthetic Evidence Share",
        .mSymbol = "XX9000000248",
        .mShares = "2.000000000",
        .mPrice = "10.0000",
        .mAmount = "-20.0000",
        .mFee = "-0.1000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-purchase",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    const IncomeCsvRow invalidRow{
        .mDatetime = "2024-01-16T10:00:00.000Z",
        .mDate = "2024-01-16",
        .mAccountType = "DEFAULT",
        .mCategory = "TRADING",
        .mType = "SELL",
        .mAssetClass = "",
        .mName = "",
        .mSymbol = "XX9000000248",
        .mShares = "-1.000000000",
        .mPrice = "",
        .mAmount = "12.0000",
        .mFee = "-0.2000",
        .mTax = "",
        .mCurrency = "EUR",
        .mOriginalAmount = "",
        .mOriginalCurrency = "",
        .mFxRate = "",
        .mDescription = "",
        .mTransactionId = "synthetic-sale",
        .mCounterpartyName = "",
        .mCounterpartyIban = "",
        .mPaymentReference = "",
        .mMccCode = "",
    };
    TemporaryIncomeInputs files;
    tr::TradeRepublicParser parser;

    const auto history = files.write("history.csv", {healthyRow, invalidRow});
    const std::array inputs{
        StatementMergeInput{.mSourceIndex = 0, .mParseResult = parser.parse(history, 0)}};

    const auto result = DeterministicStatementMerger{}.merge(inputs);

    ASSERT_EQ(result.mStatement.mPresentation.mTradeInstruments.size(), 1U);

    const auto& instrument = result.mStatement.mPresentation.mTradeInstruments[0];

    EXPECT_EQ(instrument.mIsin, "XX9000000248");
    EXPECT_EQ(instrument.mName, "Synthetic Evidence Share");
    EXPECT_EQ(instrument.mAssetClass, AssetClass::Stock);
    ASSERT_EQ(instrument.mTransactions.size(), 1U);
    ASSERT_EQ(instrument.mCorporateActions.size(), 0U);
    ASSERT_EQ(result.mStatement.mChronologicalOrder.size(), 1U);

    const auto& purchase = instrument.mTransactions[0];

    EXPECT_EQ(purchase.mMetadata.mTaxDate, parseCalendarDate("2024-01-15"));
    EXPECT_EQ(purchase.mMetadata.mSourceTimestamp,
              parseSourceTimestamp("2024-01-15T10:00:00.000Z"));
    EXPECT_FALSE(purchase.mMetadata.mOrderingTimestamp.has_value());
    ASSERT_EQ(purchase.mMetadata.mSources.size(), 1U);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mFilename.value(), "history.csv");
    EXPECT_EQ(purchase.mMetadata.mSources[0].mSourceRow, 2U);
    EXPECT_EQ(purchase.mMetadata.mSources[0].mTransactionId, "synthetic-purchase");
    EXPECT_EQ(purchase.mMetadata.mSources[0].mInputSequence, (StableInputSequence{0, 0}));
    ASSERT_EQ(purchase.mInstrumentEvidence.size(), 1U);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mSource, purchase.mMetadata.mSources[0]);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mName, "Synthetic Evidence Share");
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mAssetClass, AssetClass::Stock);
    EXPECT_EQ(purchase.mInstrumentEvidence[0].mAssetClassLabel, "STOCK");
    EXPECT_EQ(purchase.mTradeSide, TradeSide::Buy);
    EXPECT_EQ(purchase.mUnits, 200000000);  // Source 2.000000000 units at eight decimal places.
    EXPECT_EQ(purchase.mUnitPrice, 100000); // Source EUR 10.0000 at four decimal places.
    EXPECT_EQ(purchase.mAmount, 200000);
    EXPECT_EQ(purchase.mFeePaid, 1000);
    EXPECT_EQ(purchase.mCurrency, Currency::EUR);
    EXPECT_EQ(purchase.mExchangeRate, 100'000'000);
    ASSERT_EQ(purchase.mUnitEvidence.size(), 1U);
    EXPECT_EQ(purchase.mUnitEvidence[0].mSourceText, "2.000000000");
    EXPECT_EQ(purchase.mUnitEvidence[0].mSource, purchase.mMetadata.mSources[0]);

    ASSERT_EQ(result.mDiagnostics.size(), 1U);

    const auto& error = std::get<SourcedParseDiagnostic>(result.mDiagnostics[0]);

    EXPECT_EQ(error.mDiagnostic.mSeverity, DiagnosticSeverity::Error);
    EXPECT_EQ(error.mDiagnostic.mCode, DiagnosticCode::MissingField);
    EXPECT_EQ(error.mDiagnostic.mField, "price");
    EXPECT_EQ(error.mDiagnostic.mSourceFile, "history.csv");
    EXPECT_EQ(error.mDiagnostic.mRowIndex, 3U);
    EXPECT_EQ(error.mDiagnostic.mTransactionId, "synthetic-sale");
}

} // namespace
