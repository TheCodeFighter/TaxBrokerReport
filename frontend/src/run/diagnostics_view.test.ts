import { describe, expect, it } from "vitest";

import type { MessageCatalogue } from "../i18n/messages";
import {
  affectedForms,
  diagnosticText,
  groupDiagnostics,
  linkedTexts,
  placeDiagnostics,
  severityText,
  sourceLabel,
  stageText,
} from "./diagnostics_view";
import type { Diagnostic, RunResult } from "./submit";

const catalogue: MessageCatalogue = {
  messages: {
    "inconsistent_value.datetime_date": {
      en: "The calendar date in 'datetime' differs; hour {defaultHour} is used.",
      sl: "Koledarski datum v 'datetime' se razlikuje; uporabljena je ura {defaultHour}.",
      params: [{ name: "defaultHour", kind: "number" }],
    },
  },
  terms: {},
};

describe("sourceLabel", () => {
  it("uses the label of the sent file, with its file number on a repeated name", () => {
    const labels = ["taxreport.csv (file 1)", "taxreport.csv (file 2)"];

    expect(sourceLabel({ sourceId: "source-0002", file: "taxreport.csv", row: 42 }, labels, "en")).toBe(
      "taxreport.csv (file 2), row 42",
    );
  });

  it("falls back to the server name for a source outside the sent files", () => {
    expect(sourceLabel({ sourceId: "source-0003", file: "c.csv", row: 7 }, ["a.csv", "b.csv"], "en")).toBe(
      "c.csv, row 7",
    );
  });

  it("names the file by its number when there is no name at all", () => {
    expect(sourceLabel({ sourceId: "source-0003", row: 7 }, ["a.csv", "b.csv"], "en")).toBe("file 3, row 7");
  });

  it("adds the transaction ID, in both languages", () => {
    const source = { sourceId: "source-0001", file: "a.csv", row: 2, transactionId: "T-1" };

    expect(sourceLabel(source, ["a.csv"], "en")).toBe("a.csv, row 2, transaction ID T-1");
    expect(sourceLabel(source, ["a.csv"], "sl")).toBe("a.csv, vrstica 2, ID transakcije T-1");
  });
});

describe("groupDiagnostics", () => {
  it("makes one entry with a count of equal warnings and keeps every source in order", () => {
    const warnings: Diagnostic[] = [
      {
        id: "diagnostic-0001",
        severity: "warning",
        stage: "parsing",
        code: "inconsistent_value",
        messageKey: "inconsistent_value.datetime_date",
        messageParams: { defaultHour: 12 },
        affectedReports: ["capital_gains", "dividends"],
        blocking: false,
        sources: [{ sourceId: "source-0001", file: "a.csv", row: 2 }],
      },
      {
        id: "diagnostic-0002",
        severity: "warning",
        stage: "parsing",
        code: "inconsistent_value",
        messageKey: "inconsistent_value.datetime_date",
        messageParams: { defaultHour: 12 },
        affectedReports: ["capital_gains", "dividends"],
        blocking: false,
        sources: [{ sourceId: "source-0001", file: "a.csv", row: 3 }],
      },
      {
        id: "diagnostic-0003",
        severity: "warning",
        stage: "parsing",
        code: "inconsistent_value",
        messageKey: "inconsistent_value.datetime_date",
        messageParams: { defaultHour: 12 },
        affectedReports: ["capital_gains", "dividends"],
        blocking: false,
        sources: [{ sourceId: "source-0001", file: "a.csv", row: 4 }],
      },
    ];

    const groups = groupDiagnostics(warnings);

    expect(groups).toHaveLength(1);

    const group = groups[0];

    expect(group?.first.id).toBe("diagnostic-0001");
    expect(group?.count).toBe(3);
    expect(group?.sources.map((source) => source.row)).toEqual([2, 3, 4]);
  });

  it("keeps warnings of two different investments apart", () => {
    const warnings: Diagnostic[] = [
      {
        id: "diagnostic-0001",
        severity: "warning",
        stage: "parsing",
        code: "inconsistent_value",
        messageKey: "inconsistent_value.datetime_date",
        messageParams: { defaultHour: 12 },
        affectedReports: ["capital_gains"],
        blocking: false,
        sources: [{ sourceId: "source-0001", row: 2 }],
        instrument: { name: "Example Fund", isin: "XX0000000001" },
      },
      {
        id: "diagnostic-0002",
        severity: "warning",
        stage: "parsing",
        code: "inconsistent_value",
        messageKey: "inconsistent_value.datetime_date",
        messageParams: { defaultHour: 12 },
        affectedReports: ["capital_gains"],
        blocking: false,
        sources: [{ sourceId: "source-0001", row: 3 }],
        instrument: { name: "Example Fund", isin: "XX0000000002" },
      },
    ];

    const groups = groupDiagnostics(warnings);

    expect(groups.map((group) => [group.first.id, group.count])).toEqual([
      ["diagnostic-0001", 1],
      ["diagnostic-0002", 1],
    ]);
  });

  it("never merges two diagnostics that each have a question", () => {
    const errors: Diagnostic[] = [
      {
        id: "diagnostic-0001",
        severity: "error",
        stage: "fifo_tax",
        code: "loss_eligibility_unresolved",
        messageKey: "loss_eligibility_unresolved",
        messageParams: {},
        affectedReports: ["capital_gains"],
        blocking: true,
        sources: [{ sourceId: "source-0001", row: 5 }],
        requestId: "request-0001",
      },
      {
        id: "diagnostic-0002",
        severity: "error",
        stage: "fifo_tax",
        code: "loss_eligibility_unresolved",
        messageKey: "loss_eligibility_unresolved",
        messageParams: {},
        affectedReports: ["capital_gains"],
        blocking: true,
        sources: [{ sourceId: "source-0001", row: 6 }],
        requestId: "request-0002",
      },
    ];

    expect(groupDiagnostics(errors).map((group) => group.count)).toEqual([1, 1]);
  });
});

