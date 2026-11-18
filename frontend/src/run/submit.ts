import type { ApiClient } from "../api/client";
import { saveProfile } from "../api/profile";
import type { MessageParams } from "../i18n/messages";
import { MAX_FILE_BYTES, MAX_FILES } from "./limits";
import { orderedBrokerFiles, type RunInputs } from "./inputs";
import type { Correction, Decision, DecisionRequest, Reconciliation } from "./questions";
import { buildRunRequest } from "./request";
import { taxpayerObject } from "./taxpayer";

/** A safe source of a diagnostic: `file` only when the name passed the server's sanitizer. */
export interface DiagnosticSource {
  sourceId: string;
  broker?: string;
  file?: string;
  row?: number;
  transactionId?: string;
}

/** One diagnostic of the result. The text comes from `messageKey`, never from `message`. */
export interface Diagnostic {
  id: string;
  severity: string;
  stage: string;
  code: string;
  messageKey?: string;
  messageParams?: MessageParams;
  /** The forms it affects, as `capital_gains`, `dividends`, `bank_deposit_interest`, `other_interest`. */
  affectedReports: string[];
  blocking: boolean;
  sources: DiagnosticSource[];
  taxDate?: string;
  field?: string;
  instrument?: { name?: string; isin?: string };
  requestId?: string;
}

/** An investment left out of one form, or exported unsafely, with the diagnostics that explain it. */
export interface ReportInvestment {
  name: string;
  isin: string;
  reasonCodes: string[];
  diagnosticIds: string[];
}

/** The reference row a payer or a treaty text came from. */
export interface CatalogueRow {
  /** The upstream project or the supplied file's own label. */
  source: string;
  sourceRevision: string;
  file: string;
  row: number;
  /** `built_in` or `supplied`. */
  origin: string;
}

/** One row of an income form: a dividend, a deposit interest payer, cash interest or a coupon. */
export interface IncomeRow {
  /** `dividend`, `deposit_interest`, `broker_cash_interest` or `bond_coupon`. */
  kind: string;
  isin?: string;
  broker?: string;
  /** Not set for a Doh-DHO row, which sums several payments. */
  date?: string;
  grossEur: string;
  foreignTaxEur: string;
  /** True when the export's tax field was blank (no tax in the export), false for a real 0. */
  taxBlankInExport: boolean;
  /** Empty when the payer was not found in the reference files. */
  payer: { name?: string; country?: string; catalogueRow?: CatalogueRow };
  treaty: { claim: "none" | "reference_text" | "user_text"; catalogueRow?: CatalogueRow };
  evidenceRequired: boolean;
}

export interface IncomeSummary {
  grossEur: string;
  foreignTaxEur: string;
  rowCount: number;
  rows: IncomeRow[];
}

/** The count and total of one currency; `total` is `null` when it is not known. */
export interface CurrencyTotal {
  currency: string;
  count: number;
  total: string | null;
}

/** The fees of one broker. `totalEur` is `null` when a fee has no rate: no partial sum is shown. */
export interface BrokerFees {
  broker: string;
  byCurrency: CurrencyTotal[];
  totalEur: string | null;
  /** The number of fees converted with the broker's own rate, not an official one. */
  brokerRateCount: number;
}

/** The benefits (saveback, stock perk, bonus, referral) of one broker and type. */
export interface BrokerBenefits {
  broker: string;
  /** `saveback`, `stock_perk`, `bonus` or `referral`. */
  type: string;
  byCurrency: CurrencyTotal[];
  totalEur: string | null;
}

export interface FeeSummary {
  brokers: BrokerFees[];
  totalEur: string | null;
}

export interface BenefitSummary {
  brokers: BrokerBenefits[];
  totalEur: string | null;
}

/** The imported deposit interest against the EUR 1,000 allowance; only when the year has some. */
export interface DepositInterest {
  importedInterestEur: string;
  limitEur: string;
  /** True when the form holds only the imported interest and the user adds the other banks'. */
  addOtherBanks: boolean;
}

/** One report of the result (`capitalGains`, `dividends`, `bankInterest`, `otherInterest`). */
export interface ReportResult {
  status: string;
  diagnosticIds: string[];
  requestIds: string[];
  excludedInvestments?: ReportInvestment[];
  excludedBrokers?: { broker: string; reasonCodes: string[]; diagnosticIds: string[] }[];
  failureResolution?: string;
  unsafeInvestments?: ReportInvestment[];
  unsafe: boolean;
  downloadAvailable?: boolean;
  /** Why a `no_data` report has no data, when the backend knows it. */
  reason?: string;
  /** The income forms only. */
  incomeSummary?: IncomeSummary;
}

