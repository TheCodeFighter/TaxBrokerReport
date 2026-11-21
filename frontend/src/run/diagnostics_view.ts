import { renderDiagnostic, type MessageCatalogue } from "../i18n/messages";
import { uiText, uiTextWith, type Language, type UiTextKey } from "../i18n/ui_texts";
import type { Diagnostic, DiagnosticSource, RunResult } from "./submit";

/**
 * The four forms in their fixed order: the report key of the result JSON, the name used in
 * `affectedReports` and the form code (a name, not translated).
 */
export const reportForms: { key: string; affected: string; form: string }[] = [
  { key: "capitalGains", affected: "capital_gains", form: "Doh-KDVP" },
  { key: "dividends", affected: "dividends", form: "Doh-Div" },
  { key: "bankInterest", affected: "bank_deposit_interest", form: "Doh-DHO" },
  { key: "otherInterest", affected: "other_interest", form: "Doh-Obr" },
];

const sourcePrefix = "source-";

/**
 * The label of one source: the shown name of the sent file N for `source-000N` (with its
 * `(file N)` suffix on a repeated name), else the server's sanitized `file`, else "file N"; then
 * the row and the transaction ID when they are set. Never a path or a raw row.
 */
export function sourceLabel(source: DiagnosticSource, fileLabels: string[], language: Language): string {
  const number = source.sourceId.startsWith(sourcePrefix)
    ? Number.parseInt(source.sourceId.slice(sourcePrefix.length), 10)
    : Number.NaN;
  const sentLabel = Number.isInteger(number) && number >= 1 ? fileLabels[number - 1] : undefined;
  let file = sentLabel ?? source.file;

  if (file === undefined && Number.isInteger(number)) {
    file = uiTextWith(language, "sourceFile", { n: number });
  }

  const parts = [
    file,
    source.row === undefined ? undefined : uiTextWith(language, "sourceRow", { row: source.row }),
    source.transactionId === undefined
      ? undefined
      : uiTextWith(language, "sourceTransaction", { id: source.transactionId }),
  ];

  return parts.filter((part) => part !== undefined).join(", ");
}

/** The diagnostics of each report card, and the ones that belong to several reports or to none. */
export interface PlacedDiagnostics {
  byReport: Map<string, Diagnostic[]>;
  shared: Diagnostic[];
}

/**
 * Places each diagnostic, in result order: in the one report whose `diagnosticIds` lists it, or
 * in the shared list when two or more reports list it, or none.
 */
export function placeDiagnostics(result: RunResult): PlacedDiagnostics {
  // The one report that lists an ID, or null once a second report lists it too.
  const owners = new Map<string, string | null>();

  for (const [key, report] of Object.entries(result.reports)) {
    for (const id of report.diagnosticIds) {
      owners.set(id, owners.has(id) && owners.get(id) !== key ? null : key);
    }
  }

  const byReport = new Map<string, Diagnostic[]>();
  const shared: Diagnostic[] = [];

  for (const diagnostic of result.diagnostics) {
    const owner = owners.get(diagnostic.id);

    if (owner === undefined || owner === null) {
      shared.push(diagnostic);
      continue;
    }

    const list = byReport.get(owner);

    if (list === undefined) {
      byReport.set(owner, [diagnostic]);
    } else {
      list.push(diagnostic);
    }
  }

  return { byReport, shared };
}

/**
 * The distinct texts of the diagnostics linked to one investment, in order; a text that repeats
 * (for example three equal warnings) is shown once, and an ID the result does not have is skipped.
 */
export function linkedTexts(
  diagnosticIds: string[],
  byId: Map<string, Diagnostic>,
  catalogue: MessageCatalogue | undefined,
  language: Language,
): string[] {
  const texts = diagnosticIds.flatMap((id) => {
    const linked = byId.get(id);

    return linked === undefined ? [] : [diagnosticText(linked, catalogue, language)];
  });

  return [...new Set(texts)];
}

/** The form codes of `affectedReports`, in the result's order; an unknown name is kept as it is. */
export function affectedForms(diagnostic: Diagnostic): string[] {
  return diagnostic.affectedReports.map(
    (affected) => reportForms.find((entry) => entry.affected === affected)?.form ?? affected,
  );
}

/** Equal diagnostics shown as one entry: the first member, the count and every source in result order. */
export interface DiagnosticGroup {
  first: Diagnostic;
  count: number;
  sources: DiagnosticSource[];
}

function groupKey(diagnostic: Diagnostic): string {
  // A diagnostic with a question is never merged: each question is its own.
  if (diagnostic.requestId !== undefined) {
    return `request:${diagnostic.id}`;
  }

  const params = Object.entries(diagnostic.messageParams ?? {}).sort(([left], [right]) =>
    left < right ? -1 : left > right ? 1 : 0,
  );

  return JSON.stringify([
    diagnostic.severity,
    diagnostic.stage,
    diagnostic.code,
    diagnostic.messageKey ?? null,
    params,
    diagnostic.affectedReports,
    diagnostic.blocking,
    diagnostic.field ?? null,
    diagnostic.instrument?.name ?? null,
    diagnostic.instrument?.isin ?? null,
  ]);
}

/**
 * Groups diagnostics that differ only in their sources, tax date and ID. The entries keep the
 * order of their first member (the backend order is deterministic).
 */
export function groupDiagnostics(diagnostics: Diagnostic[]): DiagnosticGroup[] {
  const groups = new Map<string, DiagnosticGroup>();

  for (const diagnostic of diagnostics) {
    const key = groupKey(diagnostic);
    const group = groups.get(key);

    if (group === undefined) {
      groups.set(key, { first: diagnostic, count: 1, sources: [...diagnostic.sources] });
    } else {
      group.count += 1;
      group.sources.push(...diagnostic.sources);
    }
  }

  return [...groups.values()];
}

/** True for an error that still blocks; an error that no longer blocks (resolved by an answer) is shown as a warning. */
export function isBlockingError(diagnostic: Diagnostic): boolean {
  return diagnostic.severity === "error" && diagnostic.blocking;
}

/**
 * "Error" for a blocking error, "Notice" for an info diagnostic (a row the app does not read on
 * purpose), else "Warning": a warning, or an error that no longer blocks.
 */
export function severityText(diagnostic: Diagnostic): UiTextKey {
  if (diagnostic.severity === "info") {
    return "severityInfo";
  }

  return isBlockingError(diagnostic) ? "severityError" : "severityWarning";
}

const stageTexts = new Map<string, UiTextKey>([
  ["parsing", "stageParsing"],
  ["merging", "stageMerging"],
  ["historical_validation", "stageHistory"],
  ["corporate_action", "stageCorporateAction"],
  ["fifo_tax", "stageFifoTax"],
  ["xml_generation", "stageXml"],
]);

/** The text of the pipeline stage; an unknown stage has none (the diagnostic is still shown). */
export function stageText(stage: string): UiTextKey | undefined {
  return stageTexts.get(stage);
}

/**
 * The text of a diagnostic from its key and parameters. Without a key, or without the catalogue,
 * the "unknown message" text with the key or the code; never the English `message`.
 */
export function diagnosticText(
  diagnostic: Diagnostic,
  catalogue: MessageCatalogue | undefined,
  language: Language,
): string {
  if (diagnostic.messageKey === undefined || catalogue === undefined) {
    return `${uiText(language, "unknownMessage")}: ${diagnostic.messageKey ?? diagnostic.code}`;
  }

  return renderDiagnostic(catalogue, diagnostic.messageKey, diagnostic.messageParams ?? {}, language);
}
