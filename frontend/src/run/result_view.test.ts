import { describe, expect, it } from "vitest";

import { countDiagnostics, markOutOfDate, needsAnswerHint, overallStatusText, reportRows } from "./result_view";
import type { RunResult } from "./submit";

const result: RunResult = {
  status: "needs_input",
  runRevision: 1,
  reports: {
    otherInterest: { status: "no_data" },
    capitalGains: { status: "needs_input" },
    dividends: { status: "generated" },
  },
  diagnostics: [{ severity: "error" }, { severity: "warning" }, { severity: "warning" }, { severity: "info" }],
};

describe("result view", () => {
  it("lists the forms in a fixed order with their status", () => {
    expect(reportRows(result)).toEqual([
      { form: "Doh-KDVP", statusText: "reportNeedsInput" },
      { form: "Doh-Div", statusText: "reportGenerated" },
      { form: "Doh-Obr", statusText: "reportNoData" },
    ]);
  });

  it("counts errors and warnings", () => {
    expect(countDiagnostics(result)).toEqual({ errors: 1, warnings: 2 });
  });

  it("has a text for each overall status and a failed text for an unknown one", () => {
    expect(overallStatusText("success")).toBe("statusSuccess");
    expect(overallStatusText("partial_success")).toBe("statusPartialSuccess");
    expect(overallStatusText("something_new")).toBe("statusFailed");
  });

  it("shows the answers-later hint only for a needs_input result", () => {
    expect(needsAnswerHint(result)).toBe(true);
    expect(needsAnswerHint({ ...result, status: "success" })).toBe(false);
  });

  it("marks a shown result as out of date and keeps the result itself", () => {
    const shown = { result, fileLabels: ["a.csv"], outOfDate: false };
    const marked = markOutOfDate(shown);

    expect(marked?.outOfDate).toBe(true);
    expect(marked?.result).toBe(result);
    expect(marked?.fileLabels).toEqual(["a.csv"]);
    expect(shown.outOfDate).toBe(false);
  });

  it("has nothing to mark when there is no result", () => {
    expect(markOutOfDate(undefined)).toBeUndefined();
  });
});
