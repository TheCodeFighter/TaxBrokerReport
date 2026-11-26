import { localizeNumber, roundToCents } from "../i18n/number_format";
import { uiText, uiTextWith, type Language, type UiTextKey } from "../i18n/ui_texts";
import { reportForms } from "./diagnostics_view";
import { brokerTitle } from "./result_view";
import type {
  BenefitSummary,
  CatalogueRow,
  CurrencyTotal,
  DepositInterest,
  FeeSummary,
  IncomeRow,
  IncomeSummary,
  TaxEstimate,
} from "./submit";

/** One income row as the page shows it; every field is text, `evidence` marks a proof to attach. */
export interface IncomeRowView {
  date: string;
  /** The ISIN, or the broker of cash interest; empty when the row has neither. */
  subject: string;
  payer: string;
  /** The reference row of the payer, when the payer was found. */
  payerSource?: string;
  gross: string;
  tax: string;
  treaty: string;
  evidence: boolean;
}

/** The rows of one kind (dividends, deposit interest, cash interest, coupons) under their title. */
export interface IncomeGroup {
  title: string;
  rows: IncomeRowView[];
}

const kindTexts = new Map<string, UiTextKey>([
  ["dividend", "incomeKindDividend"],
  ["deposit_interest", "incomeKindDeposit"],
  ["broker_cash_interest", "incomeKindBrokerCash"],
  ["bond_coupon", "incomeKindBondCoupon"],
]);

function sourceText(row: CatalogueRow, language: Language): string {
  return uiTextWith(language, "incomeSource", { file: row.file, row: row.row });
}

function payerText(row: IncomeRow, language: Language): string {
  const { name, country } = row.payer;

  if (name === undefined) {
    return uiText(language, "incomePayerNotFound");
  }

  return country === undefined ? name : `${name} (${country})`;
}

function treatyText(row: IncomeRow, language: Language): string {
  switch (row.treaty.claim) {
    case "none":
      return uiText(language, "incomeTreatyNone");
    case "user_text":
      return uiText(language, "incomeTreatyUser");
    case "reference_text":
      return row.treaty.catalogueRow === undefined
        ? uiText(language, "incomeTreatyReference")
        : uiTextWith(language, "incomeTreatyReferenceRow", { source: sourceText(row.treaty.catalogueRow, language) });
  }
}

function subjectText(row: IncomeRow, language: Language): string {
  if (row.isin !== undefined) {
    return row.isin;
  }

  return row.broker === undefined ? "" : brokerTitle(row.broker, language);
}

function rowView(row: IncomeRow, language: Language): IncomeRowView {
  return {
    date: row.date ?? "",
    subject: subjectText(row, language),
    payer: payerText(row, language),
    ...(row.payer.catalogueRow === undefined ? {} : { payerSource: sourceText(row.payer.catalogueRow, language) }),
    gross: localizeNumber(row.grossEur, language),
    tax: row.taxBlankInExport ? uiText(language, "incomeTaxBlank") : localizeNumber(row.foreignTaxEur, language),
    treaty: treatyText(row, language),
    evidence: row.evidenceRequired,
  };
}

/** The income rows grouped by kind, in the order the kinds first appear in the result. */
export function incomeGroups(summary: IncomeSummary, language: Language): IncomeGroup[] {
  const groups = new Map<string, IncomeGroup>();

  for (const row of summary.rows) {
    const known = kindTexts.get(row.kind);
    const group = groups.get(row.kind) ?? { title: known === undefined ? row.kind : uiText(language, known), rows: [] };

    group.rows.push(rowView(row, language));
    groups.set(row.kind, group);
  }

  return [...groups.values()];
}

/** The one line over the rows: how many, the gross income and the foreign tax in EUR. */
export function incomeSummaryLine(summary: IncomeSummary, language: Language): string {
  return uiTextWith(language, "incomeSummaryLine", {
    count: summary.rowCount,
    gross: summary.grossEur,
    tax: summary.foreignTaxEur,
  });
}

/** The reminder to add the interest of other banks; `undefined` when the form needs none. */
export function depositReminder(deposit: DepositInterest | undefined, language: Language): string | undefined {
  if (deposit === undefined || !deposit.addOtherBanks) {
    return undefined;
  }

  return uiTextWith(language, "depositReminder", { imported: deposit.importedInterestEur, limit: deposit.limitEur });
}

/** An amount of the fee and benefit block, two decimals (the text then gets the language's comma). */
function amountText(amount: string): string {
  return roundToCents(amount);
}

/** An EUR total as text, or the "not available" text: never a sum of the known part. */
function totalText(total: string | null, language: Language): string {
  return total === null
    ? uiText(language, "summaryTotalUnavailable")
    : uiTextWith(language, "summaryTotal", { total: amountText(total) });
}

