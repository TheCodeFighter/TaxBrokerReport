import type { UiTextKey } from "../i18n/ui_texts";
import type { AnswerValue, IdentityCheck, IdentityRow } from "./questions";

/** The most days between two rows of one action; the server refuses an answer with more. */
export const SAME_ACTION_WINDOW_DAYS = 10;

const rowTypeTexts = new Map<string, UiTextKey>([
  ["split", "actionSplit"],
  ["reverse_split", "actionReverseSplit"],
  ["unresolved_split", "actionUnresolvedSplit"],
  ["merger", "actionMerger"],
  ["isin_change", "actionIsinChange"],
  ["stock_merger", "actionStockMerger"],
]);

/** The text of a row type; `undefined` for a type the page does not know (it shows the word). */
export function rowTypeText(type: string): UiTextKey | undefined {
  return rowTypeTexts.get(type);
}

/** What is wrong with the chosen action numbers; the checks run in this order. */
export type GroupingProblem = "incomplete" | "gap" | "window";

/** One action number for each row, in the order of the rows; `undefined` while it is not chosen. */
export type Choices = (number | undefined)[];

const MILLISECONDS_PER_DAY = 24 * 60 * 60 * 1000;

/** The numbers of "all rows are one action": 1 for every row. */
export function allOneAction(rowCount: number): number[] {
  return Array.from({ length: rowCount }, () => 1);
}

/** The numbers of "each row its own action": 1, 2, 3, … in the order of the rows. */
export function eachOwnAction(rowCount: number): number[] {
  return Array.from({ length: rowCount }, (_, index) => index + 1);
}

/** The whole days from one `YYYY-MM-DD` date to another, counted in UTC (no local time zone). */
function daysBetween(from: string, to: string): number {
  return Math.round((Date.parse(`${to}T00:00:00Z`) - Date.parse(`${from}T00:00:00Z`)) / MILLISECONDS_PER_DAY);
}

/**
 * The first problem of the chosen numbers, or `undefined` when the server takes them: a number for
 * every row, the numbers used run from 1 without a gap, and rows of one action are at most
 * `SAME_ACTION_WINDOW_DAYS` apart.
 */
export function groupingProblem(check: IdentityCheck, choices: Choices): GroupingProblem | undefined {
  if (choices.length !== check.rows.length || choices.some((choice) => choice === undefined)) {
    return "incomplete";
  }

  const distinct = new Set(choices);

  for (let expected = 1; expected <= distinct.size; expected += 1) {
    if (!distinct.has(expected)) {
      return "gap";
    }
  }

  const tooFarApart = check.rows.some((row, first) =>
    check.rows.some(
      (other, second) =>
        second > first &&
        choices[first] === choices[second] &&
        Math.abs(daysBetween(row.date, other.date)) > SAME_ACTION_WINDOW_DAYS,
    ),
  );

  return tooFarApart ? "window" : undefined;
}

/** True when two different actions start on one date: the report cannot tell their order. */
export function sameDayStarts(check: IdentityCheck, choices: Choices): boolean {
  const starts = new Map<number, string>();

  check.rows.forEach((row, index) => {
    const choice = choices[index];
    const start = choice === undefined ? undefined : starts.get(choice);

    if (choice !== undefined && (start === undefined || row.date < start)) {
      starts.set(choice, row.date);
    }
  });

  const dates = [...starts.values()];

  return new Set(dates).size < dates.length;
}

/** The answer of complete choices; `undefined` while a row has no number. */
export function groupingAnswer(choices: Choices): AnswerValue | undefined {
  if (choices.some((choice) => choice === undefined)) {
    return undefined;
  }

  return { type: "grouping", actionOfRow: choices };
}

/** The rows of each action, in the order of the action numbers 1, 2, 3, …; rows keep their order. */
export function rowsByAction(check: IdentityCheck, choices: Choices): IdentityRow[][] {
  const actions: IdentityRow[][] = [];

  check.rows.forEach((row, index) => {
    const choice = choices[index];

    if (choice !== undefined) {
      actions[choice - 1] = [...(actions[choice - 1] ?? []), row];
    }
  });

  return actions.filter((rows) => rows !== undefined);
}
