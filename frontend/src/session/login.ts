import type { ApiClient } from "../api/client";

/** Where the page stands after the start. */
export type SessionState = "ready" | "session_ended" | "link_invalid";

/** The parts of the browser that the start reads and changes, so the tests pass their own. */
export interface BrowserEnvironment {
  location: { hash: string };
  history: { replaceState(data: unknown, unused: string, url?: string | null): void };
}

/** The one-time token of the link (`/#token=...`), or nothing. */
function linkToken(hash: string): string | undefined {
  const token = new URLSearchParams(hash.replace(/^#/, "")).get("token");

  return token === null || token === "" ? undefined : token;
}

/**
 * Starts the session. A link with a token is exchanged once for the session cookie (the server
 * sets it) and the token is removed from the address bar, also when the exchange fails. A page
 * without a token (a reload) asks whether the cookie still works. A refused token (401) asks
 * the same once, because a used link in a second tab may still have a working cookie.
 */
export async function startSession(
  environment: BrowserEnvironment,
  send: ApiClient["send"],
): Promise<SessionState> {
  const token = linkToken(environment.location.hash);

  if (token === undefined) {
    try {
      const response = await send("/api/session");

      return response.status === 204 ? "ready" : "session_ended";
    } catch {
      return "session_ended";
    }
  }

  try {
    const response = await send("/api/session", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ token }),
    });

    if (response.status === 204) {
      return "ready";
    }

    // A used link in a second tab is also a 401, but the first login may have set the cookie.
    const cookieCheck = await send("/api/session");

    return cookieCheck.status === 204 ? "ready" : "link_invalid";
  } catch {
    return "session_ended";
  } finally {
    environment.history.replaceState(null, "", "/");
  }
}
