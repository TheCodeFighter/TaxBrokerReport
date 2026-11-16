import { describe, expect, it } from "vitest";

import { loadVersion } from "./version";

const answer = (status: number, body: string) => () => Promise.resolve(new Response(body, { status }));

describe("loadVersion", () => {
  it("returns the version of the server", async () => {
    expect(await loadVersion(answer(200, '{"version":"2.0.0"}'))).toBe("2.0.0");
  });

  it("returns nothing for a refused call", async () => {
    expect(await loadVersion(answer(404, '{"error":"not_found"}'))).toBeUndefined();
  });

  it("returns nothing when the body has no version text", async () => {
    expect(await loadVersion(answer(200, "{}"))).toBeUndefined();
    expect(await loadVersion(answer(200, '{"version":2}'))).toBeUndefined();
    expect(await loadVersion(answer(200, '{"version":""}'))).toBeUndefined();
    expect(await loadVersion(answer(200, "not json"))).toBeUndefined();
  });
});
