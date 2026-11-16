#pragma once

#include "http_message.hpp"
#include "profile_store.hpp"

#include <string_view>

namespace taxbroker {

/// The routes `/api/profile` and `/api/profile/delete`: the saved taxpayer details. The page
/// loads them after the login, saves them before a run when the user ticked the box, and deletes
/// them when the user confirms. The guard is done before; the calls come from one thread.
class ProfileRoutes {
  public:
    explicit ProfileRoutes(ProfileStore aStore);

    /// Routes a request whose path is `/api/profile` or `/api/profile/delete`; another path below
    /// `/api/profile/` is 404.
    [[nodiscard]] HttpResponse handle(const HttpRequest& aRequest, std::string_view aPath) const;

  private:
    HttpResponse load() const;
    HttpResponse save(const HttpRequest& aRequest) const;
    HttpResponse remove() const;

    /// The answer for a file that cannot be read: saved and unreadable, no fields.
    HttpResponse unreadableResponse() const;

    /// `{"saved":false}` with the location when there is one.
    HttpResponse notSavedResponse() const;

    ProfileStore mStore;
};

} // namespace taxbroker
