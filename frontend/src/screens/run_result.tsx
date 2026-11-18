import { useEffect, useMemo, useRef } from "preact/hooks";

import type { ApiClient } from "../api/client";
import type { MessageCatalogue } from "../i18n/messages";
import type { Language, UiTextKey } from "../i18n/ui_texts";
import { uiText, uiTextWith } from "../i18n/ui_texts";
import { groupDiagnostics, linkedTexts, placeDiagnostics } from "../run/diagnostics_view";
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
import { firstQuestionId, placeQuestions, supportedQuestions, type DecisionRequest } from "../run/questions";
import type { Diagnostic, ReportInvestment, RunResult } from "../run/submit";
import { DiagnosticList } from "./diagnostic_list";
import { CorrectionsBlock } from "./questions/corrections_block";
import type { QuestionEnv } from "./questions/question_frame";
import { QuestionLink } from "./questions/question_link";
import { QuestionList } from "./questions/question_list";
import { RecordedAnswers } from "./questions/recorded_answers";
import { ReportDownloadButton } from "./report_download_button";

interface Props {
  language: Language;
  shown: ShownResult;
  catalogue: MessageCatalogue | undefined;
  apiFetch: ApiClient["apiFetch"];
  /** A note about the result after an answer (refreshed, or the run is gone). */
  note: UiTextKey | undefined;
  /** An answer or a delete gave a new result. */
  onResult: (result: RunResult) => void;
  /** The question or the revision was out of date: the screen loads the run again. */
  onStale: () => void;
  onNoRun: () => void;
}

/**
 * The result of the last run: its status, one card per form with everything that affects it,
 * the messages of several reports, the classification exclusions and the file list.
 */
export function RunResultView({ language, shown, catalogue, apiFetch, note, onResult, onStale, onNoRun }: Props) {
  const { result } = shown;
  const counts = countDiagnostics(result);
  const titleRef = useRef<HTMLHeadingElement>(null);
  const placed = useMemo(() => placeDiagnostics(result), [result]);
  const byId = useMemo(() => new Map(result.diagnostics.map((diagnostic) => [diagnostic.id, diagnostic])), [result]);
  const shared = useMemo(() => groupDiagnostics(placed.shared), [placed]);
  const classification = classificationExclusions(result);
  const tone = overallTone(result.status);
  const cards = reportCards(result);
  const placedQuestions = placeQuestions(
    result,
    cards.map((card) => card.key),
  );
  const questionIds = new Set(supportedQuestions(result).map((request) => request.id));
  const firstQuestion = firstQuestionId(result);
  const env: QuestionEnv = {
    language,
    catalogue,
    apiFetch,
    revision: result.runRevision,
    disabled: shown.outOfDate,
    onResult,
    onStale,
    onNoRun,
  };

  // A new run or a new revision (after an answer) takes the focus, which also scrolls it into
  // view and makes a screen reader read it.
  useEffect(() => {
    titleRef.current?.focus();
  }, [shown.fileLabels, result.runRevision]);

  return (
    <section class="run-section run-result" aria-labelledby="result-title">
      <h2 id="result-title" ref={titleRef} tabIndex={-1}>{uiText(language, "resultTitle")}</h2>
      {shown.outOfDate ? <p class="notice">{uiText(language, "resultOutOfDate")}</p> : null}
      <div class={shown.outOfDate ? "result result-old" : "result"}>
        <p class={`result-status tone-${tone}`}>{uiText(language, overallStatusText(result.status))}</p>
        {note === undefined ? null : (
          <p class="notice" role="status">
            {uiText(language, note)}
          </p>
        )}
        {needsAnswerHint(result) ? <p class="notice">{uiText(language, "answersLater")}</p> : null}
        {firstQuestion === undefined ? null : (
          <p>
            <QuestionLink
              language={language}
              requestId={firstQuestion}
              label="answerQuestionsButton"
              primary
              count={questionIds.size}
            />
          </p>
        )}
        <p>
          {uiTextWith(language, "resultCounts", { errors: counts.errors, warnings: counts.warnings })}
        </p>
        {cards.map((card) => (
          <ReportCardView
            key={card.key}
            language={language}
            card={card}
            diagnostics={placed.byReport.get(card.key) ?? []}
            questions={placedQuestions.byReport.get(card.key) ?? []}
            questionIds={questionIds}
            env={env}
            result={result}
            byId={byId}
            fileLabels={shown.fileLabels}
            catalogue={catalogue}
            apiFetch={apiFetch}
            revision={result.runRevision}
            outOfDate={shown.outOfDate}
          />
        ))}
        <QuestionList env={env} requests={placedQuestions.rest} />
        <RecordedAnswers env={env} decisions={result.decisions} />
        {shared.length === 0 ? null : (
          <>
            <h3>{uiText(language, "sharedMessagesTitle")}</h3>
            <DiagnosticList
              language={language}
              groups={shared}
              fileLabels={shown.fileLabels}
              catalogue={catalogue}
              showForms={true}
              questionIds={questionIds}
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
  /** The open questions of this form that have a screen. */
  questions: DecisionRequest[];
  questionIds: ReadonlySet<string>;
  env: QuestionEnv;
  result: RunResult;
  byId: Map<string, Diagnostic>;
  fileLabels: string[];
  catalogue: MessageCatalogue | undefined;
  apiFetch: ApiClient["apiFetch"];
  revision: number;
  outOfDate: boolean;
}

/**
 * One form: its status, the no-data reason, questions, the choice, exclusions, the save button of
 * its XML file and its own messages.
 */
function ReportCardView({
  language,
  card,
  diagnostics,
  questions,
  questionIds,
  env,
  result,
  byId,
  fileLabels,
  catalogue,
  apiFetch,
  revision,
  outOfDate,
}: CardProps) {
  const headingId = `report-${card.key}`;
  const groups = groupDiagnostics(diagnostics);

  const investmentList = (investments: ReportInvestment[]) => (
    <ul class="plain-list">
      {investments.map((investment) => (
        <li key={investment.isin}>
          {`${investment.name} (${investment.isin})`}
          {linkedTexts(investment.diagnosticIds, byId, catalogue, language).map((text) => (
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
      {card.questions <= questions.length ? null : (
        <p>{uiTextWith(language, "questionsWaiting", { count: card.questions - questions.length })}</p>
      )}
      {questions.length === 0 || card.tone !== "pending" ? null : (
        <QuestionLink language={language} requestId={questions[0]?.id ?? ""} label="goToQuestion" />
      )}
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
      {card.key === "capitalGains" ? (
        <CorrectionsBlock language={language} corrections={result.corrections} reconciliation={result.reconciliation} />
      ) : null}
      <QuestionList env={env} requests={questions} />
      {card.downloadAvailable ? (
        <ReportDownloadButton
          key={revision}
          language={language}
          apiFetch={apiFetch}
          reportKey={card.key}
          form={card.form}
          revision={revision}
          disabled={outOfDate}
        />
      ) : null}
      {groups.length === 0 ? null : (
        <DiagnosticList
          language={language}
          groups={groups}
          fileLabels={fileLabels}
          catalogue={catalogue}
          showForms={false}
          questionIds={questionIds}
        />
      )}
    </section>
  );
}
