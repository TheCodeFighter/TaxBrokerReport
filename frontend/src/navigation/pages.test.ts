import { describe, expect, it } from "vitest";

import { hashOf, pageFromHash } from "./pages";

describe("pageFromHash", () => {
  it("is the home page without a hash and for #/", () => {
    expect(pageFromHash("")).toBe("home");
    expect(pageFromHash("#/")).toBe("home");
  });

  it("reads the four page hashes", () => {
    expect(pageFromHash("#/report")).toBe("report");
    expect(pageFromHash("#/manual")).toBe("manual");
    expect(pageFromHash("#/about")).toBe("about");
  });

  it("is the home page for an unknown or odd hash", () => {
    expect(pageFromHash("#/unknown")).toBe("home");
    expect(pageFromHash("#/REPORT")).toBe("home");
    expect(pageFromHash("#report")).toBe("home");
    expect(pageFromHash("#/report/")).toBe("home");
  });

  it("is the home page for a login link", () => {
    expect(pageFromHash("#token=abc")).toBe("home");
  });
});

describe("hashOf", () => {
  it("gives the address hash of each page", () => {
    expect(hashOf("home")).toBe("#/");
    expect(hashOf("report")).toBe("#/report");
    expect(hashOf("manual")).toBe("#/manual");
    expect(hashOf("about")).toBe("#/about");
  });
});
