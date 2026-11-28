import { describe, expect, it, vi } from "vitest";

import { createApiClient } from "../api/client";
import { closeThisTab, requestQuit } from "./quit";

describe("requestQuit", () => {
  it("sends one POST to /api/quit and says stopped when the server answers 200", async () => {
    const fetchFunction = vi.fn().mockResolvedValue(new Response('{"stopping":true}', { status: 200 }));
    const client = createApiClient(fetchFunction, () => undefined);

    const outcome = await requestQuit(client.apiFetch);

    expect(outcome).toBe("stopped");
    expect(fetchFunction).toHaveBeenCalledTimes(1);
    expect(fetchFunction).toHaveBeenCalledWith("/api/quit", {
      method: "POST",
      headers: { "X-TaxBroker": "1" },
      credentials: "same-origin",
    });
  });

  it("says failed when the server answers another status", async () => {
    const fetchFunction = vi.fn().mockResolvedValue(new Response(null, { status: 500 }));
    const onSessionEnded = vi.fn();
    const client = createApiClient(fetchFunction, onSessionEnded);

    expect(await requestQuit(client.apiFetch)).toBe("failed");
    expect(onSessionEnded).not.toHaveBeenCalled();
  });

  it("says ended and tells the page the server is unreachable when the call cannot be made", async () => {
    const fetchFunction = vi.fn().mockRejectedValue(new TypeError("network error"));
    const onSessionEnded = vi.fn();
    const client = createApiClient(fetchFunction, onSessionEnded);

    expect(await requestQuit(client.apiFetch)).toBe("ended");
    expect(onSessionEnded).toHaveBeenCalledWith("unreachable");
  });

  it("does not hide an error that is not a lost session", async () => {
    const apiFetch = vi.fn().mockRejectedValue(new RangeError("broken"));

    await expect(requestQuit(apiFetch)).rejects.toBeInstanceOf(RangeError);
  });
});

describe("closeThisTab", () => {
  it("asks the browser to close the tab once", () => {
    const closeWindow = vi.fn();

    closeThisTab(closeWindow);

    expect(closeWindow).toHaveBeenCalledTimes(1);
  });

  it("does not fail when the browser refuses to close the tab", () => {
    const closeWindow = vi.fn(() => {
      throw new Error("refused");
    });

    expect(() => closeThisTab(closeWindow)).not.toThrow();
  });
});
