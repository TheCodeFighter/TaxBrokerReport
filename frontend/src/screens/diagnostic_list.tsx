import { useState } from "preact/hooks";

import type { MessageCatalogue } from "../i18n/messages";
import { uiText, uiTextWith, type Language } from "../i18n/ui_texts";
import {
  affectedForms,
  diagnosticText,
  isBlockingError,
  severityText,
  sourceLabel,
  stageText,
  type DiagnosticGroup,
} from "../run/diagnostics_view";
import type { DiagnosticSource } from "../run/submit";
import { QuestionLink } from "./questions/question_link";

interface Props {
  language: Language;
  groups: DiagnosticGroup[];
  fileLabels: string[];
  catalogue: MessageCatalogue | undefined;
  /** True in the shared list: each entry names the forms it affects. */
  showForms: boolean;
  /** The IDs of the open questions that have a screen: their diagnostics get a link to the question. */
  questionIds: ReadonlySet<string>;
}

/** A list of grouped diagnostics: severity, count, stage, field, forms, text and sources. */
export function DiagnosticList({ language, groups, fileLabels, catalogue, showForms, questionIds }: Props) {
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
        const tone = isBlockingError(diagnostic) ? "fail" : diagnostic.severity === "info" ? "neutral" : "warn";
        const { requestId } = diagnostic;

        return (
          <li key={diagnostic.id} class={`diagnostic tone-${tone}`}>
            <p class="diagnostic-meta">{meta.join(" · ")}</p>
            <p>{diagnosticText(diagnostic, catalogue, language)}</p>
            {instrumentText === "" ? null : <p>{instrumentText}</p>}
            {requestId !== undefined && questionIds.has(requestId) ? (
              <QuestionLink language={language} requestId={requestId} label="answerThisQuestion" />
            ) : null}
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
