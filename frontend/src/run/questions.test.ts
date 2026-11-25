import { describe, expect, it } from "vitest";

import {
  defaultEffectiveDate,
  enteredRatio,
  firstQuestionId,
  hasQuestionWithoutScreen,
  placeQuestions,
  januaryHint,
  quantityAnswer,
  questionAnchor,
  questionKind,
  rangeOf,
  ratioAnswer,
  ratioPreview,
  ratioProblems,
  rejectionTargets,
  supportedQuestions,
  CANCEL_ANSWER,
  SEPARATE_EVENTS_ANSWER,
  type DecisionRequest,
  type LossSale,
  type RatioCheck,
  type RatioForm,
} from "./questions";
import type { RunResult } from "./submit";

function request(id: string, type: string): DecisionRequest {
  return { id, type, runRevision: 1, affectedReports: ["capital_gains"], sources: [] };
}

function resultWith(requests: DecisionRequest[]): RunResult {
  return {
    status: "needs_input",
    runRevision: 1,
    reports: {},
    diagnostics: [],
    excludedItems: [],
    requests,
    decisions: [],
    corrections: [],
    reconciliation: [],
  };
}

// One range, as the question of a split on 2023-01-15 holds it: 1.0 units before the action.
const oneRange: RatioCheck = {
  name: "Example Split Corp",
  isin: "US0000000001",
  effectiveDate: "2023-01-15",
  sources: [{ broker: "trade_republic", date: "2023-01-15", transactionId: "T-1", sourceValue: "1.00000000" }],
  positionBefore: "1.00000000",
  dateRanges: [
    { earliestDate: "2023-01-01", latestDate: "2023-12-31", positionBefore: "1.00000000", lowestChangeAfter: "1.00000000" },
  ],
  askSameEvent: false,
};

// Two ranges: a purchase on 2024-11-12 changes the position from 1.0 to 1.1 units.
const twoRanges: RatioCheck = {
  name: "Example Split Corp",
  isin: "US0000000001",
  effectiveDate: "2024-11-10",
  sources: [{ broker: "trade_republic", date: "2024-11-10", transactionId: "T-2" }],
  positionBefore: "1.00000000",
  dateRanges: [
    { earliestDate: "2024-01-11", latestDate: "2024-11-12", positionBefore: "1.00000000", lowestChangeAfter: "1.00000000" },
    { earliestDate: "2024-11-13", latestDate: "2024-12-31", positionBefore: "1.10000000", lowestChangeAfter: "1.10000000" },
  ],
  askSameEvent: false,
};

describe("question kinds", () => {
  it("knows the capital request types that have a screen", () => {
    expect(questionKind({ type: "loss_eligibility_confirmation_required" })).toBe("loss");
    expect(questionKind({ type: "corporate_action_ratio_required" })).toBe("ratio");
    expect(questionKind({ type: "corporate_action_identity_required" })).toBe("identity");
    expect(questionKind({ type: "corporate_action_isin_change_required" })).toBe("isinChange");
    expect(questionKind({ type: "quantity_correction_required" })).toBe("quantity");
    expect(questionKind({ type: "deposit_interest_total_required" })).toBe("deposit");
  });

  it("tells the two kinds of a classification request by its data", () => {
    const trade = { name: "Example Share", isin: "US0000000001", classLabels: ["Stock"] };
    const income = {
      eventKind: "dividend",
      receiptDate: "2023-05-02",
      name: "Example Share",
      classLabels: [],
      grossAmount: "10.0000",
      currency: "USD",
    };

    expect(questionKind({ type: "tax_classification_required", classificationCheck: trade })).toBe("classification");
    expect(questionKind({ type: "tax_classification_required", incomeCheck: income })).toBe("income");
    expect(questionKind({ type: "tax_classification_required" })).toBeUndefined();
  });

  it("tells the four kinds of an income details request by its data", () => {
    const payer = { name: "Example Corp", isin: "US0000000001", options: [] };
    const country = {
      name: "Example Corp",
      isin: "US0000000001",
      candidates: [
        { country: "IE", origin: "export" },
        { country: "US", origin: "reference" },
      ],
    };
    const bond = { isin: "XS0000000001", name: "Example Bond", soldUnits: "1.00000000", maturedUnits: "0.00000000" };
    const treaty = { year: 2023, country: "BR", isins: ["BR0000000001"] };

    expect(questionKind({ type: "income_details_required", payerCheck: payer })).toBe("payer");
    expect(questionKind({ type: "income_details_required", countryCheck: country })).toBe("country");
    expect(questionKind({ type: "income_details_required", bondCheck: bond })).toBe("bond");
    expect(questionKind({ type: "income_details_required", treatyCheck: treaty })).toBe("treaty");
    expect(questionKind({ type: "income_details_required" })).toBeUndefined();
  });

  it("has the failure screen for the three failure questions", () => {
    expect(questionKind({ type: "capital_gains_failure_resolution_required" })).toBe("failure");
    expect(questionKind({ type: "dividends_failure_resolution_required" })).toBe("failure");
    expect(questionKind({ type: "other_interest_failure_resolution_required" })).toBe("failure");
  });

  it("has the confirmation screen for the developer export steps", () => {
    expect(questionKind({ type: "developer_export_confirmation_required" })).toBe("developer");
  });

  it("has no screen for an unknown request type", () => {
    expect(questionKind({ type: "unknown_future_question_required" })).toBeUndefined();
  });

  it("names the page anchor of a question", () => {
    expect(questionAnchor("request-0002")).toBe("question-request-0002");
  });
});

