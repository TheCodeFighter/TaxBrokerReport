/** The header that every call carries. The server refuses an API call without it. */
export const CLIENT_HEADER = "X-TaxBroker";

export interface ApiInit {
  method?: string;
  headers?: Record<string, string>;
  /** A text, or a `FormData` for an upload: the browser sets its own `Content-Type` with the boundary. */
  body?: string | FormData;
  /** True lets the call finish while the page unloads (the close message of a tab). */
  keepalive?: boolean;
}

/** The part of `fetch` that the client needs, so the tests pass their own. */
export type FetchFunction = (path: string, init: RequestInit) => Promise<Response>;

/** Why the session ended: the server refused the cookie (401), or it cannot be reached. */
export type SessionEndReason = "unauthorized" | "unreachable";

/** Thrown when the session has ended: the server said 401 or cannot be reached. */
export class SessionEndedError extends Error {
  constructor() {
    super("session_ended");
    this.name = "SessionEndedError";
  }
}

export interface ApiClient {
  /** A call without any session handling. The login uses it, because its 401 means another thing. */
  send(path: string, init?: ApiInit): Promise<Response>;
  /**
   * A call of a signed-in screen. A 401 or a network error ends the session in one place: the
   * callback runs once with the reason, this call throws SessionEndedError, and later calls send
   * nothing.
   */
  apiFetch(path: string, init?: ApiInit): Promise<Response>;
  isSessionEnded(): boolean;
}

export function createApiClient(
  fetchFunction: FetchFunction,
  onSessionEnded: (reason: SessionEndReason) => void,
): ApiClient {
  let ended = false;

  const end = (reason: SessionEndReason): SessionEndedError => {
    if (!ended) {
      ended = true;
      onSessionEnded(reason);
    }

    return new SessionEndedError();
  };

  const send = (path: string, init: ApiInit = {}): Promise<Response> =>
    fetchFunction(path, {
      ...init,
      headers: { ...init.headers, [CLIENT_HEADER]: "1" },
      credentials: "same-origin",
    });

  const apiFetch = async (path: string, init: ApiInit = {}): Promise<Response> => {
    if (ended) {
      throw new SessionEndedError();
    }

    let response: Response;

    try {
      response = await send(path, init);
    } catch {
      throw end("unreachable");
    }

    if (response.status === 401) {
      throw end("unauthorized");
    }

    return response;
  };

  return { send, apiFetch, isSessionEnded: () => ended };
}
