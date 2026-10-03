#include "utils/numeric_util.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <variant>

namespace {
using namespace taxbroker;

template <typename T> T requireValue(const NumericResult<T>& aResult) {
    if (const auto* value = std::get_if<T>(&aResult))
    {
        return *value;
    }

    throw std::runtime_error{"Expected a successful synthetic arithmetic result"};
}

template <typename T> void expectError(const NumericResult<T>& aResult, NumericError aError) {
    ASSERT_TRUE(std::holds_alternative<NumericError>(aResult));
    EXPECT_EQ(std::get<NumericError>(aResult), aError);
}

TEST(ImportFixedPointTest, PreservesOriginalDigitsAndCanonicalExactValues) {
    const auto down = requireValue(importFixedPoint(" 0.12345678400 ", UNITS_SCALE));
    const auto up = requireValue(importFixedPoint("-0.123456785", UNITS_SCALE));
    const auto evidence = unitSourceEvidence(up, "-0.123456785", {});

    EXPECT_EQ(down.mValue, 123'45678);
    EXPECT_EQ(down.mDiscardedDigits, "400");
    EXPECT_EQ(down.mCanonicalValue, "0.123456784");
    EXPECT_FALSE(down.mRoundedAwayFromZero);
    EXPECT_EQ(up.mValue, -123'45679);
    EXPECT_TRUE(up.mRoundedAwayFromZero);
    EXPECT_EQ(evidence.mSourceText, "-0.123456785");
    EXPECT_EQ(evidence.mDiscardedDigits, "5");
    EXPECT_EQ(requireValue(importFixedPoint("(001,234.5000)", MONEY_SCALE)).mCanonicalValue,
              "-1234.5");
    EXPECT_EQ(requireValue(importFixedPoint(".5", MONEY_SCALE)).mValue, 5'000);
    EXPECT_EQ(requireValue(importFixedPoint("-0.00000", MONEY_SCALE)).mCanonicalValue, "0");
    expectError(importFixedPoint("0.00004", MONEY_SCALE), NumericError::UnrepresentableValue);
    EXPECT_EQ(
        requireValue(importFixedPoint("0.00004", MONEY_SCALE, ValuePolicy::Signed, false)).mValue,
        0);
    expectError(importFixedPoint("-0.00004", MONEY_SCALE, ValuePolicy::Nonnegative, false),
                NumericError::UnrepresentableValue);
    expectError(importFixedPoint("0", MONEY_SCALE, ValuePolicy::Positive),
                NumericError::UnrepresentableValue);
    expectError(importFixedPoint("922337203685477.5808", MONEY_SCALE), NumericError::Overflow);

    for (const auto text : {"", "abc", "1.2.3", "12,34.5", "()", "+", "1.2x"})
    {
        expectError(importFixedPoint(text, MONEY_SCALE), NumericError::InvalidInput);
    }

    expectError(importFixedPoint("1", 0), NumericError::InvalidInput);
    expectError(importFixedPoint("1", 12), NumericError::InvalidInput);
}

TEST(ImportFixedPointTest, KeepsOfficialRateInputStrictAndBrokerRoundingExplicit) {
    EXPECT_EQ(requireValue(importExchangeRate("1.25000000")), 125'000'000);
    expectError(importExchangeRate("1.250000000"), NumericError::InvalidExchangeRate);
    EXPECT_EQ(requireValue(importExchangeRate("1.234567895", true)), 123'456'790);

    for (const auto text : {"0", "-1", "invalid", "0.000000004", "922337203685.4775808"})
    {
        expectError(importExchangeRate(text), NumericError::InvalidExchangeRate);
    }
}

} // namespace
