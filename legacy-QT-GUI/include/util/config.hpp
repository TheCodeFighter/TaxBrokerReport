#pragma once

#include <string_view>

struct TradeRepublicConfig {
    std::string_view mName;
    std::string_view mAddress;
    std::string_view mTaxNumber;
    std::string_view mCountry;
    std::string_view mCountryCode;
    std::string_view mInterestsType;
};

inline constexpr TradeRepublicConfig TRADE_REPUBLIC_DATA{
    .mName          = "Trade Republic Bank GmbH",
    .mAddress       = "Brunnenstr. 19-21, 10119 Berlin",
    .mTaxNumber     = "DE307510626",
    .mCountry       = "Nemčija",
    .mCountryCode   = "DE",
    .mInterestsType = "1"
};
