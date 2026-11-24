import { describe, expect, it } from "vitest";

import { isCountryCode } from "./country_codes";

describe("isCountryCode", () => {
  it("knows assigned codes and nothing else", () => {
    expect(isCountryCode("SI")).toBe(true);
    expect(isCountryCode("US")).toBe(true);
    expect(isCountryCode("XX")).toBe(false); // a placeholder, no country
    expect(isCountryCode("XK")).toBe(false);
    expect(isCountryCode("si")).toBe(false); // capitals only
    expect(isCountryCode("")).toBe(false);
    expect(isCountryCode("USA")).toBe(false);
  });
});
