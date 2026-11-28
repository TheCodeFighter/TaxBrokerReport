import { describe, expect, it, vi } from "vitest";

import { createTabReporter, makeTabId } from "./tabs";

describe("makeTabId", () => {
  it("is 32 lower-case hex characters made of 16 random bytes", () => {
    const bytes = Uint8Array.from([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 255]);

    expect(makeTabId(() => bytes)).toBe("000102030405060708090a0b0c0d0eff");
  });
});

describe("createTabReporter", () => {
  it("sends the open message as a keepalive POST with the id", () => {
    const send = vi.fn().mockResolvedValue(new Response(null, { status: 204 }));

    createTabReporter(send, "abc123").open();

    expect(send).toHaveBeenCalledTimes(1);
    expect(send).toHaveBeenCalledWith("/api/tab", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: '{"id":"abc123","open":true}',
      keepalive: true,
    });
  });

  it("sends the close message with open false", () => {
    const send = vi.fn().mockResolvedValue(new Response(null, { status: 204 }));

    createTabReporter(send, "abc123").close();

    expect(send).toHaveBeenCalledWith("/api/tab", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: '{"id":"abc123","open":false}',
      keepalive: true,
    });
  });

  it("ignores a call that fails, so a lost server changes nothing", async () => {
    const send = vi.fn().mockRejectedValue(new TypeError("network error"));

    expect(() => createTabReporter(send, "abc123").close()).not.toThrow();

    // The rejected promise is handled inside: waiting a tick must not raise an unhandled rejection.
    await Promise.resolve();
  });
});
