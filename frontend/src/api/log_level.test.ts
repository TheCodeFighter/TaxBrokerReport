import { describe, expect, it, vi } from "vitest";

import { changeLogLevel, loadLogLevel, LOG_LEVELS } from "./log_level";

const answer = (status: number, body: string) => vi.fn().mockResolvedValue(new Response(body, { status }));

describe("loadLogLevel", () => {
  it("returns the level of the server", async () => {
    expect(await loadLogLevel(answer(200, '{"level":"error"}'))).toBe("error");
    expect(await loadLogLevel(answer(200, '{"level":"debug"}'))).toBe("debug");
  });

  it("returns nothing for a refused call or an answer that is not a level", async () => {
    expect(await loadLogLevel(answer(404, '{"error":"not_found"}'))).toBeUndefined();
    expect(await loadLogLevel(answer(200, '{"level":"loud"}'))).toBeUndefined();
    expect(await loadLogLevel(answer(200, "{}"))).toBeUndefined();
    expect(await loadLogLevel(answer(200, "not json"))).toBeUndefined();
  });
});

describe("changeLogLevel", () => {
  it("sends the level as JSON and returns the level of the answer", async () => {
    const fetchFunction = answer(200, '{"level":"info"}');

    expect(await changeLogLevel(fetchFunction, "info")).toBe("info");
    expect(fetchFunction).toHaveBeenCalledWith("/api/log-level", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: '{"level":"info"}',
    });
  });

  it("returns nothing when the server refuses", async () => {
    expect(await changeLogLevel(answer(400, '{"error":"bad_log_level"}'), "warn")).toBeUndefined();
    expect(await changeLogLevel(answer(500, ""), "warn")).toBeUndefined();
  });
});

describe("LOG_LEVELS", () => {
  it("lists the four levels, the default first", () => {
    expect(LOG_LEVELS).toEqual(["error", "warn", "info", "debug"]);
  });
});
