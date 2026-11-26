import type { MessageParams } from "../i18n/messages";
import type { CatalogueRow, DiagnosticSource, RunResult } from "./submit";
import {
  formatUnits,
  parseUnits,
  parseWholeNumber,
  previewUnits,
  ratioFromShareCounts,
  ratioLooksUnusual,
  reduceRatio,
  type Fraction,
  type Ratio,
} from "./units";

/** A broker transaction: its broker ID and transaction ID. */
export interface TransactionRef {
  broker: string;
  transactionId: string;
}

/** One sale of a loss question with the facts of its 30-day window. */
export interface LossSale {
  saleDate: string;
  transaction?: TransactionRef;
  /** Present when the sale was made under another ISIN, before an ISIN change. */
  soldAs?: { isin: string; name: string };
  saleUnits: string;
  lossUnits: string;
  windowStart: string;
  windowEnd: string;
  knownReplacements: { date: string; units: string }[];
  knownIneligibleUnits: string;
  openUnits: string;
}

export interface LossCheck {
  name: string;
  isin: string;
  sales: LossSale[];
}

/** One broker record of a corporate action; the broker's own quantity is shown read-only. */
export interface RatioSource {
  broker: string;
  date: string;
  transactionId?: string;
  sourceValue?: string;
}

/** The dates an answer may give, with the position held just before and the lowest change after. */
export interface DateRange {
  earliestDate: string;
  latestDate: string;
  positionBefore: string;
  lowestChangeAfter: string;
}

export interface RatioCheck {
  name: string;
  isin: string;
  effectiveDate: string;
  sources: RatioSource[];
  /** The position on the row date; left out for a question before a sale with too few units. */
  positionBefore?: string;
  earlierSaleDate?: string;
  dateRanges: DateRange[];
  askSameEvent: boolean;
}

/** One action row of an identity question; the broker's own quantity is shown read-only. */
export interface IdentityRow {
  broker: string;
  date: string;
  /** `split`, `reverse_split`, `unresolved_split`, `merger`, `isin_change` or `stock_merger`. */
  type: string;
  transactionId?: string;
  sourceValue?: string;
  brokerActionId?: string;
}

/** Action rows of one ISIN that may be one action or several, sorted by date, broker and ID. */
export interface IdentityCheck {
  name: string;
  isin: string;
  rows: IdentityRow[];
}

/** The rows of an ISIN change: the old ISIN lost units and the new one got them. */
export interface IsinChangeCheck {
  oldIsin: string;
  oldName: string;
  newIsin: string;
  newName: string;
  /** The date of the row of the old ISIN, shown pre-filled. */
  effectiveDate: string;
  /** The answer's date is between these two dates, both included. */
  earliestDate: string;
  latestDate: string;
  /** Every row of both ISINs; the value is negative for the units that left. */
  sources: RatioSource[];
  /** The units that came in over the units that left, reduced; the only ratio the server accepts. */
  suggestedRatio?: { newShares: number; oldShares: number };
}

/** The rows of a merger of two companies: the old ISIN lost units and the new one got them. */
export interface StockMergerCheck {
  oldIsin: string;
  oldName: string;
  newIsin: string;
  newName: string;
  /** All units that left and all that came in, with 8 decimals. */
  oldUnits: string;
  newUnits: string;
  /** The currency of the cash and of the market value, a code such as `USD`. */
  currency: string;
  /** The cash paid with the exchange, 4 decimals; `0.0000` when there is none. */
  cash: string;
  /** The market value of all new units as the rows give it; shown pre-filled, never used unasked. */
  suggestedValue?: string;
  /** The date of the first row, shown pre-filled. */
  effectiveDate: string;
  /** The answer's date is between these two dates, both included. */
  earliestDate: string;
  latestDate: string;
  /** Every row of both ISINs; the value is negative for the units that left. */
  sources: RatioSource[];
}

