#pragma once

#include "taxbroker/event_metadata.hpp"
#include "taxbroker/input_file.hpp"
#include "taxbroker/types.hpp"

#include <cstddef>
#include <filesystem>
#include <string_view>

namespace taxbroker {

class CsvParser {
  public:
    /// aMaxInputBytes is the size limit of one file. The app always uses the default; a smaller
    /// one lets a test use a small file.
    explicit CsvParser(std::size_t aMaxInputBytes = kMaxInputFileBytes) noexcept
        : mMaxInputBytes{aMaxInputBytes} {}

    virtual ~CsvParser() = default;

    /// Parses CSV text held in memory. The display name is the file name only (a folder part is
    /// dropped). Bad input or a bad name gives a structured error, never an exception.
    [[nodiscard("Parsed broker data should not be ignored")]] virtual ParseResult parse(
        std::string_view aCsvText, std::string_view aDisplayFilename, std::size_t aSourceIndex) = 0;

    /// Reads a file and parses it. A path with no file name, an unreadable file, a file over the
    /// size limit and a lack of memory each give a structured error.
    [[nodiscard("Parsed broker data should not be ignored")]] ParseResult
    parseFile(const std::filesystem::path& aPath, std::size_t aSourceIndex = 0);

  protected:
    /// The broker of the files this parser reads.
    [[nodiscard]] virtual Broker broker() const noexcept = 0;

    /// The size limit of one file, in bytes.
    [[nodiscard]] std::size_t maxInputBytes() const noexcept {
        return mMaxInputBytes;
    }

  private:
    std::size_t mMaxInputBytes;
};

} // namespace taxbroker