describe("placeDiagnostics", () => {
  it("puts a diagnostic of one report in its card and the others in the shared list", () => {
    const result: RunResult = {
      status: "failed",
      runRevision: 1,
      reports: {
        capitalGains: {
          status: "failed",
          diagnosticIds: ["diagnostic-0001", "diagnostic-0002"],
          requestIds: [],
          unsafe: false,
        },
        dividends: { status: "generated", diagnosticIds: ["diagnostic-0002"], requestIds: [], unsafe: false },
      },
      diagnostics: [
        {
          id: "diagnostic-0001",
          severity: "error",
          stage: "historical_validation",
          code: "incomplete_history",
          affectedReports: ["capital_gains"],
          blocking: true,
          sources: [],
        },
        {
          id: "diagnostic-0002",
          severity: "warning",
          stage: "parsing",
          code: "inconsistent_value",
          affectedReports: ["capital_gains", "dividends"],
          blocking: false,
          sources: [],
        },
        {
          id: "diagnostic-0003",
          severity: "warning",
          stage: "merging",
          code: "duplicate_transaction",
          affectedReports: [],
          blocking: false,
          sources: [],
        },
      ],
      excludedItems: [],
      requests: [],
      decisions: [],
      corrections: [],
      reconciliation: [],
    };

    const placed = placeDiagnostics(result);

    expect(placed.byReport.get("capitalGains")?.map((diagnostic) => diagnostic.id)).toEqual(["diagnostic-0001"]);
    expect(placed.byReport.get("dividends")).toBeUndefined();
    expect(placed.shared.map((diagnostic) => diagnostic.id)).toEqual(["diagnostic-0002", "diagnostic-0003"]);

    expect(placed.shared.map(affectedForms)).toEqual([["Doh-KDVP", "Doh-Div"], []]);
  });
});

