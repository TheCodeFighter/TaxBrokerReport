import { localizeNumber } from "../i18n/number_format";
import { uiText, uiTextWith, type Language, type UiTextKey } from "../i18n/ui_texts";
import type { CapitalSummary } from "./submit";

/** One investment of the Doh-KDVP summary table. */
export interface CapitalRowView {
  investment: string;
  state: string;
  /** The sales of the investment that are in the form. */
  sales: number;
  units: string;
  /** The listed units whose loss is blocked; a dash when none. */
  blockedUnits: string;
  /** The distinct sale dates, in ledger order. */
  dates: string;
}

/** The informational Doh-KDVP summary: a line, one row for each investment and the long-held units. */
export interface CapitalView {
  summaryLine: string;
  rows: CapitalRowView[];
  longHeld: string[];
}

const stateTexts = new Map<string, UiTextKey>([
  ["listed", "capitalStateListed"],
  ["blocked", "capitalStateBlocked"],
  ["excluded", "capitalStateExcluded"],
  ["long_held_only", "capitalStateLongHeld"],
]);

/** True for a units text that holds no unit, for example `0.00000000`. */
function noUnits(units: string): boolean {
  return /^0(\.0+)?$/.test(units);
}

/** The summary of the sales of the year; `undefined` when the year has no investment with a sale. */
export function capitalView(summary: CapitalSummary | undefined, language: Language): CapitalView | undefined {
  if (summary === undefined || (summary.investments.length === 0 && summary.longHeld.length === 0)) {
    return undefined;
  }

  return {
    summaryLine: uiTextWith(language, "capitalSummaryLine", {
      count: summary.investments.length,
      listed: summary.investments.filter((investment) => investment.state === "listed").length,
    }),
    rows: summary.investments.map((investment) => {
      const state = stateTexts.get(investment.state);

      return {
        investment: `${investment.name} (${investment.isin})`,
        state: state === undefined ? investment.state : uiText(language, state),
        sales: investment.listedSales,
        units: localizeNumber(investment.listedUnits, language),
        blockedUnits: noUnits(investment.blockedLossUnits) ? "–" : localizeNumber(investment.blockedLossUnits, language),
        dates: [...new Set(investment.sales.map((sale) => sale.date))].join(", "),
      };
    }),
    longHeld: summary.longHeld.map((sale) =>
      uiTextWith(language, "capitalLongHeldLine", {
        name: sale.name,
        isin: sale.isin,
        date: sale.saleDate,
        units: sale.units,
      }),
    ),
  };
}
