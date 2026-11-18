import type { Language, UiTextKey } from "../i18n/ui_texts";
import { reportForms } from "./diagnostics_view";
import { brokerKinds } from "./input_kinds";
import { hasQuestionWithoutScreen } from "./questions";
import type { ReportInvestment, ReportResult, RunResult } from "./submit";

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

export function overallStatusText(status: string): UiTextKey {
  return overallTexts.get(status) ?? "statusFailed";
}

export interface ReportRow {
  key: string;
  form: string;
  statusText: UiTextKey;
}

/** One row for each form the result has, in the fixed order of the forms. */
export function reportRows(result: RunResult): ReportRow[] {
  return reportForms.flatMap(({ key, form }) => {
    const report = result.reports[key];

    if (report === undefined) {
      return [];
    }

    return [{ key, form, statusText: reportTexts.get(report.status) ?? "reportFailed" }];
  });
}

/** True while an open question has no screen yet (classification, income and the like). */
export function needsAnswerHint(result: RunResult): boolean {
  return hasQuestionWithoutScreen(result);
}

/** The number of errors and warnings in the diagnostics of the result. */
export function countDiagnostics(result: RunResult): { errors: number; warnings: number } {
  return {
    errors: result.diagnostics.filter((diagnostic) => diagnostic.severity === "error").length,
    warnings: result.diagnostics.filter((diagnostic) => diagnostic.severity === "warning").length,
  };
}

/** The tone of a status: a class and a text label beside it, never colour alone. */
export type Tone = "ok" | "neutral" | "warn" | "pending" | "fail";

const overallTones = new Map<string, Tone>([
  ["success", "ok"],
  ["completed_with_warnings", "ok"],
  ["partial_success", "warn"],
  ["needs_input", "pending"],
  ["failed", "fail"],
]);

/** The tone of the overall status, so a partial success looks different from a failure. */
export function overallTone(status: string): Tone {
  return overallTones.get(status) ?? "fail";
}

const reportTones = new Map<string, Tone>([
  ["generated", "ok"],
  // No data is not a result to celebrate or to worry about: it stays in the plain text colour.
  ["no_data", "neutral"],
  ["needs_input", "pending"],
  ["failed", "fail"],
]);

const noDataTexts = new Map<string, UiTextKey>([
  ["below_deposit_interest_threshold", "reportNoDataBelowThreshold"],
  ["no_selected_year_disposals", "reportNoDataNoSales"],
]);

const resolutionTexts = new Map<string, UiTextKey>([
  ["exclude_failed_instruments", "resolutionExclude"],
  ["fail_report", "resolutionFail"],
  ["developer_export", "resolutionDeveloper"],
]);

/** Everything one form card shows besides its diagnostics. */
export interface ReportCard extends ReportRow {
  tone: Tone;
  /** Why a `no_data` form has no data; only set for `no_data`. */
  noDataText?: UiTextKey;
  /** The number of open questions of the form. */
  questions: number;
  /** The user's failure choice, when one was made. */
  resolutionText?: UiTextKey;
  excluded: ReportInvestment[];
  /** The `broker` IDs of the brokers left out (Doh-Obr cash interest); `brokerTitle` names them. */
  excludedBrokers: string[];
  unsafe: boolean;
  unsafeInvestments: ReportInvestment[];
  /** The income rows whose foreign tax needs proof attached in eDavki. */
  evidenceRows: number;
  downloadAvailable: boolean;
}

function noDataText(report: ReportResult): UiTextKey | undefined {
  if (report.status !== "no_data") {
    return undefined;
  }

  return (report.reason === undefined ? undefined : noDataTexts.get(report.reason)) ?? "reportNoDataEmpty";
}

/** One card for each form the result has, in the fixed order of the forms. */
export function reportCards(result: RunResult): ReportCard[] {
  return reportRows(result).map((row) => {
    const report = result.reports[row.key] as ReportResult;
    const noData = noDataText(report);
    const resolution =
      report.failureResolution === undefined ? undefined : resolutionTexts.get(report.failureResolution);
    const evidenceRows = (report.incomeSummary?.rows ?? []).filter((incomeRow) => incomeRow.evidenceRequired).length;

    return {
      ...row,
      tone: reportTones.get(report.status) ?? "fail",
      ...(noData === undefined ? {} : { noDataText: noData }),
      questions: report.requestIds.length,
      ...(resolution === undefined ? {} : { resolutionText: resolution }),
      excluded: report.excludedInvestments ?? [],
      excludedBrokers: (report.excludedBrokers ?? []).map((broker) => broker.broker),
      unsafe: report.unsafe,
      unsafeInvestments: report.unsafeInvestments ?? [],
      evidenceRows,
      downloadAvailable: report.downloadAvailable === true,
    };
  });
}

/** The name of a broker from the input table; an ID the table does not have is shown as it is. */
export function brokerTitle(broker: string, language: Language): string {
  return brokerKinds.find((kind) => kind.broker === broker)?.title[language] ?? broker;
}

const classificationTexts = new Map<string, UiTextKey>([
  ["debt_security", "excludedReasonDebtSecurity"],
  ["crypto_not_reported", "excludedReasonCrypto"],
  ["slovenian_tax_settled", "excludedReasonSlovenianTax"],
  ["user_choice", "excludedReasonUserChoice"],
]);

/** One instrument left out by classification; separate from the exclusions of one form. */
export interface ClassificationRow {
  name: string;
  isin?: string;
  /** Not set for a reason the page does not know; the name and ISIN are still shown. */
  reasonText?: UiTextKey;
}

/** The classification exclusions (`excludedItems`) in result order. */
export function classificationExclusions(result: RunResult): ClassificationRow[] {
  return result.excludedItems.map((item) => {
    const reasonText = classificationTexts.get(item.reason);

    return {
      name: item.name,
      ...(item.isin === undefined ? {} : { isin: item.isin }),
      ...(reasonText === undefined ? {} : { reasonText }),
    };
  });
}
