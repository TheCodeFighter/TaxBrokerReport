#pragma once

#include "taxbroker/api/application_result_json.hpp"
#include "taxbroker/application_result.hpp"
#include "taxbroker/types.hpp"
#include "utils/numeric_util.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace taxbroker::cli {

/// The count with the word for one or for more: `1 sale`, `2 sales`.
inline std::string countText(std::size_t aCount, std::string_view aOne, std::string_view aMany) {
    return std::to_string(aCount) + " " + std::string{aCount == 1 ? aOne : aMany};
}

/// Units with four to eight decimals, as in the XML: 1.5 units is `1.5000`.
inline std::string unitsText(Units aUnits) {
    constexpr std::size_t kMinimumDecimals = 4;

    return formatFixedPoint(aUnits, UNITS_SCALE, kMinimumDecimals);
}

/// The ISIN and the name, each only when it is known: `XX0000000001 Synthetic Share`.
inline std::string investmentText(const Isin& aIsin, const std::string& aName) {
    if (aIsin.empty() || aName.empty())
    {
        return aIsin + aName;
    }

    return aIsin + " " + aName;
}

/// The reason codes of an investment of a failure question or of an exclusion, in the order of the
/// result: `incomplete_history, missing_exchange_rate`.
inline std::string reasonCodesText(const RequestInvestment& aInvestment) {
    std::string text;

    for (const auto& code : aInvestment.mReasonCodes)
    {
        text += (text.empty() ? "" : ", ") + std::string{api::diagnosticCodeName(code)};
    }

    return text;
}

} // namespace taxbroker::cli
