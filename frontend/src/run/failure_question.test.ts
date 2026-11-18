import { describe, expect, it } from "vitest";

import { failureChoices, failureForm } from "./failure_question";

describe("failureChoices", () => {
  it("keeps the two normal words in their order", () => {
    expect(failureChoices(["exclude_failed_instruments", "fail_report"])).toEqual([
      "exclude_failed_instruments",
      "fail_report",
    ]);
  });

  it("adds the developer export choice last when the request offers it", () => {
    expect(failureChoices(["exclude_failed_instruments", "fail_report", "developer_export"])).toEqual([
      "exclude_failed_instruments",
      "fail_report",
      "developer_export",
    ]);
    expect(failureChoices(["fail_report", "developer_export", "exclude_failed_instruments"])).toEqual([
      "exclude_failed_instruments",
      "fail_report",
      "developer_export",
    ]);
  });

  it("puts the words in the page order and offers nothing when the request has no options", () => {
    expect(failureChoices(["fail_report", "exclude_failed_instruments"])).toEqual([
      "exclude_failed_instruments",
      "fail_report",
    ]);
    expect(failureChoices(undefined)).toEqual([]);
  });
});

describe("failureForm", () => {
  it("names the form of each failure question", () => {
    expect(failureForm("capital_gains_failure_resolution_required")).toBe("Doh-KDVP");
    expect(failureForm("dividends_failure_resolution_required")).toBe("Doh-Div");
    expect(failureForm("other_interest_failure_resolution_required")).toBe("Doh-Obr");
  });

  it("has no form for another type", () => {
    expect(failureForm("loss_eligibility_confirmation_required")).toBeUndefined();
  });
});
