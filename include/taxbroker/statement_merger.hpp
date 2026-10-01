#pragma once

#include "taxbroker/types.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace taxbroker {

// The source index is the file's request position, independent of parser completion order.
struct StatementMergeInput {
    std::size_t mSourceIndex{};
    ParseResult mParseResult;
};

enum class StatementEventKind {
    Trade,
    CorporateAction,
    Dividend,
    Interest,
    Benefit,
    PrivateMarket,
};

// Addresses one event owned by MergedStatement::mPresentation.
struct StatementEventReference {
    StatementEventKind mKind{};
    std::optional<std::size_t> mInstrumentIndex;
    std::size_t mEventIndex{};

    bool operator==(const StatementEventReference&) const = default;
};

struct MergedStatement {
    BrokerStatement mPresentation;
    std::vector<StatementEventReference> mChronologicalOrder;
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
};

struct InstrumentNameVariant {
    std::string mName;
    std::vector<SourceReference> mSources;
};

struct InstrumentAssetClassVariant {
    AssetClass mAssetClass{AssetClass::Unknown};
    std::vector<SourceReference> mSources;
};

struct MergeDiagnostic {
    DiagnosticSeverity mSeverity{DiagnosticSeverity::Error};
    MergeDiagnosticCode mCode{MergeDiagnosticCode::ConflictingDuplicate};
    std::string mMessage;
    std::vector<SourceReference> mSources;
    std::optional<Date> mTaxDate;
    std::optional<std::string> mInstrumentName;
    std::optional<Isin> mIsin;
    std::optional<std::size_t> mSourceIndex;
    std::vector<StatementEventKind> mEventKinds;
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

struct StatementMergeResult {
    MergedStatement mStatement;
    std::vector<StatementMergeDiagnostic> mDiagnostics;
};

class StatementMerger {
  public:
    virtual ~StatementMerger() = default;

    [[nodiscard("Merged statement data and diagnostics should not be "
                "ignored")]] virtual StatementMergeResult
    merge(std::span<const StatementMergeInput> aInputs) const = 0;
};

class DeterministicStatementMerger final : public StatementMerger {
  public:
    [[nodiscard("Merged statement data and diagnostics should not be "
                "ignored")]] StatementMergeResult
    merge(std::span<const StatementMergeInput> aInputs) const override;
};

} // namespace taxbroker
