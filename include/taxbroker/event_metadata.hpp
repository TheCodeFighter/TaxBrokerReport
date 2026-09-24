#pragma once

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ratio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace taxbroker {

enum class Broker {
    Unknown,
    TradeRepublic,
    InteractiveBrokers,
};

// Broker-reported tax date; never derive it from the normalized timestamp.
using DayDuration = std::chrono::duration<std::int64_t, std::ratio<86400>>;
using Date = std::chrono::time_point<std::chrono::system_clock, DayDuration>;

// UTC-normalized source instant; finer source precision is truncated.
using SourceTimestamp = std::chrono::sys_time<std::chrono::milliseconds>;

class SourceFilename {
  public:
    SourceFilename() = default;

    [[nodiscard]] static SourceFilename fromPath(std::string_view aPath) {
        const auto separator = aPath.find_last_of("/\\");
        const auto basename = aPath.substr(separator == std::string_view::npos ? 0 : separator + 1);
        if (basename.empty() || basename == "." || basename == "..")
        {
            throw std::invalid_argument{"Source filename must have a non-empty basename"};
        }
        return SourceFilename{std::string{basename}};
    }

    [[nodiscard]] const std::string& value() const noexcept {
        return mValue;
    }

    bool operator==(const SourceFilename&) const = default;

  private:
    explicit SourceFilename(std::string aValue) : mValue(std::move(aValue)) {}

    std::string mValue;
};

// Scoped to one complete input request; source index follows request order, not completion order.
struct StableInputSequence {
    std::size_t mSourceIndex{};
    std::size_t mEventIndex{};

    auto operator<=>(const StableInputSequence&) const = default;
};

struct SourceReference {
    Broker mBroker{Broker::Unknown};
    SourceFilename mFilename;
    // One-based logical record; CSV headers count as row 1.
    std::size_t mSourceRow{};
    std::optional<std::string> mTransactionId;
    StableInputSequence mInputSequence;

    bool operator==(const SourceReference&) const = default;
};

struct EventMetadata {
    Date mTaxDate{};
    std::optional<SourceTimestamp> mSourceTimestamp;
    SourceReference mSource;

    bool operator==(const EventMetadata&) const = default;
};

// Filename, row, and sequence do not identify a transaction across overlapping exports.
struct TransactionIdentity {
    Broker mBroker{Broker::Unknown};
    std::string mTransactionId;

    bool operator==(const TransactionIdentity&) const = default;
};

[[nodiscard]] inline std::optional<TransactionIdentity>
transactionIdentity(const SourceReference& aSource) {
    if (!aSource.mTransactionId || aSource.mTransactionId->empty())
    {
        return std::nullopt;
    }
    return TransactionIdentity{
        .mBroker = aSource.mBroker,
        .mTransactionId = *aSource.mTransactionId,
    };
}

[[nodiscard]] inline std::optional<TransactionIdentity>
transactionIdentity(const EventMetadata& aMetadata) {
    return transactionIdentity(aMetadata.mSource);
}

// Matching identity makes events duplicate candidates. Their kind, timing, instrument, and payload
// must also match; source location and sequence are excluded from semantic equality.
[[nodiscard]] inline bool sameTransactionIdentity(const EventMetadata& aLeft,
                                                  const EventMetadata& aRight) {
    const auto leftIdentity = transactionIdentity(aLeft);
    return leftIdentity && leftIdentity == transactionIdentity(aRight);
}

// On the same tax date, known timestamps precede unknown times; input sequence breaks ties.
struct EventMetadataChronologicalLess {
    [[nodiscard]] bool operator()(const EventMetadata& aLeft, const EventMetadata& aRight) const {
        if (aLeft.mTaxDate != aRight.mTaxDate)
        {
            return aLeft.mTaxDate < aRight.mTaxDate;
        }

        const bool leftMissingTimestamp = !aLeft.mSourceTimestamp.has_value();
        const bool rightMissingTimestamp = !aRight.mSourceTimestamp.has_value();
        if (leftMissingTimestamp != rightMissingTimestamp)
        {
            return !leftMissingTimestamp;
        }
        if (aLeft.mSourceTimestamp != aRight.mSourceTimestamp)
        {
            return aLeft.mSourceTimestamp < aRight.mSourceTimestamp;
        }

        return aLeft.mSource.mInputSequence < aRight.mSource.mInputSequence;
    }
};

} // namespace taxbroker