export interface QuantityCheck {
  name: string;
  isin: string;
  saleDate: string;
  transaction?: TransactionRef;
  originalUnits: string;
  proposedUnits: string;
  unitValueEur: Fraction;
  budgetUsedEur: Fraction;
}

/** A trade the export does not classify: the class labels as the file wrote them. */
export interface ClassificationCheck {
  name: string;
  isin: string;
  classLabels: string[];
}

/** An income payment of an unknown type, as the file gave it. */
export interface IncomeCheck {
  /** `dividend` or `interest`. */
  eventKind: string;
  receiptDate: string;
  name: string;
  isin?: string;
  transaction?: TransactionRef;
  classLabels: string[];
  /** Gross amount in the payment currency, four decimals. */
  grossAmount: string;
  currency: string;
}

/** One reference row that could be the payer of an ISIN. */
export interface PayerOption {
  name: string;
  address: string;
  country: string;
  taxNumber?: string;
  source: CatalogueRow;
}

/** Several reference rows disagree on the payer of an ISIN. */
export interface PayerCheck {
  name: string;
  isin: string;
  options: PayerOption[];
}

/** One country a payer country question offers, with where it comes from. */
export interface CountryCandidate {
  country: string;
  /** `export`, `reference` or `export_and_reference`. */
  origin: string;
}

/** The sources name different countries for the payer of an ISIN. */
export interface CountryCheck {
  name: string;
  isin: string;
  candidates: CountryCandidate[];
}

/** A bond with a sale or a maturity in the year, whose kind decides the income. */
export interface BondCheck {
  isin: string;
  name: string;
  soldUnits: string;
  maturedUnits: string;
  maturityDate?: string;
}

/** A country whose taxed dividends can claim a treaty relief. */
export interface TreatyCheck {
  year: number;
  country: string;
  isins: string[];
}

/** The imported deposit interest and the allowance it is compared with. */
export interface DepositCheck {
  year: number;
  importedInterestEur: string;
  limitEur: string;
}

/** One investment a failure question lists; a recorded answer has no diagnostic IDs. */
export interface FailureInvestment {
  name: string;
  isin: string;
  reasonCodes: string[];
  diagnosticIds?: string[];
}

/** One broker a failure question lists (Doh-Obr cash interest). */
export interface FailureBroker {
  broker: string;
  reasonCodes: string[];
  diagnosticIds?: string[];
}

/** An open question. Only the parts the screens read are typed. */
export interface DecisionRequest {
  id: string;
  type: string;
  runRevision: number;
  affectedReports: string[];
  sources: DiagnosticSource[];
  taxDate?: string;
  isin?: string;
  /** The step (1 to 3) of a developer export confirmation. */
  step?: number;
  /** The choices the question offers (classification, income, bond); the page never adds one. */
  options?: string[];
  lossCheck?: LossCheck;
  ratioCheck?: RatioCheck;
  identityCheck?: IdentityCheck;
  isinChangeCheck?: IsinChangeCheck;
  stockMergerCheck?: StockMergerCheck;
  quantityCheck?: QuantityCheck;
  classificationCheck?: ClassificationCheck;
  incomeCheck?: IncomeCheck;
  payerCheck?: PayerCheck;
  countryCheck?: CountryCheck;
  bondCheck?: BondCheck;
  treatyCheck?: TreatyCheck;
  depositCheck?: DepositCheck;
  /** The kinds of `other_interest` the question offers. */
  otherInterestSubtypes?: string[];
  /** The investments and brokers of a failure question, sorted by the server. */
  investments?: FailureInvestment[];
  brokers?: FailureBroker[];
}

