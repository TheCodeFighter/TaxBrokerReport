import { describe, expect, it, vi } from "vitest";

import { createApiClient, SessionEndedError } from "./client";

function answer(status: number): Response {
  return new Response(null, { status });
}

describe("createApiClient", () => {
  it("adds the client header and the same-origin cookie to every call", async () => {
    const fetchFunction = vi.fn().mockResolvedValue(answer(200));
    const client = createApiClient(fetchFunction, () => undefined);

    await client.apiFetch("/api/run", { method: "POST", headers: { "Content-Type": "text/plain" } });

    expect(fetchFunction).toHaveBeenCalledWith("/api/run", {
      method: "POST",
      headers: { "Content-Type": "text/plain", "X-TaxBroker": "1" },
      credentials: "same-origin",
    });
  });

  it("ends the session once on a 401 and sends nothing afterwards", async () => {
    const fetchFunction = vi.fn().mockResolvedValue(answer(401));
    const onSessionEnded = vi.fn();
    const client = createApiClient(fetchFunction, onSessionEnded);

    await expect(client.apiFetch("/api/run")).rejects.toBeInstanceOf(SessionEndedError);
    await expect(client.apiFetch("/api/run")).rejects.toBeInstanceOf(SessionEndedError);

    expect(fetchFunction).toHaveBeenCalledTimes(1);
    expect(onSessionEnded).toHaveBeenCalledTimes(1);
    expect(client.isSessionEnded()).toBe(true);
  });

  it("ends the session when the server cannot be reached", async () => {
    const fetchFunction = vi.fn().mockRejectedValue(new TypeError("network error"));
    const onSessionEnded = vi.fn();
    const client = createApiClient(fetchFunction, onSessionEnded);

    await expect(client.apiFetch("/api/run")).rejects.toBeInstanceOf(SessionEndedError);

    expect(onSessionEnded).toHaveBeenCalledTimes(1);
    expect(client.isSessionEnded()).toBe(true);
  });

  it("keeps the session on another error status such as 409", async () => {
    const fetchFunction = vi.fn().mockResolvedValue(answer(409));
    const onSessionEnded = vi.fn();
    const client = createApiClient(fetchFunction, onSessionEnded);

    const response = await client.apiFetch("/api/run/answers", { method: "POST" });

    expect(response.status).toBe(409);
    expect(onSessionEnded).not.toHaveBeenCalled();
    expect(client.isSessionEnded()).toBe(false);
  });

  it("send does no session handling, so the login can read a 401 itself", async () => {
    const fetchFunction = vi.fn().mockResolvedValue(answer(401));
    const onSessionEnded = vi.fn();
    const client = createApiClient(fetchFunction, onSessionEnded);

    const response = await client.send("/api/session");

    expect(response.status).toBe(401);
    expect(onSessionEnded).not.toHaveBeenCalled();
  });
});