describe("labels and texts", () => {
  it("has a severity text for a warning and a blocking error, and shows an error that no longer blocks as a warning", () => {
    const warning: Diagnostic = {
      id: "diagnostic-0001",
      severity: "warning",
      stage: "parsing",
      code: "inconsistent_value",
      affectedReports: [],
      blocking: false,
      sources: [],
    };
    const blocking: Diagnostic = {
      id: "diagnostic-0002",
      severity: "error",
      stage: "fifo_tax",
      code: "incomplete_history",
      affectedReports: ["capital_gains"],
      blocking: true,
      sources: [],
    };
    const kept: Diagnostic = {
      id: "diagnostic-0003",
      severity: "error",
      stage: "fifo_tax",
      code: "incomplete_history",
      affectedReports: ["capital_gains"],
      blocking: false,
      sources: [],
    };

    expect(severityText(warning)).toBe("severityWarning");
    expect(severityText(blocking)).toBe("severityError");
    expect(severityText(kept)).toBe("severityWarning");
  });

  it("has a notice text for an info diagnostic", () => {
    const notice: Diagnostic = {
      id: "diagnostic-0004",
      severity: "info",
      stage: "parsing",
      code: "unsupported_row_type",
      affectedReports: [],
      blocking: false,
      sources: [],
    };

    expect(severityText(notice)).toBe("severityInfo");
  });

  it("has a text for each of the six stages and none for an unknown stage", () => {
    expect(stageText("parsing")).toBe("stageParsing");
    expect(stageText("merging")).toBe("stageMerging");
    expect(stageText("historical_validation")).toBe("stageHistory");
    expect(stageText("corporate_action")).toBe("stageCorporateAction");
    expect(stageText("fifo_tax")).toBe("stageFifoTax");
    expect(stageText("xml_generation")).toBe("stageXml");
    expect(stageText("something_new")).toBeUndefined();
  });

  it("builds the text from the key and its parameters", () => {
    const warning: Diagnostic = {
      id: "diagnostic-0001",
      severity: "warning",
      stage: "parsing",
      code: "inconsistent_value",
      messageKey: "inconsistent_value.datetime_date",
      messageParams: { defaultHour: 12 },
      affectedReports: [],
      blocking: false,
      sources: [],
    };

    expect(diagnosticText(warning, catalogue, "en")).toBe(
      "The calendar date in 'datetime' differs; hour 12 is used.",
    );
  });

  it("shows the unknown message text with the code when there is no key, never the server message", () => {
    const error: Diagnostic = {
      id: "diagnostic-0001",
      severity: "error",
      stage: "fifo_tax",
      code: "internal_error",
      affectedReports: [],
      blocking: true,
      sources: [],
    };

    expect(diagnosticText(error, catalogue, "en")).toBe("Unknown message: internal_error");
  });

  it("shows the unknown message text with the key when the catalogue is not loaded", () => {
    const warning: Diagnostic = {
      id: "diagnostic-0001",
      severity: "warning",
      stage: "parsing",
      code: "inconsistent_value",
      messageKey: "inconsistent_value.datetime_date",
      messageParams: { defaultHour: 12 },
      affectedReports: [],
      blocking: false,
      sources: [],
    };

    expect(diagnosticText(warning, undefined, "sl")).toBe("Neznano sporočilo: inconsistent_value.datetime_date");
  });
});

describe("linkedTexts", () => {
  const warning = (id: string, row: number): Diagnostic => ({
    id,
    severity: "warning",
    stage: "parsing",
    code: "inconsistent_value",
    messageKey: "inconsistent_value.datetime_date",
    messageParams: { defaultHour: 12 },
    affectedReports: ["capital_gains"],
    blocking: false,
    sources: [{ sourceId: "source-0001", row }],
  });

  it("shows a text that three diagnostics share once, and skips an unknown ID", () => {
    const byId = new Map([
      ["diagnostic-0001", warning("diagnostic-0001", 2)],
      ["diagnostic-0002", warning("diagnostic-0002", 3)],
      ["diagnostic-0003", warning("diagnostic-0003", 4)],
    ]);

    expect(
      linkedTexts(["diagnostic-0001", "diagnostic-0002", "diagnostic-0003", "diagnostic-0099"], byId, catalogue, "en"),
    ).toEqual(["The calendar date in 'datetime' differs; hour 12 is used."]);
  });

  it("keeps two different texts in the order of the IDs", () => {
    const other: Diagnostic = {
      id: "diagnostic-0002",
      severity: "error",
      stage: "fifo_tax",
      code: "internal_error",
      affectedReports: ["capital_gains"],
      blocking: true,
      sources: [],
    };
    const byId = new Map([
      ["diagnostic-0001", warning("diagnostic-0001", 2)],
      ["diagnostic-0002", other],
    ]);

    expect(linkedTexts(["diagnostic-0002", "diagnostic-0001"], byId, catalogue, "en")).toEqual([
      "Unknown message: internal_error",
      "The calendar date in 'datetime' differs; hour 12 is used.",
    ]);
  });
});
