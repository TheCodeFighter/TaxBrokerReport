import { useEffect, useMemo, useRef } from "preact/hooks";

import type { MessageCatalogue } from "../i18n/messages";
import type { Language } from "../i18n/ui_texts";
import { uiText, uiTextWith } from "../i18n/ui_texts";
import { diagnosticText, groupDiagnostics, placeDiagnostics } from "../run/diagnostics_view";
import {
  brokerTitle,
  classificationExclusions,
  countDiagnostics,
  needsAnswerHint,
  overallStatusText,
  overallTone,
  reportCards,
  type ReportCard,
  type ShownResult,
} from "../run/result_view";
import type { Diagnostic, ReportInvestment } from "../run/submit";
import { DiagnosticList } from "./diagnostic_list";

interface Props {
  language: Language;
  shown: ShownResult;
  catalogue: MessageCatalogue | undefined;
}

/**
 * The result of the last run: its status, one card per form with everything that affects it,
 * the messages of several reports, the classification exclusions and the file list.
 */
export function RunResultView({ language, shown, catalogue }: Props) {
  const { result } = shown;
  const counts = countDiagnostics(result);
  const titleRef = useRef<HTMLHeadingElement>(null);
  const placed = useMemo(() => placeDiagnostics(result), [result]);
  const byId = useMemo(() => new Map(result.diagnostics.map((diagnostic) => [diagnostic.id, diagnostic])), [result]);
  const shared = useMemo(() => groupDiagnostics(placed.shared), [placed]);
  const classification = classificationExclusions(result);
  const tone = overallTone(result.status);

  // A new result takes the focus, which also scrolls it into view and makes a screen reader read it.
  useEffect(() => {
    titleRef.current?.focus();
  }, [result]);

  return (
    <section class="run-section run-result" aria-labelledby="result-title">
      <h2 id="result-title" ref={titleRef} tabIndex={-1}>{uiText(language, "resultTitle")}</h2>
      {shown.outOfDate ? <p class="notice">{uiText(language, "resultOutOfDate")}</p> : null}
      <div class={shown.outOfDate ? "result result-old" : "result"}>
        <p class={`result-status tone-${tone}`}>{uiText(language, overallStatusText(result.status))}</p>
        {needsAnswerHint(result) ? <p class="notice">{uiText(language, "answersLater")}</p> : null}
        <p>
          {uiTextWith(language, "resultCounts", { errors: counts.errors, warnings: counts.warnings })}
        </p>
        {reportCards(result).map((card) => (
          <ReportCardView
            key={card.key}
            language={language}
            card={card}
            diagnostics={placed.byReport.get(card.key) ?? []}
            byId={byId}
            fileLabels={shown.fileLabels}
            catalogue={catalogue}
          />
        ))}
        {shared.length === 0 ? null : (
          <>
            <h3>{uiText(language, "sharedMessagesTitle")}</h3>
            <DiagnosticList
              language={language}
              groups={shared}
              fileLabels={shown.fileLabels}
              catalogue={catalogue}
              showForms={true}
            />
          </>
        )}
        {classification.length === 0 ? null : (
          <>
            <h3>{uiText(language, "classificationTitle")}</h3>
            <ul class="plain-list">
              {classification.map((row, index) => (
                <li key={index}>
                  {row.isin === undefined ? row.name : `${row.name} (${row.isin})`}
                  {row.reasonText === undefined ? null : ` — ${uiText(language, row.reasonText)}`}
                </li>
              ))}
            </ul>
          </>
        )}
        <h3>{uiText(language, "resultFiles")}</h3>
        <ol class="plain-list">
          {shown.fileLabels.map((label, index) => (
            <li key={index}>{label}</li>
          ))}
        </ol>
      </div>
    </section>
  );
}

interface CardProps {
  language: Language;
  card: ReportCard;
  diagnostics: Diagnostic[];
  byId: Map<string, Diagnostic>;
  fileLabels: string[];
  catalogue: MessageCatalogue | undefined;
}

/** One form: its status, the no-data reason, questions, the choice, exclusions and its own messages. */
function ReportCardView({ language, card, diagnostics, byId, fileLabels, catalogue }: CardProps) {
  const headingId = `report-${card.key}`;
  const groups = groupDiagnostics(diagnostics);

  // Each distinct text of the linked diagnostics once; the card's list below has every source.
  const linkedTexts = (investment: ReportInvestment): string[] => {
    const texts = investment.diagnosticIds.flatMap((id) => {
      const linked = byId.get(id);

      return linked === undefined ? [] : [diagnosticText(linked, catalogue, language)];
    });

    return [...new Set(texts)];
  };

  const investmentList = (investments: ReportInvestment[]) => (
    <ul class="plain-list">
      {investments.map((investment) => (
        <li key={investment.isin}>
          {`${investment.name} (${investment.isin})`}
          {linkedTexts(investment).map((text) => (
            <p key={text} class="hint">
              {text}
            </p>
          ))}
        </li>
      ))}
    </ul>
  );

  return (
    <section class={`report-card tone-${card.tone}`} aria-labelledby={headingId}>
      <div class="report-card-head">
        <h3 id={headingId}>{card.form}</h3>
        <span class={`badge tone-${card.tone}`}>{uiText(language, card.statusText)}</span>
        {card.unsafe ? <span class="badge tone-fail">{uiText(language, "unsafeMarker")}</span> : null}
      </div>
      {card.noDataText === undefined ? null : <p>{uiText(language, card.noDataText)}</p>}
      {card.questions === 0 ? null : <p>{uiTextWith(language, "questionsWaiting", { count: card.questions })}</p>}
      {card.resolutionText === undefined ? null : <p>{uiText(language, card.resolutionText)}</p>}
      {card.excluded.length === 0 ? null : (
        <>
          <h4>{uiText(language, "excludedTitle")}</h4>
          {investmentList(card.excluded)}
        </>
      )}
      {card.excludedBrokers.length === 0 ? null : (
        <>
          <h4>{uiText(language, "excludedBrokersTitle")}</h4>
          <ul class="plain-list">
            {card.excludedBrokers.map((broker) => (
              <li key={broker}>{brokerTitle(broker, language)}</li>
            ))}
          </ul>
        </>
      )}
      {card.unsafeInvestments.length === 0 ? null : (
        <>
          <h4>{uiText(language, "unsafeTitle")}</h4>
          {investmentList(card.unsafeInvestments)}
        </>
      )}
      {card.evidenceRows === 0 ? null : (
        <p>{uiTextWith(language, "evidenceRows", { count: card.evidenceRows })}</p>
      )}
      {groups.length === 0 ? null : (
        <DiagnosticList
          language={language}
          groups={groups}
          fileLabels={fileLabels}
          catalogue={catalogue}
          showForms={false}
        />
      )}
    </section>
  );
}
