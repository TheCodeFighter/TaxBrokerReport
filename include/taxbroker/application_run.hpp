#pragma once

#include "taxbroker/application_request.hpp"
#include "taxbroker/application_result.hpp"
#include "taxbroker/decision_session.hpp"
#include "taxbroker/diagnostics.hpp"
#include "taxbroker/reference_catalogue.hpp"

#include <memory>
#include <optional>
#include <variant>

namespace taxbroker {

/// The first error of a supplied ECB rate file. A broken file fails the run before any
/// calculation, because a gap would otherwise show up as a missing rate.
struct RateFileFault {
    ParseDiagnostic mDiagnostic;
};

/// A supplied file that makes the run impossible: a rate file or a reference file that is not
/// valid. No result exists for it.
using ApplicationInputFault = std::variant<RateFileFault, ReferenceFileFault>;

/**
 * One run of the application service: loads the supplied files, parses, merges, prepares the year
 * and calculates every report in a decision session, in a fixed order on one thread. It is the
 * public entry point; the stages behind it are not part of the API.
 */
class ApplicationRun final {
  public:
    /// Starts a run. A supplied file that is not valid gives a fault and no run.
    [[nodiscard]] static std::variant<ApplicationInputFault, ApplicationRun>
    start(ApplicationRequest aRequest);

    /// A new run with new input. The recorded answers stay and are marked as from an earlier run,
    /// and an answer given for the revision before it is stale. A supplied file that is not valid
    /// gives the fault and the run stays as it was; an empty optional means success.
    [[nodiscard]] std::optional<ApplicationInputFault> restart(ApplicationRequest aRequest);

    ApplicationRun(const ApplicationRun&) = delete;
    ApplicationRun& operator=(const ApplicationRun&) = delete;
    ApplicationRun(ApplicationRun&&) noexcept;
    ApplicationRun& operator=(ApplicationRun&&) noexcept;
    ~ApplicationRun();

    /// The request of the current run; a successful restart ends the life of an earlier reference.
    [[nodiscard]] const ApplicationRequest& request() const;

    /// The result of the last calculation that succeeded.
    [[nodiscard]] const ApplicationResult& result() const;

    /// Answers and deletions recalculate through it.
    [[nodiscard]] DecisionSession& session();

  private:
    /// The data of the current run and its decision session; defined in the source file.
    struct Impl;

    explicit ApplicationRun(std::unique_ptr<Impl> aImpl);

    std::unique_ptr<Impl> mImpl;
};

} // namespace taxbroker
