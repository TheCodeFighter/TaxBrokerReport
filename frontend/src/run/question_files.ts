import type { Language } from "../i18n/ui_texts";
import { sourceLabel } from "./diagnostics_view";
import { brokerTitle } from "./result_view";
import type { DiagnosticSource } from "./submit";

/**
 * The files an open question is about, one line for each file in the order of the file numbers:
 * the broker and the shown name of the file (`Trade Republic: tr_2023.csv`). A file appears once
 * however many rows of it the question uses; no row, transaction ID or path is part of a line.
 */
export function questionFiles(sources: DiagnosticSource[], fileLabels: string[], language: Language): string[] {
  const bySourceId = new Map<string, DiagnosticSource>();

  for (const source of sources) {
    if (!bySourceId.has(source.sourceId)) {
      bySourceId.set(source.sourceId, source);
    }
  }

  return [...bySourceId.keys()]
    .sort()
    .map((sourceId) => {
      const source = bySourceId.get(sourceId) as DiagnosticSource;
      const file = sourceLabel({ sourceId, ...(source.file === undefined ? {} : { file: source.file }) }, fileLabels, language);

      return source.broker === undefined ? file : `${brokerTitle(source.broker, language)}: ${file}`;
    });
}
