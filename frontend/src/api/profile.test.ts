import { describe, expect, it, vi } from "vitest";

import { deleteProfile, loadProfile, saveProfile } from "./profile";

// A 204 has no body, so an empty text is sent as no body.
const answer = (status: number, body = "") =>
  vi.fn().mockResolvedValue(new Response(body === "" ? null : body, { status }));

describe("loadProfile", () => {
  it("returns the answer of the server", async () => {
    const body = '{"saved":true,"location":"~/x","taxpayer":{"taxNumber":"12345679"}}';

    expect(await loadProfile(answer(200, body))).toEqual({
      saved: true,
      location: "~/x",
      taxpayer: { taxNumber: "12345679" },
    });
  });

  it("returns nothing for a refused call or an answer without `saved`", async () => {
    expect(await loadProfile(answer(404, '{"error":"not_found"}'))).toBeUndefined();
    expect(await loadProfile(answer(200, "{}"))).toBeUndefined();
    expect(await loadProfile(answer(200, "not json"))).toBeUndefined();
  });
});

describe("saveProfile", () => {
  it("sends the taxpayer as JSON and reports a 204 as saved", async () => {
    const fetchFunction = answer(204);

    expect(await saveProfile(fetchFunction, { taxNumber: "12345679", isResident: true })).toEqual({ kind: "saved" });

    expect(fetchFunction).toHaveBeenCalledWith("/api/profile", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: '{"taxNumber":"12345679","isResident":true}',
    });
  });

  it("gives the field of a refused taxpayer", async () => {
    expect(await saveProfile(answer(400, '{"error":"bad_taxpayer","field":"taxNumber"}'), {})).toEqual({
      kind: "refused",
      field: "taxNumber",
    });
    expect(await saveProfile(answer(400, '{"error":"bad_taxpayer"}'), {})).toEqual({ kind: "refused" });
    expect(await saveProfile(answer(400, "not json"), {})).toEqual({ kind: "refused" });
  });

  it("reports any other answer as failed", async () => {
    expect(await saveProfile(answer(500, '{"error":"profile_not_saved"}'), {})).toEqual({ kind: "failed" });
  });
});

describe("deleteProfile", () => {
  it("is true for a 200 and false for a 500", async () => {
    const fetchFunction = answer(200, '{"saved":false}');

    expect(await deleteProfile(fetchFunction)).toBe(true);
    expect(fetchFunction).toHaveBeenCalledWith("/api/profile/delete", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: "",
    });
    expect(await deleteProfile(answer(500, '{"error":"profile_not_deleted"}'))).toBe(false);
  });
});