function currencyLines(totals: CurrencyTotal[], language: Language): string[] {
  return totals.map((entry) =>
    entry.total === null
      ? uiTextWith(language, "summaryLineNoTotal", { currency: entry.currency, count: entry.count })
      : uiTextWith(language, "summaryLine", {
          currency: entry.currency,
          total: amountText(entry.total),
          count: entry.count,
        }),
  );
}

/** One broker of the fee or benefit block. */
export interface SummaryRowView {
  /** The broker, with the benefit type after it for a benefit. */
  title: string;
  /** One line for each currency. */
  lines: string[];
  total: string;
  /** How many fees were converted with the broker's own rate; not set when none was. */
  converted?: string;
}

/** The informational block of fees or benefits: one row for each broker and the overall total. */
export interface SummaryView {
  rows: SummaryRowView[];
  total: string;
}

const benefitTexts = new Map<string, UiTextKey>([
  ["saveback", "benefitSaveback"],
  ["stock_perk", "benefitStockPerk"],
  ["bonus", "benefitBonus"],
  ["referral", "benefitReferral"],
]);

/** The fees by broker and currency; `undefined` when there are none. A missing rate shows no sum. */
export function feeView(summary: FeeSummary | undefined, language: Language): SummaryView | undefined {
  if (summary === undefined || summary.brokers.length === 0) {
    return undefined;
  }

  return {
    rows: summary.brokers.map((broker) => ({
      title: brokerTitle(broker.broker, language),
      lines: currencyLines(broker.byCurrency, language),
      total: totalText(broker.totalEur, language),
      ...(broker.brokerRateCount === 0
        ? {}
        : { converted: uiTextWith(language, "feeBrokerRate", { count: broker.brokerRateCount }) }),
    })),
    total: totalText(summary.totalEur, language),
  };
}

/** The benefits by broker and type; `undefined` when there are none. */
export function benefitView(summary: BenefitSummary | undefined, language: Language): SummaryView | undefined {
  if (summary === undefined || summary.brokers.length === 0) {
    return undefined;
  }

  return {
    rows: summary.brokers.map((row) => {
      const type = benefitTexts.get(row.type);

      return {
        title: `${brokerTitle(row.broker, language)}: ${type === undefined ? row.type : uiText(language, type)}`,
        lines: currencyLines(row.byCurrency, language),
        total: totalText(row.totalEur, language),
      };
    }),
    total: totalText(summary.totalEur, language),
  };
}

/** One form of the tax estimate: its form code, the amount or the reason it has none, and a note. */
export interface TaxEstimateRowView {
  form: string;
  text: string;
  note?: string;
}

/** The estimate block: a message instead of rows for a year without rates, else the rows and the total. */
export interface TaxEstimateView {
  unsupported: boolean;
  rows: TaxEstimateRowView[];
  total: string;
}

function formCode(report: string): string {
  return reportForms.find((form) => form.key === report)?.form ?? report;
}

/**
 * The approximate tax to pay; `undefined` when no form has data. A form that is not complete has no
 * amount and makes the total a lower limit. Amounts are the server's cents, never recalculated.
 */
export function taxEstimateView(
  estimate: TaxEstimate | undefined,
  addOtherBanks: boolean,
  language: Language,
): TaxEstimateView | undefined {
  if (estimate === undefined || (estimate.yearSupported && estimate.forms.length === 0)) {
    return undefined;
  }

  const rows: TaxEstimateRowView[] = estimate.forms.map((form) => {
    const notes: string[] = [];

    if (form.notAssessed) {
      notes.push(uiText(language, "taxNotAssessed"));
    }

    if (form.report === "bankInterest" && addOtherBanks && !form.incomplete) {
      notes.push(uiText(language, "taxDepositNote"));
    }

    const note = notes.length === 0 ? undefined : notes.join(" ");

    return {
      form: formCode(form.report),
      text:
        form.taxEur === null || form.incomplete
          ? uiText(language, "taxIncomplete")
          : uiTextWith(language, "taxAmount", { amount: roundToCents(form.taxEur) }),
      ...(note === undefined ? {} : { note }),
    };
  });
  const amount = roundToCents(estimate.totalEur);
  const missing = estimate.forms.filter((form) => form.incomplete).map((form) => formCode(form.report));

  return {
    unsupported: !estimate.yearSupported,
    rows,
    total: estimate.totalPartial && missing.length === estimate.forms.length
      ? uiText(language, "taxTotalNone")
      : estimate.totalPartial
      ? uiTextWith(language, "taxTotalPartial", { amount, forms: missing.join(", ") })
      : uiTextWith(language, "taxTotal", { amount }),
  };
}
