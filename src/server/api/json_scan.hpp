#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string_view>

namespace taxbroker::api::detail {

/// The deepest nesting a request body has; the real ones have three levels.
inline constexpr int kMaxJsonDepth = 8;

/// What a scan of a request JSON found.
enum class JsonScan {
    Clean,
    HasPathKey, ///< An object has a key named `path` (R10: the server never reads a path).
    TooDeep,    ///< Nested deeper than kMaxJsonDepth.
};

/// True when aKey is `path` in any ASCII case (`Path`, `PATH`, ...). This library does not use
/// the server's text helpers, so the compare is local; it copies nothing.
[[nodiscard]] inline bool isPathKey(std::string_view aKey) {
    constexpr std::string_view kPath = "path";

    return std::equal(aKey.begin(),
                      aKey.end(),
                      kPath.begin(),
                      kPath.end(),
                      [](char aCharacter, char aLetter) {
                          return aCharacter == aLetter ||
                                 aCharacter == static_cast<char>(aLetter - 'a' + 'A');
                      });
}

/// Looks at every object key of the JSON, depth first. aDepth is 1 for the top value.
[[nodiscard]] inline JsonScan scanRequestJson(const nlohmann::json& aJson, int aDepth) {
    if (aDepth > kMaxJsonDepth)
    {
        return JsonScan::TooDeep;
    }

    if (aJson.is_object())
    {
        for (const auto& [key, value] : aJson.items())
        {
            if (isPathKey(key))
            {
                return JsonScan::HasPathKey;
            }

            const JsonScan inner = scanRequestJson(value, aDepth + 1);

            if (inner != JsonScan::Clean)
            {
                return inner;
            }
        }
    }
    else if (aJson.is_array())
    {
        for (const nlohmann::json& value : aJson)
        {
            const JsonScan inner = scanRequestJson(value, aDepth + 1);

            if (inner != JsonScan::Clean)
            {
                return inner;
            }
        }
    }

    return JsonScan::Clean;
}

} // namespace taxbroker::api::detail
