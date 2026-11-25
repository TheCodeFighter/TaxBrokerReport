import { describe, expect, it } from "vitest";

import { initialIsinChangeForm, isinChangeAnswer, isinChangeProblems } from "./isin_change_question";
import type { IsinChangeCheck } from "./questions";

// The rows are booked on 2024-03-04 (the old ISIN) and 2024-03-06 (the new one).
const check: IsinChangeCheck = {
  oldIsin: "XX0000000134",
  oldName: "Old Synthetic Share",
  newIsin: "XX0000000135",
  newName: "New Synthetic Share",
  effectiveDate: "2024-03-04",
  earliestDate: "2024-03-04",
  latestDate: "2024-03-06",
  sources: [
    { broker: "interactive_brokers", date: "2024-03-04", transactionId: "t-3", sourceValue: "-20" },
    { broker: "interactive_brokers", date: "2024-03-06", transactionId: "t-4", sourceValue: "20" },
  ],
};

describe("initialIsinChangeForm", () => {
  it("starts with no choice, 1 : 1 and the date of the old row when the rows give no ratio", () => {
    expect(initialIsinChangeForm(check)).toEqual({
      sameIssuer: undefined,
      newShares: "1",
      oldShares: "1",
      date: "2024-03-04",
    });
  });

  it("starts with the ratio of the rows: 20 units out and 40 in is 2 : 1", () => {
    expect(initialIsinChangeForm({ ...check, suggestedRatio: { newShares: 2, oldShares: 1 } })).toEqual({
      sameIssuer: undefined,
      newShares: "2",
      oldShares: "1",
      date: "2024-03-04",
    });
  });
});

describe("isinChangeAnswer", () => {
  it("has no answer before the user chooses", () => {
    expect(isinChangeAnswer(check, initialIsinChangeForm(check))).toBeUndefined();
  });

  it("answers no with the word, whatever the other fields hold", () => {
    expect(isinChangeAnswer(check, { sameIssuer: "no", newShares: "x", oldShares: "", date: "" })).toBe("different_issuer");
  });

  it("answers yes with the two numbers and the date", () => {
    expect(isinChangeAnswer(check, { sameIssuer: "yes", newShares: "2", oldShares: "1", date: "2024-03-05" })).toEqual({
      newShares: 2,
      oldShares: 1,
      effectiveDate: "2024-03-05",
    });
  });

  it("waits while a number is 0 or not a whole number, or the date is outside the rows", () => {
    const yes = { sameIssuer: "yes" as const, newShares: "1", oldShares: "1", date: "2024-03-04" };

    expect(isinChangeAnswer(check, { ...yes, newShares: "0" })).toBeUndefined();
    expect(isinChangeAnswer(check, { ...yes, oldShares: "1,5" })).toBeUndefined();
    expect(isinChangeAnswer(check, { ...yes, date: "2024-03-03" })).toBeUndefined();
    expect(isinChangeAnswer(check, { ...yes, date: "2024-03-07" })).toBeUndefined();
    expect(isinChangeAnswer(check, { ...yes, date: "04.03.2024" })).toBeUndefined();
  });
});

describe("isinChangeProblems", () => {
  const yes = { sameIssuer: "yes" as const, newShares: "1", oldShares: "1", date: "2024-03-04" };

  it("has none for a valid form, for no, and for an empty field", () => {
    expect(isinChangeProblems(check, yes)).toEqual({ numbers: false, date: false });
    expect(isinChangeProblems(check, { ...yes, sameIssuer: "no", newShares: "x" })).toEqual({ numbers: false, date: false });
    expect(isinChangeProblems(check, { ...yes, newShares: "", date: "" })).toEqual({ numbers: false, date: false });
  });

  it("marks the numbers and the date that the server would refuse", () => {
    expect(isinChangeProblems(check, { ...yes, oldShares: "0" })).toEqual({ numbers: true, date: false });
    expect(isinChangeProblems(check, { ...yes, date: "2024-04-01" })).toEqual({ numbers: false, date: true });
  });
});