/** An instrument left out by classification (a tax rule or a user choice), not by one form. */
export interface ExcludedItem {
  name: string;
  isin?: string;
  category: string;
  reason: string;
  basis: string;
  diagnosticIds: string[];
}

/** The part of the result JSON (schema 2) that the screens show. */
export interface RunResult {
  status: string;
  runRevision: number;
  reports: Record<string, ReportResult>;
  diagnostics: Diagnostic[];
  excludedItems: ExcludedItem[];
  /** The open questions. */
  requests: DecisionRequest[];
  /** The recorded answers, also those of earlier runs. */
  decisions: Decision[];
  /** The sale quantities that were corrected, and the cumulative impact per ISIN. */
  corrections: Correction[];
  reconciliation: Reconciliation[];
  /** The informational summaries of the year; no form holds them. */
  feeSummary?: FeeSummary;
  benefitSummary?: BenefitSummary;
  depositInterest?: DepositInterest;
}

/** A rate or reference file that the server could not read (the 422 `inputFault`). */
export interface InputFault {
  kind: string;
  messageKey: string;
  messageParams: MessageParams;
  source?: { file?: string; row?: number };
}

/** How the save of the taxpayer details went, when they were to be saved. */
export type ProfileSave = "saved" | "not_saved";

type RunOutcome =
  | { kind: "result"; result: RunResult }
  /** A 400: the stable `code`, and for `bad_taxpayer` the `field` of the taxpayer object. */
  | { kind: "refused"; code: string; field?: string }
  /** A file over the limit (checked before sending: `fileName`) or a 413 of the server. */
  | { kind: "tooLarge"; fileName?: string }
  | { kind: "inputFault"; fault: InputFault }
  /** Any other answer, for example a 500 of the server. */
  | { kind: "unexpected" };

/**
 * The outcome of the run. `profile` is set whenever the details were saved before the run (also
 * when the run itself failed): `saved`, or `not_saved` (the run went on).
 */
export type SubmitOutcome = RunOutcome & { profile?: ProfileSave };

function allFiles(inputs: RunInputs): File[] {
  return [...orderedBrokerFiles(inputs).map((entry) => entry.file), ...inputs.rateFiles, ...inputs.referenceFiles];
}

async function readJson(response: Response): Promise<Record<string, unknown>> {
  try {
    return (await response.json()) as Record<string, unknown>;
  } catch {
    return {};
  }
}

/**
 * Sends the run and sorts the answer. The count and size of the files are checked first, so a
 * refused large body never reaches the network. A lost session (401, no server) throws
 * `SessionEndedError` from the client; the page already shows its screen. With `rememberDetails`
 * the taxpayer details are saved first: a refused field stops the run (the form marks it), a
 * failed write does not (the result says `not_saved`).
 */
export async function submitRun(
  apiFetch: ApiClient["apiFetch"],
  inputs: RunInputs,
  rememberDetails = false,
): Promise<SubmitOutcome> {
  const files = allFiles(inputs);

  if (files.length > MAX_FILES) {
    return { kind: "refused", code: "too_many_files" };
  }

  const tooLarge = files.find((file) => file.size > MAX_FILE_BYTES);

  if (tooLarge !== undefined) {
    return { kind: "tooLarge", fileName: tooLarge.name };
  }

  let profile: ProfileSave | undefined;

  if (rememberDetails) {
    const saved = await saveProfile(apiFetch, taxpayerObject(inputs.taxpayer));

    if (saved.kind === "refused") {
      return { kind: "refused", code: "bad_taxpayer", ...(saved.field === undefined ? {} : { field: saved.field }) };
    }

    profile = saved.kind === "saved" ? "saved" : "not_saved";
  }

  const outcome = await sendRun(apiFetch, inputs);

  return profile === undefined ? outcome : { ...outcome, profile };
}

/** Sends the run and sorts the answer. */
async function sendRun(apiFetch: ApiClient["apiFetch"], inputs: RunInputs): Promise<RunOutcome> {
  const response = await apiFetch("/api/run", { method: "POST", body: buildRunRequest(inputs) });

  if (response.status === 200) {
    return { kind: "result", result: (await response.json()) as RunResult };
  }

  const body = await readJson(response);

  if (response.status === 413) {
    return { kind: "tooLarge" };
  }

  if (response.status === 422 && typeof body.inputFault === "object" && body.inputFault !== null) {
    return { kind: "inputFault", fault: body.inputFault as InputFault };
  }

  if (response.status === 400 && typeof body.error === "string") {
    return {
      kind: "refused",
      code: body.error,
      ...(typeof body.field === "string" ? { field: body.field } : {}),
    };
  }

  return { kind: "unexpected" };
}
