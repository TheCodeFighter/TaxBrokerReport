import { describe, expect, it } from "vitest";

import {
  choiceText,
  incomeAnswer,
  NO_CLAIM_ANSWER,
  payerAnswer,
  shownClassLabels,
  SKIP_ANSWER,
  treatyAnswer,
  treatyText,
} from "./income_questions";

describe("incomeAnswer", () => {
  it("has no answer until a choice is made", () => {
    expect(incomeAnswer(undefined, undefined)).toBeUndefined();
    expect(incomeAnswer(undefined, "broker_cash")).toBeUndefined();
  });

  it("answers a plain choice with its word", () => {
    expect(incomeAnswer("dividends", undefined)).toBe("dividends");
    expect(incomeAnswer("bank_deposit_interest", undefined)).toBe("bank_deposit_interest");
    expect(incomeAnswer("exclude_from_supported_reports", undefined)).toBe("exclude_from_supported_reports");
  });

  it("needs the kind of other interest before there is an answer", () => {
    expect(incomeAnswer("other_interest", undefined)).toBeUndefined();
    expect(incomeAnswer("other_interest", "broker_cash")).toEqual({ choice: "other_interest", subtype: "broker_cash" });
    expect(incomeAnswer("other_interest", "bond_coupon")).toEqual({ choice: "other_interest", subtype: "bond_coupon" });
  });

  it("does not send a kind with a choice that has none", () => {
    // The user picked a kind, then changed the choice to dividends: the kind is not sent.
    expect(incomeAnswer("dividends", "broker_cash")).toBe("dividends");
  });
});

describe("payerAnswer", () => {
  it("picks the position of the row among the options", () => {
    expect(payerAnswer(undefined)).toBeUndefined();
    expect(payerAnswer(0)).toEqual({ option: 0 });
    expect(payerAnswer(1)).toEqual({ option: 1 });
  });

  it("has the words of a skip and of no treaty claim", () => {
    expect(SKIP_ANSWER).toBe("skip");
    expect(NO_CLAIM_ANSWER).toBe("no_claim");
  });
});

describe("treatyText", () => {
  it("trims spaces and no-break spaces and gives the text", () => {
    expect(treatyText("  Article 10  ")).toEqual({ ok: true, text: "Article 10" });
    expect(treatyText("  Article 10  ")).toEqual({ ok: true, text: "Article 10" });
  });

  it("refuses an empty text, also one of spaces only", () => {
    expect(treatyText("")).toEqual({ ok: false, problem: "empty" });
    expect(treatyText("     ")).toEqual({ ok: false, problem: "empty" });
  });

  it("allows exactly 100 characters and refuses 101", () => {
    expect(treatyText("a".repeat(100)).ok).toBe(true);
    expect(treatyText("a".repeat(101))).toEqual({ ok: false, problem: "tooLong" });
  });

  it("counts characters, not UTF-16 units", () => {
    // 100 emoji are 200 UTF-16 units but 100 characters.
    expect(treatyText("\u{1F600}".repeat(100)).ok).toBe(true);
    expect(treatyText("\u{1F600}".repeat(101))).toEqual({ ok: false, problem: "tooLong" });
  });

  it("refuses the words true and false in any case", () => {
    expect(treatyText("true")).toEqual({ ok: false, problem: "yesNo" });
    expect(treatyText("TRUE")).toEqual({ ok: false, problem: "yesNo" });
    expect(treatyText(" False ")).toEqual({ ok: false, problem: "yesNo" });
    expect(treatyText("true story").ok).toBe(true);
  });

  it("refuses control characters and a lone surrogate", () => {
    expect(treatyText("a\u0007b")).toEqual({ ok: false, problem: "characters" });
    expect(treatyText("a\u007fb")).toEqual({ ok: false, problem: "characters" });
    expect(treatyText("a\u0085b")).toEqual({ ok: false, problem: "characters" });
    expect(treatyText("a\ud800b")).toEqual({ ok: false, problem: "characters" });
  });
});

describe("treatyAnswer", () => {
  it("sends the trimmed text as the statement", () => {
    expect(treatyAnswer("  Article 10  ")).toEqual({ statement: "Article 10" });
  });

  it("has no answer for a text the server would refuse", () => {
    expect(treatyAnswer("")).toBeUndefined();
    expect(treatyAnswer("TRUE")).toBeUndefined();
    expect(treatyAnswer("a".repeat(101))).toBeUndefined();
  });
});

describe("choiceText", () => {
  it("has a text for every word the server offers", () => {
    const words = [
      "capital_gains",
      "exclude_from_supported_reports",
      "dividends",
      "bank_deposit_interest",
      "other_interest",
      "broker_cash",
      "bond_coupon",
      "ordinary",
      "discount_or_zero_coupon",
      "above_limit",
      "not_above_limit",
    ];

    expect(words.filter((word) => choiceText(word) === undefined)).toEqual([]);
  });

  it("has no text for a word it does not know", () => {
    expect(choiceText("something_new")).toBeUndefined();
  });
});

describe("shownClassLabels", () => {
  it("leaves out the empty label of a row whose class column is empty", () => {
    expect(shownClassLabels([""])).toEqual([]);
    expect(shownClassLabels(["", "  ", "Stock"])).toEqual(["Stock"]);
  });
});
