import { describe, expect, it } from "vitest";

import { hasWrongCheckDigit, isSlovenianTaxNumber } from "./tax_number";

describe("isSlovenianTaxNumber", () => {
  it("accepts a number with the right check digit", () => {
    // 1*8 + 2*7 + 3*6 + 4*5 + 5*4 + 6*3 + 7*2 = 112; 112 mod 11 = 2; 11 - 2 = 9.
    expect(isSlovenianTaxNumber("12345679")).toBe(true);
  });

  it("writes a check digit of 10 as 0", () => {
    // 1*8 + 2*2 = 12; 12 mod 11 = 1; 11 - 1 = 10, written as 0.
    expect(isSlovenianTaxNumber("10000020")).toBe(true);
  });

  it("refuses a wrong check digit", () => {
    expect(isSlovenianTaxNumber("12345678")).toBe(false);
  });

  it("refuses a sum that is a multiple of 11, which has no check digit", () => {
    // 1*8 + 1*3 = 11; 11 mod 11 = 0.
    expect(isSlovenianTaxNumber("10000100")).toBe(false);
  });

  it("refuses all zeros, a short text and a letter", () => {
    expect(isSlovenianTaxNumber("00000000")).toBe(false);
    expect(isSlovenianTaxNumber("1234567")).toBe(false);
    expect(isSlovenianTaxNumber("1234567A")).toBe(false);
  });
});

describe("hasWrongCheckDigit", () => {
  it("is true for eight digits with a wrong check digit, with spaces around", () => {
    expect(hasWrongCheckDigit(" 12345678 ")).toBe(true);
  });

  it("is false for a right number", () => {
    expect(hasWrongCheckDigit("12345679")).toBe(false);
  });

  it("is false while the number is not complete or empty, so the hint does not flash while typing", () => {
    expect(hasWrongCheckDigit("")).toBe(false);
    expect(hasWrongCheckDigit("1234567")).toBe(false);
    expect(hasWrongCheckDigit("1234567x")).toBe(false);
  });
});
