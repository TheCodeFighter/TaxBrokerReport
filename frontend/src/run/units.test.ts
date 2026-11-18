import { describe, expect, it } from "vitest";

import {
  budgetAfterEur,
  formatUnits,
  fractionToEur4,
  parseUnits,
  parseWholeNumber,
  previewUnits,
  ratioFromShareCounts,
  ratioLooksUnusual,
  reduceRatio,
  removedValueEur,
} from "./units";

describe("parseUnits", () => {
  it("takes a point or a comma as the decimal separator", () => {
    expect(parseUnits("0.52")).toBe(52000000n); // 0.52 * 10^8
    expect(parseUnits("0,52")).toBe(52000000n);
  });

  it("reads 1,234 as 1.234 and never as a grouped number", () => {
    expect(parseUnits("1,234")).toBe(123400000n); // 1.234 * 10^8
  });

  it("takes a whole number and the largest number of decimals", () => {
    expect(parseUnits("2")).toBe(200000000n);
    expect(parseUnits("0.00000001")).toBe(1n);
    expect(parseUnits(" 0.99999998 ")).toBe(99999998n);
  });

  it("refuses grouping, a ninth decimal, an empty text and a sign", () => {
    expect(parseUnits("1.000,5")).toBeUndefined();
    expect(parseUnits("1.123456789")).toBeUndefined();
    expect(parseUnits("")).toBeUndefined();
    expect(parseUnits("-1")).toBeUndefined();
    expect(parseUnits(".5")).toBeUndefined();
    expect(parseUnits("1e3")).toBeUndefined();
  });
});

describe("formatUnits", () => {
  it("writes all 8 decimals", () => {
    expect(formatUnits(520000000n)).toBe("5.20000000");
    expect(formatUnits(1n)).toBe("0.00000001");
    expect(formatUnits(99999998n)).toBe("0.99999998");
  });
});

describe("parseWholeNumber", () => {
  it("takes digits only, at most 15", () => {
    expect(parseWholeNumber("10")).toBe(10);
    expect(parseWholeNumber(" 2 ")).toBe(2);
    expect(parseWholeNumber("999999999999999")).toBe(999999999999999);
    expect(parseWholeNumber("1000000000000000")).toBeUndefined(); // 16 digits
    expect(parseWholeNumber("1.5")).toBeUndefined();
    expect(parseWholeNumber("-1")).toBeUndefined();
    expect(parseWholeNumber("")).toBeUndefined();
  });
});

describe("reduceRatio", () => {
  it("reduces by the greatest common divisor", () => {
    expect(reduceRatio(20, 10)).toEqual({ newShares: 2, oldShares: 1 });
    expect(reduceRatio(1, 10)).toEqual({ newShares: 1, oldShares: 10 });
  });

  it("has no ratio when the position would not change or a number is not above 0", () => {
    expect(reduceRatio(5, 5)).toBeUndefined();
    expect(reduceRatio(0, 1)).toBeUndefined();
    expect(reduceRatio(1, 0)).toBeUndefined();
  });
});

describe("ratioFromShareCounts", () => {
  it("turns 0.52 before and 5.20 after into 10 for 1", () => {
    // 5.20 / 0.52 = 10
    expect(ratioFromShareCounts("0.52", "5.20")).toEqual({ newShares: 10, oldShares: 1 });
    expect(ratioFromShareCounts("0,52", "5,2")).toEqual({ newShares: 10, oldShares: 1 });
  });

  it("keeps a rounded share count as a large reduced ratio", () => {
    // 5.20000001 / 0.52 = 520000001 / 52000000, already reduced
    expect(ratioFromShareCounts("0.52", "5.20000001")).toEqual({ newShares: 520000001, oldShares: 52000000 });
  });

  it("has no ratio for equal counts, zero or text that is not a count", () => {
    expect(ratioFromShareCounts("1", "1.00000000")).toBeUndefined();
    expect(ratioFromShareCounts("0", "5")).toBeUndefined();
    expect(ratioFromShareCounts("1.000,5", "5")).toBeUndefined();
  });
});

describe("previewUnits", () => {
  it("gives the position after the split with 8 decimals", () => {
    expect(previewUnits("0.52", 10, 1)).toBe("5.20000000"); // 0.52 x 10
    expect(previewUnits("0.52", 9, 1)).toBe("4.68000000"); // 0.52 x 9, nine additional per unit
    expect(previewUnits("1.00000000", 2, 1)).toBe("2.00000000");
    expect(previewUnits("10", 1, 10)).toBe("1.00000000"); // reverse split 1 for 10
  });

  it("rounds a half away from zero", () => {
    expect(previewUnits("0.00000003", 1, 2)).toBe("0.00000002"); // 1.5 ticks
  });

  it("has nothing when the result rounds to 0 or the position is not a quantity", () => {
    expect(previewUnits("0.00000001", 1, 3)).toBeUndefined(); // 0.33 ticks
    expect(previewUnits("abc", 2, 1)).toBeUndefined();
  });
});

describe("ratioLooksUnusual", () => {
  it("is true when a reduced number is above 1000", () => {
    expect(ratioLooksUnusual(520000001, 52000000)).toBe(true);
    expect(ratioLooksUnusual(1001, 1)).toBe(true);
    expect(ratioLooksUnusual(1, 1001)).toBe(true);
    expect(ratioLooksUnusual(1000, 1)).toBe(false);
    expect(ratioLooksUnusual(10, 1)).toBe(false);
  });
});

describe("EUR amounts", () => {
  it("rounds a fraction to 4 decimals", () => {
    expect(fractionToEur4({ numerator: "1", denominator: "3" })).toBe("0.3333");
    expect(fractionToEur4({ numerator: "2", denominator: "3" })).toBe("0.6667");
    expect(fractionToEur4({ numerator: "1", denominator: "20000" })).toBe("0.0001"); // 0.00005, half up
    expect(fractionToEur4({ numerator: "0", denominator: "1" })).toBe("0.0000");
    expect(fractionToEur4({ numerator: "-1", denominator: "3" })).toBe("-0.3333");
  });

  it("values the removed ticks at the unit value", () => {
    // 2 ticks = 2e-8 units at EUR 25 = EUR 5e-7
    expect(removedValueEur({ numerator: "25", denominator: "1" }, 2n)).toBe("0.0000");
    // 2 ticks at EUR 5000 = EUR 1e-4
    expect(removedValueEur({ numerator: "5000", denominator: "1" }, 2n)).toBe("0.0001");
    // 1 tick at EUR 10/3 per unit = EUR 3.33e-8
    expect(removedValueEur({ numerator: "10", denominator: "3" }, 1n)).toBe("0.0000");
  });

  it("adds the value of the removed ticks to the budget already used", () => {
    // used EUR 0.005 + 2 ticks at EUR 5000 (EUR 0.0001) = EUR 0.0051
    expect(
      budgetAfterEur({ numerator: "1", denominator: "200" }, { numerator: "5000", denominator: "1" }, 2n),
    ).toBe("0.0051");
    // nothing used and nothing removed
    expect(budgetAfterEur({ numerator: "0", denominator: "1" }, { numerator: "5000", denominator: "1" }, 0n)).toBe(
      "0.0000",
    );
  });
});