/** A recorded answer with the question data it belongs to. */
export interface Decision {
  id: string;
  type: string;
  fromEarlierRun: boolean;
  used: boolean;
  lossCheck?: LossCheck;
  ratioCheck?: RatioCheck;
  identityCheck?: IdentityCheck;
  isinChangeCheck?: IsinChangeCheck;
  stockMergerCheck?: StockMergerCheck;
  quantityCheck?: QuantityCheck;
  classificationCheck?: ClassificationCheck;
  incomeCheck?: IncomeCheck;
  payerCheck?: PayerCheck;
  countryCheck?: CountryCheck;
  bondCheck?: BondCheck;
  treatyCheck?: TreatyCheck;
  depositCheck?: DepositCheck;
  investments?: FailureInvestment[];
  brokers?: FailureBroker[];
  /** A word (`other_activity`, `cancel`) or an object (a ratio, a corrected quantity). */
  answer: string | Record<string, unknown>;
}

/** A sale quantity that was corrected, by the rounding evidence or by the user's answer. */
export interface Correction {
  isin: string;
  saleDate: string;
  source: DiagnosticSource;
  originalUnits: string;
  correctedUnits: string;
  kind: string;
  impactEur: Fraction;
}

/** The cumulative EUR impact of the corrections of one ISIN against its limit. */
export interface Reconciliation {
  isin: string;
  impactEur: Fraction;
  limitEur: string;
}

/** The kinds of question that have a screen. */
export type QuestionKind =
  | "loss"
  | "ratio"
  | "identity"
  | "isinChange"
  | "stockMerger"
  | "quantity"
  | "classification"
  | "income"
  | "payer"
  | "country"
  | "bond"
  | "treaty"
  | "deposit"
  | "failure"
  | "developer";

const questionKinds = new Map<string, QuestionKind>([
  ["loss_eligibility_confirmation_required", "loss"],
  ["corporate_action_identity_required", "identity"],
  ["corporate_action_isin_change_required", "isinChange"],
  ["corporate_action_stock_merger_required", "stockMerger"],
  ["corporate_action_ratio_required", "ratio"],
  ["quantity_correction_required", "quantity"],
  ["deposit_interest_total_required", "deposit"],
  ["capital_gains_failure_resolution_required", "failure"],
  ["dividends_failure_resolution_required", "failure"],
  ["other_interest_failure_resolution_required", "failure"],
  ["developer_export_confirmation_required", "developer"],
]);

/** What tells the kind of a question: its request type and, for the shared types, its data. */
interface KindSource {
  type: string;
  classificationCheck?: ClassificationCheck;
  incomeCheck?: IncomeCheck;
  payerCheck?: PayerCheck;
  countryCheck?: CountryCheck;
  bondCheck?: BondCheck;
  treatyCheck?: TreatyCheck;
}

/**
 * The kind of a request or a recorded answer, or `undefined` for a type that has no screen. The
 * types `tax_classification_required` and `income_details_required` each hold two or three kinds;
 * the data key tells them apart, never the type alone.
 */
export function questionKind(source: KindSource): QuestionKind | undefined {
  if (source.type === "tax_classification_required") {
    if (source.incomeCheck !== undefined) {
      return "income";
    }

    return source.classificationCheck === undefined ? undefined : "classification";
  }

  if (source.type === "income_details_required") {
    if (source.payerCheck !== undefined) {
      return "payer";
    }

    if (source.countryCheck !== undefined) {
      return "country";
    }

    if (source.bondCheck !== undefined) {
      return "bond";
    }

    return source.treatyCheck === undefined ? undefined : "treaty";
  }

  return questionKinds.get(source.type);
}

/** The open questions that have a screen, in result order. */
export function supportedQuestions(result: RunResult): DecisionRequest[] {
  return result.requests.filter((request) => questionKind(request) !== undefined);
}

/** The open questions with a screen, placed in the first form that lists them; `rest` has no form. */
export function placeQuestions(
  result: RunResult,
  reportKeys: string[],
): { byReport: Map<string, DecisionRequest[]>; rest: DecisionRequest[] } {
  const byReport = new Map<string, DecisionRequest[]>();
  const rest: DecisionRequest[] = [];

  for (const request of supportedQuestions(result)) {
    const owner = reportKeys.find((key) => result.reports[key]?.requestIds.includes(request.id));

    if (owner === undefined) {
      rest.push(request);
    } else {
      byReport.set(owner, [...(byReport.get(owner) ?? []), request]);
    }
  }

  return { byReport, rest };
}

