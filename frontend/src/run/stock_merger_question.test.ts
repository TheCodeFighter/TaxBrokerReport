import { describe, expect, it } from "vitest";

import type { StockMergerCheck } from "./questions";
import {
  initialStockMergerForm,
  parseMarketValue,
  stockMergerAnswer,
  stockMergerProblems,
} from "./stock_merger_question";

// The rows are booked on 2024-06-03 (the old ISIN) and 2024-06-05 (the new one).
const check: StockMergerCheck = {
  oldIsin: "XX0000000136",
  oldName: "Synthetic Merged Share",
  newIsin: "XX0000000137",
  newName: "Synthetic Acquirer",
  oldUnits: "10.00000000",
  newUnits: "4.00000000",
  currency: "USD",
  cash: "0.0000",
  suggestedValue: "520.0000",
  effectiveDate: "2024-06-03",
  earliestDate: "2024-06-03",
  latestDate: "2024-06-05",
  sources: [
    { broker: "interactive_brokers", date: "2024-06-03", transactionId: "t-5", sourceValue: "-10" },
    { broker: "interactive_brokers", date: "2024-06-05", transactionId: "t-6", sourceValue: "4" },
  ],
};

describe("initialStockMergerForm", () => {
  it("starts with no choice, the value of the rows and the date of the first row", () => {
    expect(initialStockMergerForm(check)).toEqual({ treatment: undefined, value: "520.0000", date: "2024-06-03" });
  });

  it("starts with an empty value when the rows give none", () => {
    const { suggestedValue: _none, ...withoutValue } = check;

    expect(initialStockMergerForm(withoutValue).value).toBe("");
  });
});

describe("parseMarketValue", () => {
  it("takes a point or a comma with up to 4 decimals", () => {
    expect(parseMarketValue("520")).toBe("520");
    expect(parseMarketValue("520,5")).toBe("520.5");
    expect(parseMarketValue(" 0.0001 ")).toBe("0.0001");
  });

  it("refuses zero, a sign, grouping, a fifth decimal and text", () => {
    expect(parseMarketValue("0")).toBeUndefined();
    expect(parseMarketValue("0.0000")).toBeUndefined();
    expect(parseMarketValue("-5")).toBeUndefined();
    expect(parseMarketValue("1,000.50")).toBeUndefined();
    expect(parseMarketValue("1.00001")).toBeUndefined();
    expect(parseMarketValue("")).toBeUndefined();
  });

  it("takes at most 14 whole digits, which always fit the backend amount", () => {
    // The backend keeps an amount as a 64-bit whole number at 4 decimals: at most
    // 922,337,203,685,477.5807, so 14 digits always fit and some 15-digit values do not.
    expect(parseMarketValue("99999999999999")).toBe("99999999999999");
    expect(parseMarketValue("999999999999999")).toBeUndefined();
  });
});

describe("stockMergerAnswer", () => {
  it("has no answer until the user chooses", () => {
    expect(stockMergerAnswer(check, initialStockMergerForm(check))).toBeUndefined();
  });

  it("gives the taxable answer with the value and the date", () => {
    expect(stockMergerAnswer(check, { treatment: "taxable", value: "520", date: "2024-06-05" })).toEqual({
      treatment: "taxable",
      marketValue: "520",
      effectiveDate: "2024-06-05",
    });
  });

  it("waits for a value above 0 when taxable", () => {
    expect(stockMergerAnswer(check, { treatment: "taxable", value: "", date: "2024-06-03" })).toBeUndefined();
    expect(stockMergerAnswer(check, { treatment: "taxable", value: "0", date: "2024-06-03" })).toBeUndefined();
  });

  it("gives the deferred answer without a value", () => {
    expect(stockMergerAnswer(check, { treatment: "deferred", value: "", date: "2024-06-03" })).toEqual({
      treatment: "deferred",
      effectiveDate: "2024-06-03",
    });
  });

  it("waits for a date between the rows", () => {
    expect(stockMergerAnswer(check, { treatment: "deferred", value: "", date: "2024-06-06" })).toBeUndefined();
    expect(stockMergerAnswer(check, { treatment: "deferred", value: "", date: "2024-6-3" })).toBeUndefined();
  });
});

describe("stockMergerProblems", () => {
  it("has none before the user chooses", () => {
    expect(stockMergerProblems(check, initialStockMergerForm(check))).toEqual({ value: false, date: false });
  });

  it("marks a value that is not an amount and a date outside the rows", () => {
    expect(stockMergerProblems(check, { treatment: "taxable", value: "abc", date: "2024-06-06" })).toEqual({
      value: true,
      date: true,
    });
  });

  it("does not check the value of a deferral", () => {
    expect(stockMergerProblems(check, { treatment: "deferred", value: "abc", date: "2024-06-04" })).toEqual({
      value: false,
      date: false,
    });
  });

  it("has no problem with an empty field: the button waits", () => {
    expect(stockMergerProblems(check, { treatment: "taxable", value: "", date: "" })).toEqual({
      value: false,
      date: false,
    });
  });
});