describe("firstQuestionId", () => {
  it("is the first open question that has a screen", () => {
    const result = resultWith([
      request("request-0001", "unknown_future_question_required"),
      request("request-0002", "loss_eligibility_confirmation_required"),
      request("request-0003", "quantity_correction_required"),
    ]);

    expect(firstQuestionId(result)).toBe("request-0002");
    expect(supportedQuestions(result).map((entry) => entry.id)).toEqual(["request-0002", "request-0003"]);
  });

  it("is undefined when no open question has a screen, so there is no link", () => {
    const result = resultWith([request("request-0001", "unknown_future_question_required")]);

    expect(firstQuestionId(result)).toBeUndefined();
    expect(firstQuestionId(resultWith([]))).toBeUndefined();
  });
});

describe("placeQuestions with two failure questions", () => {
  it("puts the capital question in Doh-KDVP and the other-interest question in Doh-Obr", () => {
    const capital: DecisionRequest = {
      ...request("request-0001", "capital_gains_failure_resolution_required"),
      investments: [{ name: "Example Share", isin: "XX0000000001", reasonCodes: ["incomplete_history"] }],
    };
    const other: DecisionRequest = {
      ...request("request-0002", "other_interest_failure_resolution_required"),
      affectedReports: ["other_interest"],
      investments: [{ name: "Example Bond", isin: "XX0000000002", reasonCodes: ["missing_exchange_rate"] }],
      brokers: [{ broker: "trade_republic", reasonCodes: ["missing_exchange_rate"] }],
    };
    const result = resultWith([capital, other]);

    result.reports = {
      capitalGains: { status: "needs_input", diagnosticIds: [], requestIds: ["request-0001"], unsafe: false },
      otherInterest: { status: "needs_input", diagnosticIds: [], requestIds: ["request-0002"], unsafe: false },
    };

    const placed = placeQuestions(result, ["capitalGains", "dividends", "bankInterest", "otherInterest"]);

    expect(placed.byReport.get("capitalGains")?.map((entry) => entry.id)).toEqual(["request-0001"]);
    expect(placed.byReport.get("otherInterest")?.map((entry) => entry.id)).toEqual(["request-0002"]);
    expect(placed.rest).toEqual([]);
  });
});

describe("hasQuestionWithoutScreen", () => {
  it("is false for a loss, a classification and a failure question and true beside an unknown question", () => {
    const loss = request("request-0001", "loss_eligibility_confirmation_required");
    const classification: DecisionRequest = {
      ...request("request-0002", "tax_classification_required"),
      classificationCheck: { name: "Example Share", isin: "US0000000001", classLabels: [] },
    };
    const failure = request("request-0003", "dividends_failure_resolution_required");
    const unknown = request("request-0004", "unknown_future_question_required");

    expect(hasQuestionWithoutScreen(resultWith([loss]))).toBe(false);
    expect(hasQuestionWithoutScreen(resultWith([loss, classification, failure]))).toBe(false);
    expect(hasQuestionWithoutScreen(resultWith([classification, unknown]))).toBe(true);
  });
});

describe("answers", () => {
  it("has the words of the loss and cancel answers", () => {
    expect(CANCEL_ANSWER).toBe("cancel");
    expect(SEPARATE_EVENTS_ANSWER).toBe("separate_events");
  });

  it("turns a quantity into the corrected units with 8 decimals", () => {
    expect(quantityAnswer("0,99999998")).toEqual({ correctedUnits: "0.99999998" });
    expect(quantityAnswer("0,5")).toEqual({ correctedUnits: "0.50000000" });
    expect(quantityAnswer("0.5")).toEqual({ correctedUnits: "0.50000000" });
    expect(quantityAnswer("1")).toEqual({ correctedUnits: "1.00000000" });
    expect(quantityAnswer("1.000,5")).toBeUndefined();
    expect(quantityAnswer("")).toBeUndefined();
  });
});

