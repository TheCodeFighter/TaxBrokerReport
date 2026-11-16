import type { UiTextKey } from "../i18n/ui_texts";
import type { RunResult } from "./submit";

/** What a shown result belongs to: the files as they were sent, with their display names. */
export interface ShownResult {
  result: RunResult;
  fileLabels: string[];
  /** True once the inputs changed after this result. */
  outOfDate: boolean;
}

/** The shown result marked as out of date; nothing stays nothing. The result itself is kept. */
export function markOutOfDate(shown: ShownResult | undefined): ShownResult | undefined {
  return shown === undefined ? undefined : { ...shown, outOfDate: true };
}

const overallTexts = new Map<string, UiTextKey>([
  ["success", "statusSuccess"],
  ["completed_with_warnings", "statusCompletedWithWarnings"],
  ["partial_success", "statusPartialSuccess"],
  ["needs_input", "statusNeedsInput"],
  ["failed", "statusFailed"],
]);

const reportTexts = new Map<string, UiTextKey>([
  ["generated", "reportGenerated"],
  ["no_data", "reportNoData"],
  ["needs_input", "reportNeedsInput"],
  ["failed", "reportFailed"],
]);

/** The form of each report key of the result JSON; the form codes are names, not translated. */
const forms: [string, string][] = [
  ["capitalGains", "Doh-KDVP"],
  ["dividends", "Doh-Div"],
  ["bankInterest", "Doh-DHO"],
  ["otherInterest", "Doh-Obr"],
];

export function overallStatusText(status: string): UiTextKey {
  return overallTexts.get(status) ?? "statusFailed";
}

export interface ReportRow {
  form: string;
  statusText: UiTextKey;
}

/** One row for each form the result has, in the fixed order of the forms. */
export function reportRows(result: RunResult): ReportRow[] {
  return forms.flatMap(([key, form]) => {
    const report = result.reports[key];

    if (report === undefined) {
      return [];
    }

    return [{ form, statusText: reportTexts.get(report.status) ?? "reportFailed" }];
  });
}

/** True while the result waits for an answer that the page cannot take yet (until the question screens). */
export function needsAnswerHint(result: RunResult): boolean {
  return result.status === "needs_input";
}

/** The number of errors and warnings in the diagnostics of the result. */
export function countDiagnostics(result: RunResult): { errors: number; warnings: number } {
  return {
    errors: result.diagnostics.filter((diagnostic) => diagnostic.severity === "error").length,
    warnings: result.diagnostics.filter((diagnostic) => diagnostic.severity === "warning").length,
  };
}
