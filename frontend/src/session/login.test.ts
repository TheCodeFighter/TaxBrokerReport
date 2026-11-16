import { describe, expect, it, vi } from "vitest";

import { startSession } from "./login";

function answer(status: number): Response {
  return new Response(null, { status });
}

/** A browser with the given address fragment; `replaceState` is a spy. */
function browser(hash: string) {
  return { location: { hash }, history: { replaceState: vi.fn() } };
}

describe("startSession", () => {
  it("exchanges the token of the link once and removes it from the address", async () => {
    const environment = browser("#token=abc");
    const send = vi.fn().mockResolvedValue(answer(204));

    const state = await startSession(environment, send);

    expect(state).toBe("ready");
    expect(send).toHaveBeenCalledTimes(1);
    expect(send).toHaveBeenCalledWith("/api/session", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: '{"token":"abc"}',
    });
    expect(environment.history.replaceState).toHaveBeenCalledWith(null, "", "/");
  });

  it("says the link is invalid on a 401 without a cookie and still removes the token", async () => {
    const environment = browser("#token=abc");
    const send = vi.fn().mockResolvedValue(answer(401));

    const state = await startSession(environment, send);

    expect(state).toBe("link_invalid");
    expect(send).toHaveBeenCalledTimes(2);
    expect(send).toHaveBeenNthCalledWith(2, "/api/session");
    expect(environment.history.replaceState).toHaveBeenCalledWith(null, "", "/");
  });

  it("is ready when a used link gets a 401 but the cookie of the first login works", async () => {
    const environment = browser("#token=abc");
    const send = vi.fn().mockResolvedValueOnce(answer(401)).mockResolvedValueOnce(answer(204));

    const state = await startSession(environment, send);

    expect(state).toBe("ready");
    expect(send).toHaveBeenCalledTimes(2);
    expect(send).toHaveBeenNthCalledWith(2, "/api/session");
    expect(environment.history.replaceState).toHaveBeenCalledWith(null, "", "/");
  });

  it("says the session ended when the server cannot be reached, and removes the token", async () => {
    const environment = browser("#token=abc");
    const send = vi.fn().mockRejectedValue(new TypeError("network error"));

    const state = await startSession(environment, send);

    expect(state).toBe("session_ended");
    expect(environment.history.replaceState).toHaveBeenCalledWith(null, "", "/");
  });

  it("asks whether the cookie works when there is no fragment", async () => {
    const environment = browser("");
    const send = vi.fn().mockResolvedValue(answer(204));

    const state = await startSession(environment, send);

    expect(state).toBe("ready");
    expect(send).toHaveBeenCalledTimes(1);
    expect(send).toHaveBeenCalledWith("/api/session");
    expect(environment.history.replaceState).not.toHaveBeenCalled();
  });

  it("says the session ended on a 401 without a fragment, and sends no POST", async () => {
    const environment = browser("");
    const send = vi.fn().mockResolvedValue(answer(401));

    const state = await startSession(environment, send);

    expect(state).toBe("session_ended");
    expect(send).toHaveBeenCalledTimes(1);
    expect(send).not.toHaveBeenCalledWith("/api/session", expect.objectContaining({ method: "POST" }));
  });

  it("does not post an empty token or another fragment", async () => {
    const send = vi.fn().mockResolvedValue(answer(204));

    await startSession(browser("#token="), send);
    await startSession(browser("#other=1"), send);

    expect(send).toHaveBeenCalledTimes(2);
    expect(send).toHaveBeenNthCalledWith(1, "/api/session");
    expect(send).toHaveBeenNthCalledWith(2, "/api/session");
  });
});