describe("ratio form", () => {
  const ratioMode = (form: Partial<RatioForm>): RatioForm => ({
    mode: "ratio",
    newShares: "",
    oldShares: "",
    before: "",
    after: "",
    date: "",
    ...form,
  });

  it("reduces the entered numbers", () => {
    expect(enteredRatio(ratioMode({ newShares: "20", oldShares: "10" }))).toEqual({ newShares: 2, oldShares: 1 });
  });

  it("turns the shares before and after into the ratio", () => {
    const form = ratioMode({ mode: "shares", before: "0.52", after: "5.20" });

    expect(enteredRatio(form)).toEqual({ newShares: 10, oldShares: 1 }); // 5.20 / 0.52
  });

  it("has no ratio for equal numbers, a decimal or an empty field", () => {
    expect(enteredRatio(ratioMode({ newShares: "2", oldShares: "2" }))).toBeUndefined();
    expect(enteredRatio(ratioMode({ newShares: "1.5", oldShares: "1" }))).toBeUndefined();
    expect(enteredRatio(ratioMode({ newShares: "", oldShares: "1" }))).toBeUndefined();
  });

  it("answers 2 for 1 on 2023-01-15 with the position 1.00000000 and the confirmed 2.00000000", () => {
    const preview = ratioPreview(oneRange, ratioMode({ newShares: "2", oldShares: "1", date: "2023-01-15" }));

    expect(preview).toEqual({
      ratio: { newShares: 2, oldShares: 1 },
      date: "2023-01-15",
      position: "1.00000000",
      units: "2.00000000",
      unusual: false,
    });
    expect(preview && ratioAnswer(preview)).toEqual({
      newShares: 2,
      oldShares: 1,
      effectiveDate: "2023-01-15",
      confirmedUnits: "2.00000000",
    });
  });

  it("answers 20 for 10 as 2 for 1", () => {
    const preview = ratioPreview(oneRange, ratioMode({ newShares: "20", oldShares: "10", date: "2023-01-15" }));

    expect(preview && ratioAnswer(preview)).toEqual({
      newShares: 2,
      oldShares: 1,
      effectiveDate: "2023-01-15",
      confirmedUnits: "2.00000000",
    });
  });

  it("answers the shares 0.52 before and 5.20 after as 10 for 1", () => {
    const check: RatioCheck = {
      ...oneRange,
      positionBefore: "0.52000000",
      dateRanges: [
        { earliestDate: "2023-01-01", latestDate: "2023-12-31", positionBefore: "0.52000000", lowestChangeAfter: "0.52000000" },
      ],
    };
    const preview = ratioPreview(check, ratioMode({ mode: "shares", before: "0.52", after: "5.20", date: "2023-01-15" }));

    expect(preview && ratioAnswer(preview)).toEqual({
      newShares: 10,
      oldShares: 1,
      effectiveDate: "2023-01-15",
      confirmedUnits: "5.20000000", // 0.52 x 10
    });
  });

  it("warns about a ratio with a number above 1000", () => {
    const check: RatioCheck = {
      ...oneRange,
      positionBefore: "0.52000000",
      dateRanges: [
        { earliestDate: "2023-01-01", latestDate: "2023-12-31", positionBefore: "0.52000000", lowestChangeAfter: "0.52000000" },
      ],
    };
    const preview = ratioPreview(
      check,
      ratioMode({ mode: "shares", before: "0.52", after: "5.20000001", date: "2023-01-15" }),
    );

    expect(preview?.ratio).toEqual({ newShares: 520000001, oldShares: 52000000 });
    expect(preview?.unusual).toBe(true);
  });

  it("has no preview while the date is outside the ranges or the ratio is not valid", () => {
    expect(ratioPreview(oneRange, ratioMode({ newShares: "2", oldShares: "1", date: "2024-01-01" }))).toBeUndefined();
    expect(ratioPreview(oneRange, ratioMode({ newShares: "2", oldShares: "2", date: "2023-01-15" }))).toBeUndefined();
  });
});

