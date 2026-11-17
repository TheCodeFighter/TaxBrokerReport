#pragma once

#include "http_message.hpp"

#include "taxbroker/application_run.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace taxbroker {

/// Writes the error line `Run request failed: <route> (<kind of the exception>)`. Call it only
/// inside a `catch` block; aRoute is one of the fixed route words, never request text.
void logRouteFailure(std::string_view aRoute);

/// The routes below `/api/run`: upload and start, the result, the support report, answers, deleting
/// answers and the XML downloads. They hold the one run of the server and only convert HTTP and
/// JSON to the application service and back; every check of an answer and every calculation is
/// the service's.
/// The guard (Host, Origin, client header, session) is done before; the calls come from one thread.
class RunRoutes {
  public:
    /// aMaxFileBytes is the longest file of an upload; a longer one is 413.
    explicit RunRoutes(std::size_t aMaxFileBytes);

    /// The longest body of an upload: the request part and the most files that fit.
    [[nodiscard]] std::uint64_t uploadBodyLimit() const;

    /// Routes a request whose path is `/api/run` or below `/api/run/`. aQuery is the text after
    /// the `?` of the target, or empty.
    [[nodiscard]] HttpResponse
    handle(const HttpRequest& aRequest, std::string_view aPath, std::string_view aQuery);

  private:
    /// Routes the request. It sets aRoute to a fixed word (`upload`, `answers`, ...) as soon as the
    /// route is known, so `handle` can name it when the work throws.
    HttpResponse dispatch(const HttpRequest& aRequest,
                          std::string_view aPath,
                          std::string_view aQuery,
                          std::string_view& aRoute);
    HttpResponse upload(const HttpRequest& aRequest);
    HttpResponse result() const;
    HttpResponse supportReport() const;
    HttpResponse answers(const HttpRequest& aRequest);
    HttpResponse deleteAnswers(const HttpRequest& aRequest);
    HttpResponse download(std::string_view aReport, std::string_view aQuery) const;

    std::size_t mMaxFileBytes;
    std::optional<ApplicationRun> mRun; ///< None until the first upload.
};

} // namespace taxbroker
