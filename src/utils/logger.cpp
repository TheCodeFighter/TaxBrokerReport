#include "utils/logger.hpp"

namespace taxbroker {

void InitializeLogger() {
    auto logger = spdlog::basic_logger_mt("taxbroker", "logs/taxbroker.log", true);

    spdlog::set_default_logger(logger);

    spdlog::set_level(spdlog::level::debug);

    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%s:%#] %v");
}

} // namespace taxbroker