describe("ratioProblems", () => {
  const form = (changed: Partial<RatioForm>): RatioForm => ({
    mode: "ratio",
    newShares: "",
    oldShares: "",
    before: "",
    after: "",
    date: "",
    ...changed,
  });

  it("has no problem while a field is still empty", () => {
    expect(ratioProblems(oneRange, form({}))).toEqual({ numbers: false, date: false });
    expect(ratioProblems(oneRange, form({ newShares: "2" }))).toEqual({ numbers: false, date: false });
  });

  it("has a numbers problem for 2 : 2, 0 : 1 and a decimal", () => {
    expect(ratioProblems(oneRange, form({ newShares: "2", oldShares: "2", date: "2023-01-15" })).numbers).toBe(true);
    expect(ratioProblems(oneRange, form({ newShares: "0", oldShares: "1", date: "2023-01-15" })).numbers).toBe(true);
    expect(ratioProblems(oneRange, form({ newShares: "1.5", oldShares: "1", date: "2023-01-15" })).numbers).toBe(true);
  });

  it("has a numbers problem for equal share counts and none for a good ratio", () => {
    expect(ratioProblems(oneRange, form({ mode: "shares", before: "0.52", after: "0.52" })).numbers).toBe(true);
    expect(ratioProblems(oneRange, form({ mode: "shares", before: "0.52", after: "5.20" })).numbers).toBe(false);
    expect(ratioProblems(oneRange, form({ newShares: "2", oldShares: "1", date: "2023-01-15" }))).toEqual({
      numbers: false,
      date: false,
    });
  });

  it("has a numbers problem when the units after the split round to 0", () => {
    // 0.00000001 units x 1 / 3 = 0.0000000033, which rounds to 0.
    const tiny: RatioCheck = {
      ...oneRange,
      dateRanges: [
        { earliestDate: "2023-01-01", latestDate: "2023-12-31", positionBefore: "0.00000001", lowestChangeAfter: "0.00000001" },
      ],
    };

    expect(ratioProblems(tiny, form({ newShares: "1", oldShares: "3", date: "2023-01-15" })).numbers).toBe(true);
  });

  it("has a date problem only for a date outside every range", () => {
    expect(ratioProblems(oneRange, form({ date: "2030-01-01" })).date).toBe(true);
    expect(ratioProblems(oneRange, form({ date: "2023-1-5" })).date).toBe(true);
    expect(ratioProblems(oneRange, form({ date: "2023-01-15" })).date).toBe(false);
  });
});

describe("effective date", () => {
  it("takes the position of the range that holds the date", () => {
    expect(rangeOf(twoRanges, "2024-12-01")?.positionBefore).toBe("1.10000000");
    expect(rangeOf(twoRanges, "2024-11-12")?.positionBefore).toBe("1.00000000"); // the last day of range 1
    expect(rangeOf(twoRanges, "2024-11-13")?.positionBefore).toBe("1.10000000"); // the first day of range 2
  });

  it("has no range for a date outside them or a text that is not a date", () => {
    expect(rangeOf(twoRanges, "2025-01-02")).toBeUndefined();
    expect(rangeOf(twoRanges, "2024-01-10")).toBeUndefined();
    expect(rangeOf(twoRanges, "2024-1-5")).toBeUndefined();
    expect(rangeOf(twoRanges, "")).toBeUndefined();
  });

  it("starts with the row date only when it is in a range", () => {
    expect(defaultEffectiveDate(twoRanges)).toBe("2024-11-10");
    expect(defaultEffectiveDate({ ...twoRanges, effectiveDate: "2025-01-02" })).toBe("");
  });

  it("uses the position of the entered date for the preview", () => {
    const form: RatioForm = { mode: "ratio", newShares: "2", oldShares: "1", before: "", after: "", date: "2024-12-01" };

    expect(ratioPreview(twoRanges, form)?.units).toBe("2.20000000"); // 1.1 x 2
    expect(ratioPreview(twoRanges, { ...form, date: "2024-06-01" })?.units).toBe("2.00000000"); // 1.0 x 2
  });
});

