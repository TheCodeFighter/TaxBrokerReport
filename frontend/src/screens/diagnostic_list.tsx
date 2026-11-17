import { useState } from "preact/hooks";

import type { MessageCatalogue } from "../i18n/messages";
import { uiText, uiTextWith, type Language } from "../i18n/ui_texts";
import {
  affectedForms,
  diagnosticText,
  severityText,
  sourceLabel,
  stageText,
  type DiagnosticGroup,
} from "../run/diagnostics_view";
import type { DiagnosticSource } from "../run/submit";

interface Props {
  language: Language;
  groups: DiagnosticGroup[];
  fileLabels: string[];
  catalogue: MessageCatalogue | undefined;
  /** True in the shared list: each entry names the forms it affects. */
  showForms: boolean;
}

/** A list of grouped diagnostics: severity, count, stage, field, forms, text and sources. */
export function DiagnosticList({ language, groups, fileLabels, catalogue, showForms }: Props) {
  return (
    <ul class="plain-list diagnostic-list">
      {groups.map((group) => {
        const diagnostic = group.first;
        const stage = stageText(diagnostic.stage);
        const forms = showForms ? affectedForms(diagnostic) : [];
        const meta = [
          uiText(language, severityText(diagnostic)),
          group.count > 1 ? uiTextWith(language, "groupCount", { count: group.count }) : undefined,
          stage === undefined ? undefined : uiText(language, stage),
          diagnostic.field === undefined ? undefined : uiTextWith(language, "fieldLabel", { field: diagnostic.field }),
          forms.length === 0 ? undefined : forms.join(", "),
        ].filter((part) => part !== undefined);
        const { instrument } = diagnostic;
        const instrumentText = [instrument?.name, instrument?.isin === undefined ? undefined : `(${instrument.isin})`]
          .filter((part) => part !== undefined)
          .join(" ");
        const tone = diagnostic.severity === "error" && diagnostic.blocking ? "fail" : "warn";

        return (
          <li key={diagnostic.id} class={`diagnostic tone-${tone}`}>
            <p class="diagnostic-meta">{meta.join(" · ")}</p>
            <p>{diagnosticText(diagnostic, catalogue, language)}</p>
            {instrumentText === "" ? null : <p>{instrumentText}</p>}
            <SourceList language={language} sources={group.sources} fileLabels={fileLabels} />
          </li>
        );
      })}
    </ul>
  );
}

interface SourceProps {
  language: Language;
  sources: DiagnosticSource[];
  fileLabels: string[];
}

/** The sources, one per line, behind a "Details" button; the labels are only built once it is opened. */
function SourceList({ language, sources, fileLabels }: SourceProps) {
  const [open, setOpen] = useState(false);

  if (sources.length === 0) {
    return null;
  }

  return (
    <details class="diagnostic-sources" onToggle={(event) => setOpen((event.currentTarget as HTMLDetailsElement).open)}>
      <summary>{uiTextWith(language, "detailsSummary", { count: sources.length })}</summary>
      {open ? (
        <ul class="plain-list">
          {sources.map((source, index) => (
            <li key={index}>{sourceLabel(source, fileLabels, language)}</li>
          ))}
        </ul>
      ) : null}
    </details>
  );
}
