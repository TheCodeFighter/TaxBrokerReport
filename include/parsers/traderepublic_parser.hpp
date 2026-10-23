#pragma once

#include "csv_parser.hpp"
#include "types_parser.hpp"

#include <string_view>
#include <utility>

namespace csv {
class CSVRow;
}

namespace taxbroker::tr {

/// UTC ordering hour when the original datetime calendar date disagrees with date.
inline constexpr int kTrDateMismatchDefaultHour = 9;
static_assert(kTrDateMismatchDefaultHour >= 0 && kTrDateMismatchDefaultHour < 24);

class TradeRepublicParser final : public CsvParser {
  public:
    ParseResult parse(const std::filesystem::path& aCsvPath, std::size_t aSourceIndex = 0) override;

  private:
    struct RowContext;
    struct ParsedIncome;

    static constexpr char delimiter = ',';

    static RowMeta detectRowType(const csv::CSVRow& aCsvRow);
    static InterestType detectInterestType(const csv::CSVRow& aCsvRow);

    bool parseTradeRow(const csv::CSVRow& aCsvRow,
                       std::vector<TradeInstrument>& aInstruments,
                       const RowParsedValues& aParsedValues,
                       const RowContext& aContext);
    void parseDividendRow(const csv::CSVRow& aCsvRow,
                          std::vector<DividendInstrument>& aInstruments,
                          const RowContext& aContext);
    void parseInterestRow(const csv::CSVRow& aCsvRow,
                          std::vector<InterestInstrument>& aInstruments,
                          InterestType aInterestType,
                          const RowContext& aContext);
    void parseCorporateActionRow(const csv::CSVRow& aCsvRow,
                                 std::vector<TradeInstrument>& aInstruments,
                                 const RowContext& aContext);
    void parseBenefitRow(const csv::CSVRow& aCsvRow,
                         std::vector<BenefitEvent>& aBenefitEvents,
                         const RowParsedValues& aParsedValues,
                         const RowContext& aContext);
    void parsePrivateMarketRow(const csv::CSVRow& aCsvRow,
                               std::vector<PrivateMarketEvent>& aPrivateMarketEvents,
                               const RowParsedValues& aParsedValues,
                               const RowContext& aContext);

    /// Shared income validation; blank withholding and absent broker FX remain unknown. Reports
    /// every invalid field, and the instrument too when aCheckInstrument is set.
    std::optional<ParsedIncome> parseIncome(const csv::CSVRow& aCsvRow,
                                            std::string_view aRowKind,
                                            bool aCheckInstrument,
                                            const RowContext& aContext);

    // Parsing helpers
    std::optional<Money> parseMoney(std::string_view aValue);
    std::optional<UnitPrice> parseUnitPrice(std::string_view aValue);
    std::optional<ExchangeRate> parseExchangeRate(std::string_view aValue);
    static Currency parseCurrency(std::string_view aValue);
    static AssetClass parseAssetClass(std::string_view aValue);
    static std::optional<BenefitType> parseBenefitType(std::string_view aValue);
    static std::optional<PrivateMarketEventType>
    parsePrivateMarketEventType(std::string_view aValue);
    static std::optional<TradeSide> parseTradeSide(std::string_view aValue);

    // class helpers
    static void isInstrumentValid(std::string_view aContext,
                                  const std::string& aIsin,
                                  const std::string& aName,
                                  const RowContext& aRowContext);
    static std::optional<Units> normalizeTradeUnits(TradeSide aTradeSide, Units aSignedUnits);
    static std::optional<Money> normalizeTradeAmount(TradeSide aTradeSide, Money aSignedAmount);
    GetAmount getAmountAndCurrency(const csv::CSVRow& aCsvRow);
    /// The broker rate of a trade or income row: the supplied `fx_rate`, or the identity rate for a
    /// plain EUR row, or none. An invalid supplied rate also gives none; the caller reports it.
    std::optional<ExchangeRate>
    parseBrokerRate(std::string_view aRateText, bool aUsesOriginalCurrency, Currency aRowCurrency);
    static std::pair<std::string_view, std::string> pickAmountField(const csv::CSVRow& aRow);
    static std::optional<Money> parseTaxPaid(std::string_view aValue);
    static std::optional<Money> parseFeePaid(std::string_view aValue);
};

} // namespace taxbroker::tr
