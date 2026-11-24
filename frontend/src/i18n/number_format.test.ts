import { describe, expect, it } from "vitest";

import { localizeNumber, roundToCents } from "./number_format";

describe("localizeNumber", () => {
  it("writes the decimal comma in Slovenian and keeps the point in English", () => {
    expect(localizeNumber("1234.50", "sl")).toBe("1234,50");
    expect(localizeNumber("1234.50", "en")).toBe("1234.50");
  });

  it("changes the point of a many-digit and a negative number", () => {
    expect(localizeNumber("0.99999999", "sl")).toBe("0,99999999");
    expect(localizeNumber("-12.5000", "sl")).toBe("-12,5000");
  });

  it("leaves a whole number as it is", () => {
    expect(localizeNumber("100", "sl")).toBe("100");
    expect(localizeNumber("2023", "sl")).toBe("2023");
  });

  it("leaves text that is not a plain decimal number as it is", () => {
    expect(localizeNumber("XX0000000001", "sl")).toBe("XX0000000001");
    expect(localizeNumber("2023-05-03", "sl")).toBe("2023-05-03");
    expect(localizeNumber("1.2.3", "sl")).toBe("1.2.3");
    expect(localizeNumber("12.5 EUR", "sl")).toBe("12.5 EUR");
    expect(localizeNumber("", "sl")).toBe("");
  });
});

describe("roundToCents", () => {
  it("cuts four decimals to two", () => {
    expect(roundToCents("4.9000")).toBe("4.90");
    expect(roundToCents("3.0000")).toBe("3.00");
    expect(roundToCents("12.3449")).toBe("12.34");
  });

  it("rounds a half away from zero", () => {
    expect(roundToCents("1.0050")).toBe("1.01");
    expect(roundToCents("-1.0050")).toBe("-1.01");
    expect(roundToCents("0.0050")).toBe("0.01");
    expect(roundToCents("9.9950")).toBe("10.00");
  });

  it("does not round a third decimal below five up", () => {
    expect(roundToCents("1.0049")).toBe("1.00");
    expect(roundToCents("-1.0049")).toBe("-1.00");
  });

  it("writes no minus sign for an amount that rounds to zero", () => {
    expect(roundToCents("-0.0049")).toBe("0.00");
  });

  it("adds missing decimals", () => {
    expect(roundToCents("2")).toBe("2.00");
    expect(roundToCents("2.5")).toBe("2.50");
    expect(roundToCents("0")).toBe("0.00");
  });

  it("leaves text that is not a plain number as it is", () => {
    expect(roundToCents("")).toBe("");
    expect(roundToCents("1.2.3")).toBe("1.2.3");
    expect(roundToCents("12.5 EUR")).toBe("12.5 EUR");
  });
});
