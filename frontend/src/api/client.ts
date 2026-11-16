/** The header that every call carries. The server refuses an API call without it. */
export const CLIENT_HEADER = "X-TaxBroker";

export interface ApiInit {
  method?: string;
  headers?: Record<string, string>;
  body?: string;
}

/** The part of `fetch` that the client needs, so the tests pass their own. */
export type FetchFunction = (path: string, init: RequestInit) => Promise<Response>;

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
   * callback runs once, this call throws SessionEndedError, and later calls send nothing.
   */
  apiFetch(path: string, init?: ApiInit): Promise<Response>;
  isSessionEnded(): boolean;
}

export function createApiClient(fetchFunction: FetchFunction, onSessionEnded: () => void): ApiClient {
  let ended = false;

  const end = (): SessionEndedError => {
    if (!ended) {
      ended = true;
      onSessionEnded();
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
      throw end();
    }

    if (response.status === 401) {
      throw end();
    }

    return response;
  };

  return { send, apiFetch, isSessionEnded: () => ended };
}
