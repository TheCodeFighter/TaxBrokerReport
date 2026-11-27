#pragma once

#include "taxbroker/types.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace taxbroker {

enum class StatementEventKind {
    Trade,
    CorporateAction,
    Dividend,
    Interest,
    Benefit,
    PrivateMarket,
    BondRedemption,
};

/// Addresses one event of the merged statement (its presentation lists).
struct StatementEventReference {
    StatementEventKind mKind{};
    std::optional<std::size_t> mInstrumentIndex;
    std::size_t mEventIndex{};

    bool operator==(const StatementEventReference&) const = default;
};

struct SourcedParseDiagnostic {
    std::size_t mSourceIndex{};
    Broker mBroker{Broker::Unknown};
    ParseDiagnostic mDiagnostic;
};

enum class MergeDiagnosticCode {
    DuplicateSourceIndex,
    InconsistentSourceIndex,
    ConflictingDuplicate,
    InstrumentNameConflict,
    InstrumentAssetClassConflict,
    DuplicateTransactionInFile,
    IncomePaymentRowsDisagree,
    DividendReversalWithoutPayment,
    DividendNetNotPositive,
    SubstituteDividendPayment,
    IncomeCorrectionLaterYear, ///< Warning: a row booked in a later year than the pay date.
};

struct InstrumentNameVariant {
    std::string mName;
    std::vector<SourceReference> mSources;
};

struct InstrumentAssetClassVariant {
    AssetClass mAssetClass{AssetClass::Unknown};
    std::vector<SourceReference> mSources;
    /// Unrecognized labels remain distinct even when they all map to Unknown.
    std::optional<std::string> mUnrecognizedLabel;
};

/// Which interest form an interest event can belong to, for the forms a diagnostic affects.
enum class InterestEventScope {
    Coupon,     ///< A bond coupon: Doh-Obr.
    BrokerCash, ///< Interest on cash a broker holds: Doh-Obr.
    BankCash,   ///< Interest on a bank deposit: Doh-DHO.
    Unknown,    ///< The type is not known (an answer decides): both interest forms.
};

struct MergeDiagnostic {
    DiagnosticSeverity mSeverity{DiagnosticSeverity::Error};
    MergeDiagnosticCode mCode{MergeDiagnosticCode::ConflictingDuplicate};
    /// English sentence for logs, the CLI and developer tools; the frontend uses the key.
    std::string mMessage;
    std::optional<MessageKey> mMessageKey;
    std::vector<MessageParam> mMessageParams;
    std::vector<SourceReference> mSources;
    std::optional<Date> mTaxDate;
    std::optional<std::string> mInstrumentName;
    std::optional<Isin> mIsin;
    std::optional<std::size_t> mSourceIndex;
    std::vector<StatementEventKind> mEventKinds;
    /// The scope of each interest event the diagnostic is about, sorted, without repeats. Empty
    /// when no interest event is involved.
    std::vector<InterestEventScope> mInterestScopes;
    std::vector<InstrumentNameVariant> mNameVariants;
    std::vector<InstrumentAssetClassVariant> mAssetClassVariants;
};

using StatementMergeDiagnostic = std::variant<SourcedParseDiagnostic, MergeDiagnostic>;

enum class StatementDiagnosticStage {
    Parsing,
    Merging,
};

[[nodiscard]] inline StatementDiagnosticStage
diagnosticStage(const StatementMergeDiagnostic& aDiagnostic) noexcept {
    return std::holds_alternative<SourcedParseDiagnostic>(aDiagnostic)
               ? StatementDiagnosticStage::Parsing
               : StatementDiagnosticStage::Merging;
}

} // namespace taxbroker