/**
 * The ID of the first open question with a screen, as the page shows them: the forms in their
 * order (`reportKeys`), in each form from the top down, then the questions that no form lists.
 * `undefined` when there is none (no link). Open questions are the unanswered ones: an answered
 * question is no longer in the result.
 */
export function firstQuestionId(result: RunResult, reportKeys: string[]): string | undefined {
  const placed = placeQuestions(result, reportKeys);

  for (const key of reportKeys) {
    const first = placed.byReport.get(key)?.[0];

    if (first !== undefined) {
      return first.id;
    }
  }

  return placed.rest[0]?.id;
}

/** The page anchor of a question; the link scrolls to it and moves the focus to its heading. */
export function questionAnchor(requestId: string): string {
  return `question-${requestId}`;
}

/** True when an open question has no screen yet, so the page still says it cannot take it. */
export function hasQuestionWithoutScreen(result: RunResult): boolean {
  return result.requests.some((request) => questionKind(request) === undefined);
}

/** The loss choices; the page preselects none. */
export type LossChoice = "no_other_activity" | "other_activity";

/** What one answer holds: a word or an object, as the server's answer JSON. */
export type AnswerValue = string | Record<string, unknown>;

/** The answer that closes a question without answering it. */
export const CANCEL_ANSWER = "cancel";

/** The answer for the same-event choice of a ratio question: they are two separate splits. */
export const SEPARATE_EVENTS_ANSWER = "separate_events";

/** The corrected quantity as the answer of a quantity question; empty when the text is not one. */
export function quantityAnswer(input: string): AnswerValue | undefined {
  const ticks = parseUnits(input);

  return ticks === undefined ? undefined : { correctedUnits: formatUnits(ticks) };
}

/** The text fields of the ratio form. */
export interface RatioForm {
  mode: "ratio" | "shares";
  newShares: string;
  oldShares: string;
  /** The shares held before the action, as the broker shows them (mode `shares`). */
  before: string;
  after: string;
  /** `YYYY-MM-DD`. */
  date: string;
}

/** The ratio the entered numbers give, reduced; `undefined` when they are not a valid ratio. */
export function enteredRatio(form: RatioForm): Ratio | undefined {
  if (form.mode === "shares") {
    return ratioFromShareCounts(form.before, form.after);
  }

  const newer = parseWholeNumber(form.newShares);
  const older = parseWholeNumber(form.oldShares);

  return newer === undefined || older === undefined ? undefined : reduceRatio(newer, older);
}

/** The range of effective dates that holds the date; `undefined` for an invalid or outside date. */
export function rangeOf(check: RatioCheck, date: string): DateRange | undefined {
  if (!/^\d{4}-\d{2}-\d{2}$/.test(date)) {
    return undefined;
  }

  return check.dateRanges.find((range) => range.earliestDate <= date && date <= range.latestDate);
}

/** The date the form starts with: the row date of the broker when it is in a range, else empty. */
export function defaultEffectiveDate(check: RatioCheck): string {
  return rangeOf(check, check.effectiveDate) === undefined ? "" : check.effectiveDate;
}

/** What the entered ratio does: the reduced numbers, the position before and after. */
export interface RatioPreview {
  ratio: Ratio;
  date: string;
  /** The position just before the action on the entered date. */
  position: string;
  /** The position after the action; the user confirms it, and it is sent as `confirmedUnits`. */
  units: string;
  /** True when a reduced number is above 1000: shown as a warning, still accepted. */
  unusual: boolean;
}

