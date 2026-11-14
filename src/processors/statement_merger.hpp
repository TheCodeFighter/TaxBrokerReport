#pragma once

#include "taxbroker/statement_merger.hpp"
#include "taxbroker/types.hpp"

#include <cstddef>
#include <vector>

namespace taxbroker {

// The source index is the file's request position, independent of parser completion order.
struct StatementMergeInput {
    std::size_t mSourceIndex{};
    ParseResult mParseResult;
};

struct MergedStatement {
    BrokerStatement mPresentation;
    // Cross-collection chronology; same-day split priority belongs to tax processing.
    std::vector<StatementEventReference> mChronologicalOrder;
};

/// Builds the chronological order of the statement again from its events, for a stage that moves
/// or removes events.
void rebuildChronologicalOrder(MergedStatement& aStatement);

struct StatementMergeResult {
    MergedStatement mStatement;
    std::vector<StatementMergeDiagnostic> mDiagnostics;
};

class StatementMerger {
  public:
    virtual ~StatementMerger() = default;

    [[nodiscard("Merged statement data and diagnostics should not be "
                "ignored")]] virtual StatementMergeResult
    merge(std::vector<StatementMergeInput>&& aInputs) const = 0;
};

class DeterministicStatementMerger final : public StatementMerger {
  public:
    [[nodiscard("Merged statement data and diagnostics should not be "
                "ignored")]] StatementMergeResult
    merge(std::vector<StatementMergeInput>&& aInputs) const override;
};

} // namespace taxbroker
