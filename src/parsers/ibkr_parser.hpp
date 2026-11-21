#pragma once

#include "csv_parser.hpp"

#include <cstddef>
#include <string_view>

namespace taxbroker::ibkr {

/// Reads one Interactive Brokers Activity Flex Query in XML format into the broker-neutral
/// statement (`docs/architecture.md`, "Interactive Brokers export contract"). It derives from
/// CsvParser only for the shared file reading and size limit; the text it parses is XML.
class IbkrParser final : public CsvParser {
  public:
    using CsvParser::CsvParser;

    /// Parses XML text held in memory. Every row that is skipped leaves a diagnostic, and a file
    /// that is not a Flex Query XML gives one file-level error and no events.
    ParseResult parse(std::string_view aXmlText,
                      std::string_view aDisplayFilename,
                      std::size_t aSourceIndex) override;

  protected:
    [[nodiscard]] Broker broker() const noexcept override {
        return Broker::InteractiveBrokers;
    }
};

} // namespace taxbroker::ibkr