describe("rejectionTargets", () => {
  it("marks the field a message names", () => {
    expect(rejectionTargets("answer_rejected.ratio_not_positive", { requestId: "request-0001", field: "oldShares" })).toEqual([
      "oldShares",
    ]);
    expect(rejectionTargets("answer_rejected.ratio_not_positive", { requestId: "request-0001", field: "newShares" })).toEqual([
      "newShares",
    ]);
  });

  it("marks both numbers for an unchanged or an out-of-range ratio", () => {
    expect(rejectionTargets("answer_rejected.ratio_unchanged", {})).toEqual(["newShares", "oldShares"]);
    expect(rejectionTargets("answer_rejected.ratio_out_of_range", {})).toEqual(["newShares", "oldShares"]);
    expect(rejectionTargets("answer_rejected.isin_change_ratio_mismatch", {})).toEqual(["newShares", "oldShares"]);
  });

  it("marks the ratio and the date when a later sale is too large", () => {
    expect(rejectionTargets("answer_rejected.later_sale_too_large", {})).toEqual([
      "newShares",
      "oldShares",
      "effectiveDate",
    ]);
  });

  it("marks the confirmation, the date and the corrected quantity", () => {
    expect(rejectionTargets("answer_rejected.preview_mismatch", {})).toEqual(["confirmation"]);
    expect(rejectionTargets("answer_rejected.effective_date_out_of_range", {})).toEqual(["effectiveDate"]);
    expect(rejectionTargets("answer_rejected.effective_date_without_units", {})).toEqual(["effectiveDate"]);
    expect(rejectionTargets("answer_rejected.correction_over_budget", {})).toEqual(["correctedUnits"]);
    expect(rejectionTargets("answer_rejected.correction_not_positive", {})).toEqual(["correctedUnits"]);
    expect(rejectionTargets("answer_rejected.correction_above_open_units", {})).toEqual(["correctedUnits"]);
    expect(rejectionTargets("answer_rejected.correction_too_large", {})).toEqual(["correctedUnits"]);
  });

  it("marks the list of action numbers for an incomplete or too wide grouping", () => {
    expect(rejectionTargets("answer_rejected.grouping_incomplete", { requestId: "request-0001" })).toEqual(["grouping"]);
    expect(rejectionTargets("answer_rejected.grouping_window", { requestId: "request-0001", days: 10 })).toEqual([
      "grouping",
    ]);
  });

  it("loads the run again for a stale revision or an unknown question", () => {
    expect(rejectionTargets("answer_rejected.stale_revision", {})).toEqual(["reload"]);
    expect(rejectionTargets("answer_rejected.unknown_request", {})).toEqual(["reload"]);
  });

  it("shows any other rejection beside the send button", () => {
    expect(rejectionTargets("answer_rejected.duplicate_answer", {})).toEqual(["form"]);
    expect(rejectionTargets("answer_rejected.something_new", {})).toEqual(["form"]);
  });
});

describe("januaryHint", () => {
  const sale = (saleDate: string, windowEnd: string): LossSale => ({
    saleDate,
    saleUnits: "1.00000000",
    lossUnits: "1.00000000",
    windowStart: "2024-05-11",
    windowEnd,
    knownReplacements: [],
    knownIneligibleUnits: "0.00000000",
    openUnits: "1.00000000",
  });

  it("is true when the window of a December sale ends in the next year", () => {
    expect(januaryHint(sale("2024-12-20", "2025-01-19"))).toBe(true);
  });

  it("is false when the window ends in the year of the sale", () => {
    expect(januaryHint(sale("2024-06-10", "2024-07-10"))).toBe(false);
    expect(januaryHint(sale("2024-12-01", "2024-12-31"))).toBe(false); // ends on 31 December itself
  });
});

describe("placeQuestions", () => {
  const loss = request("request-0001", "loss_eligibility_confirmation_required");
  const ratio = request("request-0002", "corporate_action_ratio_required");
  const classification = request("request-0003", "tax_classification_required");

  it("places each question in the first form that lists it and leaves out a type without a screen", () => {
    const result: RunResult = {
      ...resultWith([loss, ratio, classification]),
      reports: {
        capitalGains: {
          status: "needs_input",
          diagnosticIds: [],
          requestIds: ["request-0001", "request-0002", "request-0003"],
          unsafe: false,
        },
        otherInterest: { status: "needs_input", diagnosticIds: [], requestIds: ["request-0001"], unsafe: false },
      },
    };

    const placed = placeQuestions(result, ["capitalGains", "dividends", "otherInterest"]);

    expect(placed.byReport.get("capitalGains")).toEqual([loss, ratio]);
    expect(placed.byReport.has("otherInterest")).toBe(false); // request-0001 is already in the first form
    expect(placed.rest).toEqual([]);
  });

  it("keeps a question that no form lists, so it is never hidden", () => {
    const result: RunResult = {
      ...resultWith([loss]),
      reports: { capitalGains: { status: "generated", diagnosticIds: [], requestIds: [], unsafe: false } },
    };

    const placed = placeQuestions(result, ["capitalGains"]);

    expect(placed.byReport.size).toBe(0);
    expect(placed.rest).toEqual([loss]);
  });
});
