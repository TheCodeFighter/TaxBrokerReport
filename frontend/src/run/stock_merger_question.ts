import type { AnswerValue, StockMergerCheck } from "./questions";

/** How the merger is taxed: a sale at the market value of the new shares, or a deferral. */
export type MergerTreatment = "taxable" | "deferred";

/** The fields of the stock merger form. */
export interface StockMergerForm {
  /** `undefined` until the user chooses: nothing is preselected. */
  treatment: MergerTreatment | undefined;
  /** The market value of all new units, as typed; only a taxable answer uses it. */
  value: string;
  /** `YYYY-MM-DD`. */
  date: string;
}

/** The form a question starts with: no choice, the value the rows give (if any) and the first date. */
export function initialStockMergerForm(check: StockMergerCheck): StockMergerForm {
  return {
    treatment: undefined,
    value: check.suggestedValue ?? "",
    date: check.effectiveDate,
  };
}

/**
 * An amount above 0 with up to 4 decimals and 14 whole digits (always within the backend's amount),
 * as the backend takes it (a point, no grouping): `520,5` is `520.5`. `undefined` for an empty,
 * signed, longer or zero amount.
 */
export function parseMarketValue(text: string): string | undefined {
  const match = /^(\d{1,14})(?:[.,](\d{1,4}))?$/.exec(text.trim());

  if (match === null) {
    return undefined;
  }

  const decimals = match[2] ?? "";

  if (/^0*$/.test(match[1] as string) && /^0*$/.test(decimals)) {
    return undefined;
  }

  return decimals === "" ? (match[1] as string) : `${match[1]}.${decimals}`;
}

/** What is wrong with the entered fields; only a taxable answer has a value to check. */
export interface StockMergerProblems {
  value: boolean;
  date: boolean;
}

/** True when the text is a date `YYYY-MM-DD` between the first and the last row of the action. */
function isDateOfRows(check: StockMergerCheck, date: string): boolean {
  return /^\d{4}-\d{2}-\d{2}$/.test(date) && check.earliestDate <= date && date <= check.latestDate;
}

/** The problems of the entered fields; a field that is empty has none yet (the button waits). */
export function stockMergerProblems(check: StockMergerCheck, form: StockMergerForm): StockMergerProblems {
  if (form.treatment === undefined) {
    return { value: false, date: false };
  }

  return {
    value: form.treatment === "taxable" && form.value.trim() !== "" && parseMarketValue(form.value) === undefined,
    date: form.date.trim() !== "" && !isDateOfRows(check, form.date),
  };
}

/**
 * The answer of the form, or `undefined` while it is not complete: a market value above 0 (taxable
 * only) and a date of the rows.
 */
export function stockMergerAnswer(check: StockMergerCheck, form: StockMergerForm): AnswerValue | undefined {
  if (form.treatment === undefined || !isDateOfRows(check, form.date)) {
    return undefined;
  }

  if (form.treatment === "deferred") {
    return { treatment: "deferred", effectiveDate: form.date };
  }

  const marketValue = parseMarketValue(form.value);

  return marketValue === undefined ? undefined : { treatment: "taxable", marketValue, effectiveDate: form.date };
}
