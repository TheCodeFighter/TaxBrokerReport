#pragma once

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <cstddef>
#include <memory>
#include <sstream>
#include <string>

namespace taxbroker {

/// Keeps the lines written to the default logger while it lives; the destructor restores the
/// previous logger, also when a test fails early.
class LogCapture {
  public:
    LogCapture() : mPrevious(spdlog::default_logger()) {
        spdlog::set_default_logger(std::make_shared<spdlog::logger>(
            "log_capture",
            std::make_shared<spdlog::sinks::ostream_sink_mt>(mStream)));
    }

    ~LogCapture() {
        spdlog::set_default_logger(mPrevious);
    }

    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    /// Lets the debug lines through (the captured logger starts at info).
    static void showDebug() {
        spdlog::default_logger()->set_level(spdlog::level::debug);
    }

    [[nodiscard]] std::string text() const {
        return mStream.str();
    }

    /// How many times the text is in the log.
    [[nodiscard]] std::size_t count(const std::string& aText) const {
        const std::string log = mStream.str();
        std::size_t found = 0;

        for (auto at = log.find(aText); at != std::string::npos; at = log.find(aText, at + 1))
        {
            ++found;
        }

        return found;
    }

  private:
    std::ostringstream mStream;
    std::shared_ptr<spdlog::logger> mPrevious;
};

} // namespace taxbroker
