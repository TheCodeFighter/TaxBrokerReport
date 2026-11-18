import { describe, expect, it } from "vitest";

import {
  brokerTitle,
  classificationExclusions,
  countDiagnostics,
  markOutOfDate,
  needsAnswerHint,
  overallStatusText,
  overallTone,
  reportCards,
  reportRows,
} from "./result_view";
import type { Diagnostic, RunResult } from "./submit";

function diagnostic(id: string, severity: string): Diagnostic {
  return { id, severity, stage: "parsing", code: "inconsistent_value", affectedReports: [], blocking: false, sources: [] };
}

const result: RunResult = {
  status: "needs_input",
  runRevision: 1,
  reports: {
    otherInterest: { status: "no_data", diagnosticIds: [], requestIds: [], unsafe: false },
    capitalGains: { status: "needs_input", diagnosticIds: [], requestIds: ["request-0001"], unsafe: false },
    dividends: { status: "generated", diagnosticIds: [], requestIds: [], unsafe: false, downloadAvailable: true },
  },
  diagnostics: [
    diagnostic("diagnostic-0001", "error"),
    diagnostic("diagnostic-0002", "warning"),
    diagnostic("diagnostic-0003", "warning"),
    diagnostic("diagnostic-0004", "info"),
  ],
  excludedItems: [],
};

