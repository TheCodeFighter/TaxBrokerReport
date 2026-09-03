#pragma once

#include "csv_parser.hpp"
#include "types_parser.hpp"

#include <string_view>
#include <utility>

namespace csv {
class CSVRow;
}

namespace taxbroker::tr {

class TradeRepublicParser final : public CsvParser {
  public:
    ParseResult parse(const std::filesystem::path& aCsvPath) override;

  private:
    struct RowContext;

    static constexpr char delimiter = ',';

    RowMeta detectRowType(const csv::CSVRow& aCsvRow) const;
    InterestType detectInterestType(std::string_view aType) const;

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

    // Parsing helpers
    std::optional<Date> parseDate(std::string_view aValue);
    std::optional<Money> parseMoney(std::string_view aValue);
    std::optional<ExchangeRate> parseExchangeRate(std::string_view aValue);
    std::optional<Units> parseUnits(std::string_view aValue);
    Currency parseCurrency(std::string_view aValue);
    AssetClass parseAssetClass(std::string_view aValue);
    std::optional<BenefitType> parseBenefitType(std::string_view aValue);
    std::optional<PrivateMarketEventType> parsePrivateMarketEventType(std::string_view aValue);
    std::optional<TradeSide> parseTradeSide(std::string_view aValue);

    // class helpers
    static bool isInstrumentValid(std::string_view aContext,
                                  const std::string& aIsin,
                                  const std::string& aName,
                                  const RowContext& aRowContext);
    static std::optional<Units> normalizeTradeUnits(TradeSide aTradeSide, Units aSignedUnits);
    static std::optional<Money> normalizeTradeAmount(TradeSide aTradeSide, Money aSignedAmount);
    GetAmount getAmountAndCurrency(const csv::CSVRow& aCsvRow);
    std::pair<std::string_view, std::string> pickAmountField(const csv::CSVRow& aRow);
    static std::optional<Money> parseTaxPaid(std::string_view aValue);
    static std::optional<Money> parseFeePaid(std::string_view aValue);
};

} // namespace taxbroker::tr