/** The preview of the entered ratio and date; `undefined` while one of them is not valid. */
export function ratioPreview(check: RatioCheck, form: RatioForm): RatioPreview | undefined {
  const ratio = enteredRatio(form);
  const range = rangeOf(check, form.date);

  if (ratio === undefined || range === undefined) {
    return undefined;
  }

  const units = previewUnits(range.positionBefore, ratio.newShares, ratio.oldShares);

  if (units === undefined) {
    return undefined;
  }

  return {
    ratio,
    date: form.date,
    position: range.positionBefore,
    units,
    unusual: ratioLooksUnusual(ratio.newShares, ratio.oldShares),
  };
}

/** What is wrong with the entries of the ratio form, so the page can say why nothing can be sent. */
export interface RatioProblems {
  /** Both number fields are filled but give no split (equal, zero, a decimal, or 0 units after). */
  numbers: boolean;
  /** A date is entered that is in none of the allowed ranges. */
  date: boolean;
}

/** The problems of the entered ratio and date; a field that is still empty is no problem yet. */
export function ratioProblems(check: RatioCheck, form: RatioForm): RatioProblems {
  const [first, second] = form.mode === "shares" ? [form.before, form.after] : [form.newShares, form.oldShares];
  const filled = first.trim() !== "" && second.trim() !== "";
  const ratio = enteredRatio(form);
  const range = rangeOf(check, form.date);
  const noUnits =
    ratio !== undefined && range !== undefined && previewUnits(range.positionBefore, ratio.newShares, ratio.oldShares) === undefined;

  return {
    numbers: filled && (ratio === undefined || noUnits),
    date: form.date.trim() !== "" && range === undefined,
  };
}

/** The answer of a confirmed preview: the two numbers, the date and the confirmed position. */
export function ratioAnswer(preview: RatioPreview): AnswerValue {
  return {
    newShares: preview.ratio.newShares,
    oldShares: preview.ratio.oldShares,
    effectiveDate: preview.date,
    confirmedUnits: preview.units,
  };
}

/** The place on the form that a rejection marks. */
export type RejectionTarget =
  | "newShares"
  | "oldShares"
  | "effectiveDate"
  /** The market value of a stock merger question. */
  | "marketValue"
  | "confirmation"
  | "correctedUnits"
  /** The action numbers of an identity question: shown under the list of rows. */
  | "grouping"
  /** Not tied to one input: shown beside the send button. */
  | "form"
  /** The question or the revision is out of date: the page loads the run again. */
  | "reload";

const rejectionPrefix = "answer_rejected.";

/** The input or inputs a rejection marks, from its message key and parameters. */
export function rejectionTargets(messageKey: string, params: MessageParams): RejectionTarget[] {
  const key = messageKey.startsWith(rejectionPrefix) ? messageKey.slice(rejectionPrefix.length) : messageKey;

  switch (key) {
    case "ratio_not_positive":
      return params.field === "newShares" || params.field === "oldShares" ? [params.field] : ["newShares", "oldShares"];
    case "ratio_unchanged":
    case "ratio_out_of_range":
    case "isin_change_ratio_mismatch":
      return ["newShares", "oldShares"];
    case "market_value_not_positive":
      return ["marketValue"];
    case "later_sale_too_large":
      return ["newShares", "oldShares", "effectiveDate"];
    case "preview_mismatch":
      return ["confirmation"];
    case "effective_date_out_of_range":
    case "effective_date_without_units":
      return ["effectiveDate"];
    case "correction_not_positive":
    case "correction_above_open_units":
    case "correction_too_large":
    case "correction_over_budget":
      return ["correctedUnits"];
    case "grouping_incomplete":
    case "grouping_window":
      return ["grouping"];
    case "stale_revision":
    case "unknown_request":
      return ["reload"];
    default:
      return ["form"];
  }
}

/** True when the sale's window ends after 31 December of the sale's year. */
export function januaryHint(sale: LossSale): boolean {
  return sale.windowEnd.slice(0, 4) > sale.saleDate.slice(0, 4);
}