describe("result view", () => {
  it("lists the forms in a fixed order with their status", () => {
    expect(reportRows(result)).toEqual([
      { key: "capitalGains", form: "Doh-KDVP", statusText: "reportNeedsInput" },
      { key: "dividends", form: "Doh-Div", statusText: "reportGenerated" },
      { key: "otherInterest", form: "Doh-Obr", statusText: "reportNoData" },
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

  it("gives a partial success another tone than a failure", () => {
    expect(overallTone("success")).toBe("ok");
    expect(overallTone("completed_with_warnings")).toBe("ok");
    expect(overallTone("partial_success")).toBe("warn");
    expect(overallTone("needs_input")).toBe("pending");
    expect(overallTone("failed")).toBe("fail");
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

describe("reportCards", () => {
  it("explains a no_data form by its reason and never gives it the failed tone", () => {
    const noData: RunResult = {
      status: "success",
      runRevision: 1,
      reports: {
        capitalGains: {
          status: "no_data",
          diagnosticIds: [],
          requestIds: [],
          unsafe: false,
          reason: "no_selected_year_disposals",
        },
        dividends: { status: "no_data", diagnosticIds: [], requestIds: [], unsafe: false },
        bankInterest: {
          status: "no_data",
          diagnosticIds: [],
          requestIds: [],
          unsafe: false,
          reason: "below_deposit_interest_threshold",
        },
      },
      diagnostics: [],
      excludedItems: [],
    };

    const cards = reportCards(noData);

    expect(cards.map((card) => [card.form, card.tone, card.noDataText])).toEqual([
      ["Doh-KDVP", "ok", "reportNoDataNoSales"],
      ["Doh-Div", "ok", "reportNoDataEmpty"],
      ["Doh-DHO", "ok", "reportNoDataBelowThreshold"],
    ]);
  });

  it("counts the open questions of a form that needs input", () => {
    const cards = reportCards(result);

    expect(cards[0]?.form).toBe("Doh-KDVP");
    expect(cards[0]?.tone).toBe("pending");
    expect(cards[0]?.questions).toBe(1);
    expect(cards[0]?.noDataText).toBeUndefined();
  });

  it("keeps the failure choice and every excluded investment in order", () => {
    const excluded: RunResult = {
      status: "completed_with_warnings",
      runRevision: 2,
      reports: {
        capitalGains: {
          status: "generated",
          diagnosticIds: ["diagnostic-0001", "diagnostic-0002"],
          requestIds: [],
          failureResolution: "exclude_failed_instruments",
          excludedInvestments: [
            {
              name: "Example Fund",
              isin: "XX0000000001",
              reasonCodes: ["incomplete_history"],
              diagnosticIds: ["diagnostic-0001"],
            },
            {
              name: "Example Shares",
              isin: "XX0000000002",
              reasonCodes: ["corporate_action_ambiguous"],
              diagnosticIds: ["diagnostic-0002"],
            },
          ],
          unsafe: false,
          downloadAvailable: true,
        },
      },
      diagnostics: [],
      excludedItems: [],
    };

    const card = reportCards(excluded)[0];

    expect(card?.tone).toBe("ok");
    expect(card?.resolutionText).toBe("resolutionExclude");
    expect(card?.downloadAvailable).toBe(true);
    expect(card?.excluded.map((investment) => [investment.isin, investment.name])).toEqual([
      ["XX0000000001", "Example Fund"],
      ["XX0000000002", "Example Shares"],
    ]);
  });

  it("keeps the exclusions and the excluded brokers of a form that has no data left", () => {
    const allExcluded: RunResult = {
      status: "completed_with_warnings",
      runRevision: 2,
      reports: {
        otherInterest: {
          status: "no_data",
          diagnosticIds: ["diagnostic-0001"],
          requestIds: [],
          failureResolution: "exclude_failed_instruments",
          excludedInvestments: [
            {
              name: "Example Bond",
              isin: "XS0000000001",
              reasonCodes: ["missing_exchange_rate"],
              diagnosticIds: ["diagnostic-0001"],
            },
          ],
          excludedBrokers: [{ broker: "example_broker", reasonCodes: ["missing_exchange_rate"], diagnosticIds: [] }],
          unsafe: false,
        },
      },
      diagnostics: [],
      excludedItems: [],
    };

    const card = reportCards(allExcluded)[0];

    expect(card?.form).toBe("Doh-Obr");
    expect(card?.noDataText).toBe("reportNoDataEmpty");
    expect(card?.excluded.map((investment) => investment.isin)).toEqual(["XS0000000001"]);
    expect(card?.excludedBrokers).toEqual(["example_broker"]);
  });

  it("marks only the unsafe developer export and lists its investments", () => {
    const unsafe: RunResult = {
      status: "completed_with_warnings",
      runRevision: 3,
      reports: {
        capitalGains: {
          status: "generated",
          diagnosticIds: [],
          requestIds: [],
          failureResolution: "developer_export",
          unsafeInvestments: [
            {
              name: "Example Fund",
              isin: "XX0000000001",
              reasonCodes: ["incomplete_history"],
              diagnosticIds: [],
            },
          ],
          unsafe: true,
          downloadAvailable: true,
        },
        dividends: { status: "generated", diagnosticIds: [], requestIds: [], unsafe: false, downloadAvailable: true },
      },
      diagnostics: [],
      excludedItems: [],
    };

    const cards = reportCards(unsafe);

    expect(cards[0]?.unsafe).toBe(true);
    expect(cards[0]?.resolutionText).toBe("resolutionDeveloper");
    expect(cards[0]?.unsafeInvestments.map((investment) => investment.isin)).toEqual(["XX0000000001"]);
    expect(cards[1]?.unsafe).toBe(false);
    expect(cards[1]?.unsafeInvestments).toEqual([]);
  });

  it("names the failure choice of a stopped form", () => {
    const stopped: RunResult = {
      status: "failed",
      runRevision: 2,
      reports: {
        capitalGains: {
          status: "failed",
          diagnosticIds: [],
          requestIds: [],
          failureResolution: "fail_report",
          excludedInvestments: [],
          unsafe: false,
        },
      },
      diagnostics: [],
      excludedItems: [],
    };

    const card = reportCards(stopped)[0];

    expect(card?.tone).toBe("fail");
    expect(card?.resolutionText).toBe("resolutionFail");
    expect(card?.downloadAvailable).toBe(false);
  });

  it("counts the income rows that need proof of the foreign tax", () => {
    const income: RunResult = {
      status: "success",
      runRevision: 1,
      reports: {
        dividends: {
          status: "generated",
          diagnosticIds: [],
          requestIds: [],
          unsafe: false,
          downloadAvailable: true,
          incomeSummary: { rows: [{ evidenceRequired: true }, { evidenceRequired: false }, { evidenceRequired: true }] },
        },
      },
      diagnostics: [],
      excludedItems: [],
    };

    expect(reportCards(income)[0]?.evidenceRows).toBe(2);
  });

  it("offers a download only for generated forms that the server marks as available", () => {
    const mixed: RunResult = {
      status: "needs_input",
      runRevision: 1,
      reports: {
        capitalGains: { status: "needs_input", diagnosticIds: [], requestIds: ["request-0001"], unsafe: false },
        dividends: { status: "generated", diagnosticIds: [], requestIds: [], unsafe: false, downloadAvailable: true },
        bankInterest: { status: "failed", diagnosticIds: [], requestIds: [], unsafe: false },
        otherInterest: { status: "no_data", diagnosticIds: [], requestIds: [], unsafe: false },
      },
      diagnostics: [],
      excludedItems: [],
    };

    const flags = reportCards(mixed).map((card) => `${card.key}:${card.downloadAvailable}`);

    expect(flags).toEqual([
      "capitalGains:false",
      "dividends:true",
      "bankInterest:false",
      "otherInterest:false",
    ]);
  });

  it("never guesses a download for a generated form without the flag", () => {
    const unflagged: RunResult = {
      status: "success",
      runRevision: 1,
      reports: { capitalGains: { status: "generated", diagnosticIds: [], requestIds: [], unsafe: false } },
      diagnostics: [],
      excludedItems: [],
    };

    expect(reportCards(unflagged)[0]?.downloadAvailable).toBe(false);
  });
});

describe("brokerTitle", () => {
  it("names a broker of the input table and keeps an unknown ID as it is", () => {
    expect(brokerTitle("trade_republic", "sl")).toBe("Trade Republic");
    expect(brokerTitle("example_broker", "en")).toBe("example_broker");
  });
});

describe("classificationExclusions", () => {
  it("lists the classification exclusions apart from the exclusions of a form", () => {
    const bond: RunResult = {
      status: "success",
      runRevision: 1,
      reports: {
        capitalGains: { status: "no_data", diagnosticIds: [], requestIds: [], excludedInvestments: [], unsafe: false },
      },
      diagnostics: [],
      excludedItems: [
        {
          name: "Example Bond",
          isin: "XS0000000001",
          category: "bond",
          reason: "debt_security",
          basis: "tax_rule",
          diagnosticIds: [],
        },
      ],
    };

    expect(classificationExclusions(bond)).toEqual([
      { name: "Example Bond", isin: "XS0000000001", reasonText: "excludedReasonDebtSecurity" },
    ]);
    expect(reportCards(bond)[0]?.excluded).toEqual([]);
  });

  it("keeps an entry without an ISIN and with an unknown reason", () => {
    const unknown: RunResult = {
      status: "success",
      runRevision: 1,
      reports: {},
      diagnostics: [],
      excludedItems: [
        {
          name: "Example Payment",
          category: "unknown",
          reason: "something_new",
          basis: "user_choice",
          diagnosticIds: ["diagnostic-0001"],
        },
      ],
    };

    expect(classificationExclusions(unknown)).toEqual([{ name: "Example Payment" }]);
  });
});
