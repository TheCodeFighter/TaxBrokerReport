import { describe, expect, it } from "vitest";

import { defaultYear, needsRateHint, reportingYears } from "./years";

describe("years", () => {
  it("lists the years from the current year down to 2000", () => {
    const years = reportingYears(new Date(2026, 10, 16));

    expect(years).toHaveLength(27);
    expect(years[0]).toBe(2026);
    expect(years[1]).toBe(2025);
    expect(years[26]).toBe(2000);
  });

  it("defaults to the year before the current year", () => {
    expect(defaultYear(new Date(2026, 10, 16))).toBe(2025);
    expect(defaultYear(new Date(2027, 0, 5))).toBe(2026);
  });

  it("hints at a rate file for a year without built-in rates", () => {
    expect(needsRateHint(2015)).toBe(true);
    expect(needsRateHint(2016)).toBe(false);
    expect(needsRateHint(2025)).toBe(false);
    expect(needsRateHint(2026)).toBe(true);
  });
});
