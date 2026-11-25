import { parseWholeNumber } from "./units";
import type { AnswerValue, IsinChangeCheck } from "./questions";

/** A whole number above 0, or `undefined`. */
function positiveWhole(text: string): number | undefined {
  const number = parseWholeNumber(text);

  return number === undefined || number === 0 ? undefined : number;
}

/** The two answers of the first choice of an ISIN change question. */
export type SameIssuer = "yes" | "no";

/** The fields of the ISIN change form. */
export interface IsinChangeForm {
  /** `undefined` until the user chooses: nothing is preselected. */
  sameIssuer: SameIssuer | undefined;
  /** New units for the old ones: `1` and `1` when the number of units does not change. */
  newShares: string;
  oldShares: string;
  /** `YYYY-MM-DD`. */
  date: string;
}

/**
 * The form a question starts with: no choice, the ratio of the rows (1 : 1 when they give none),
 * and the date of the row of the old ISIN.
 */
export function initialIsinChangeForm(check: IsinChangeCheck): IsinChangeForm {
  const ratio = check.suggestedRatio ?? { newShares: 1, oldShares: 1 };

  return {
    sameIssuer: undefined,
    newShares: String(ratio.newShares),
    oldShares: String(ratio.oldShares),
    date: check.effectiveDate,
  };
}

/** What is wrong with the entered ratio and date; only a "yes" answer has any of it. */
export interface IsinChangeProblems {
  numbers: boolean;
  date: boolean;
}

/** True when the text is a date `YYYY-MM-DD` between the first and the last row of the action. */
function isDateOfRows(check: IsinChangeCheck, date: string): boolean {
  return /^\d{4}-\d{2}-\d{2}$/.test(date) && check.earliestDate <= date && date <= check.latestDate;
}

/** The problems of the entered fields; a field that is empty has none yet (the button waits). */
export function isinChangeProblems(check: IsinChangeCheck, form: IsinChangeForm): IsinChangeProblems {
  if (form.sameIssuer !== "yes") {
    return { numbers: false, date: false };
  }

  const filled = form.newShares.trim() !== "" && form.oldShares.trim() !== "";
  const valid = positiveWhole(form.newShares) !== undefined && positiveWhole(form.oldShares) !== undefined;

  return { numbers: filled && !valid, date: form.date.trim() !== "" && !isDateOfRows(check, form.date) };
}

/**
 * The answer of the form, or `undefined` while it is not complete: `different_issuer` for "no", or
 * the two whole numbers above 0 and a date of the rows for "yes".
 */
export function isinChangeAnswer(check: IsinChangeCheck, form: IsinChangeForm): AnswerValue | undefined {
  if (form.sameIssuer === "no") {
    return "different_issuer";
  }

  if (form.sameIssuer !== "yes") {
    return undefined;
  }

  const newShares = positiveWhole(form.newShares);
  const oldShares = positiveWhole(form.oldShares);

  if (newShares === undefined || oldShares === undefined || !isDateOfRows(check, form.date)) {
    return undefined;
  }

  return { newShares, oldShares, effectiveDate: form.date };
}
