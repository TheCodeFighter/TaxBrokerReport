#include "parsers/csv_parser.hpp"

#include "taxbroker/diagnostic_messages.hpp"
#include "taxbroker/input_file.hpp"
#include "utils/logger.hpp"

#include <string>
#include <utility>
#include <variant>

namespace taxbroker {

ParseResult CsvParser::parseFile(const std::filesystem::path& aPath, std::size_t aSourceIndex) {
    const std::string name = aPath.filename().string();

    // A path with no file name is reported by `parse` without reading anything.
    if (!SourceFilename::tryFromPath(name))
    {
        return parse({}, name, aSourceIndex);
    }

    auto read = readInputFile(aPath, maxInputBytes());

    if (const auto* fault = std::get_if<InputFileFault>(&read))
    {
        ParseResult result{.mBroker = broker()};

        if (*fault == InputFileFault::Unreadable)
        {
            LOG_ERROR("{} CSV source {} could not be read", brokerName(broker()), aSourceIndex);

            auto message = makeDiagnosticMessage(MessageKey::ParseErrorUnreadableFile);

            result.mDiagnostics.push_back(ParseDiagnostic{
                .mSeverity = DiagnosticSeverity::Error,
                .mCode = DiagnosticCode::ParseError,
                .mSourceFile = name,
                .mMessage = std::move(message.mEnglish),
                .mMessageKey = message.mKey,
                .mMessageParams = std::move(message.mParams),
            });

            return result;
        }

        result.mDiagnostics.push_back(inputFileFaultDiagnostic(*fault,
                                                               name,
                                                               brokerName(broker()),
                                                               aSourceIndex,
                                                               maxInputBytes()));

        return result;
    }

    const auto& file = std::get<InputFile>(read);

    return parse(file.mContent, file.mName, aSourceIndex);
}

} // namespace taxbroker
