import { describe, expect, it } from "vitest";

import { localizeNumber } from "./number_format";

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
