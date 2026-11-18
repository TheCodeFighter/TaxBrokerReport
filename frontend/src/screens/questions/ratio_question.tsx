import { useState } from "preact/hooks";

import { uiText, uiTextWith } from "../../i18n/ui_texts";
import {
  SEPARATE_EVENTS_ANSWER,
  defaultEffectiveDate,
  ratioAnswer,
  ratioPreview,
  type AnswerValue,
  type DecisionRequest,
  type RatioCheck,
  type RatioForm,
} from "../../run/questions";
import { brokerTitle } from "../../run/result_view";
import { QuestionFrame, type QuestionEnv } from "./question_frame";

interface Props {
  env: QuestionEnv;
  request: DecisionRequest;
}

type SameEvent = "yes" | "no";

/** Everything that decides whether the answer may be sent, as one value of text. */
function previewKey(parts: (string | number)[]): string {
  return parts.join("|");
}

function rangesText(env: QuestionEnv, check: RatioCheck): string {
  const { language } = env;

  return check.dateRanges
    .map((range) =>
      check.dateRanges.length > 1
        ? uiTextWith(language, "ratioRangeWithUnits", {
            from: range.earliestDate,
            to: range.latestDate,
            units: range.positionBefore,
          })
        : uiTextWith(language, "ratioRangeOne", { from: range.earliestDate, to: range.latestDate }),
    )
    .join(", ");
}

/**
 * The split ratio question of one investment. The user gives the ratio (or the shares before and
 * after) and the effective date from the announcement, sees the position after the split, and
 * confirms it; only then can the answer be sent.
 */
export function RatioQuestion({ env, request }: Props) {
  const check = request.ratioCheck;
  const [sameEvent, setSameEvent] = useState<SameEvent | undefined>(undefined);
  const [form, setForm] = useState<RatioForm>({
    mode: "ratio",
    newShares: "",
    oldShares: "",
    before: "",
    after: "",
    date: check === undefined ? "" : defaultEffectiveDate(check),
  });
  // The confirmation belongs to one preview: a changed number or date needs a new confirmation.
  const [confirmedFor, setConfirmedFor] = useState<string | undefined>(undefined);

  if (check === undefined) {
    return null;
  }

  const { language } = env;
  const asksSameEvent = check.askSameEvent;
  const entersRatio = !asksSameEvent || sameEvent === "yes";
  const preview = entersRatio ? ratioPreview(check, form) : undefined;
  const key = preview === undefined ? undefined : previewKey([preview.ratio.newShares, preview.ratio.oldShares, preview.date, preview.units]);
  const confirmed = key !== undefined && confirmedFor === key;

  let answer: AnswerValue | undefined;

  if (asksSameEvent && sameEvent === "no") {
    answer = SEPARATE_EVENTS_ANSWER;
  } else if (preview !== undefined && confirmed) {
    answer = ratioAnswer(preview);
  }

  const change = (changed: Partial<RatioForm>) => setForm({ ...form, ...changed });
  const radioName = (name: string) => `${name}-${request.id}`;
  const examples = ["ratioExampleSplit", "ratioExampleReverse", "ratioExampleAdditional"] as const;

  return (
    <QuestionFrame env={env} request={request} title={`${check.name} (${check.isin})`} answer={answer}>
      {(messagesFor) => (
        <>
          <p>{uiText(language, "ratioIntro")}</p>
          <h6>{uiText(language, "ratioSourcesTitle")}</h6>
          <ul class="plain-list">
            {check.sources.map((source, index) => {
              const broker = brokerTitle(source.broker, language);

              return (
                <li key={index}>
                  {source.transactionId === undefined
                    ? uiTextWith(language, "ratioSourceNoId", { broker, date: source.date })
                    : uiTextWith(language, "ratioSource", { broker, date: source.date, id: source.transactionId })}
                  {source.sourceValue === undefined ? null : (
                    <span class="hint"> {uiTextWith(language, "ratioSourceValue", { value: source.sourceValue })}</span>
                  )}
                </li>
              );
            })}
          </ul>
          {check.earlierSaleDate === undefined ? null : (
            <p class="hint">{uiTextWith(language, "ratioEarlierSale", { date: check.earlierSaleDate })}</p>
          )}
          <p class="notice notice-warn">
            <span aria-hidden="true">⚠ </span>
            {uiText(language, "ratioWarning")}
          </p>
          <h6>{uiText(language, "ratioExamplesTitle")}</h6>
          <ul class="plain-list">
            {examples.map((example) => (
              <li key={example}>{uiText(language, example)}</li>
            ))}
          </ul>
          {!asksSameEvent ? null : (
            <fieldset class="field">
              <legend>{uiText(language, "ratioSameEventTitle")}</legend>
              {(["yes", "no"] as const).map((value) => (
                <label key={value} class="radio">
                  <input
                    type="radio"
                    name={radioName("same")}
                    value={value}
                    checked={sameEvent === value}
                    onChange={() => setSameEvent(value)}
                  />
                  {uiText(language, value === "yes" ? "ratioSameEventYes" : "ratioSameEventNo")}
                </label>
              ))}
            </fieldset>
          )}
          {!entersRatio ? null : (
            <>
              <fieldset class="field">
                {(["ratio", "shares"] as const).map((mode) => (
                  <label key={mode} class="radio">
                    <input
                      type="radio"
                      name={radioName("mode")}
                      value={mode}
                      checked={form.mode === mode}
                      onChange={() => change({ mode })}
                    />
                    {uiText(language, mode === "ratio" ? "ratioModeRatio" : "ratioModeShares")}
                  </label>
                ))}
              </fieldset>
              {form.mode === "ratio" ? (
                <>
                  <label class="field">
                    <span>{uiText(language, "ratioNew")}</span>
                    <input
                      type="text"
                      inputMode="numeric"
                      value={form.newShares}
                      onInput={(event) => change({ newShares: event.currentTarget.value })}
                    />
                  </label>
                  <label class="field">
                    <span>{uiText(language, "ratioOld")}</span>
                    <input
                      type="text"
                      inputMode="numeric"
                      value={form.oldShares}
                      onInput={(event) => change({ oldShares: event.currentTarget.value })}
                    />
                  </label>
                </>
              ) : (
                <>
                  <label class="field">
                    <span>{uiText(language, "ratioBefore")}</span>
                    <input
                      type="text"
                      inputMode="decimal"
                      value={form.before}
                      onInput={(event) => change({ before: event.currentTarget.value })}
                    />
                  </label>
                  <label class="field">
                    <span>{uiText(language, "ratioAfter")}</span>
                    <input
                      type="text"
                      inputMode="decimal"
                      value={form.after}
                      onInput={(event) => change({ after: event.currentTarget.value })}
                    />
                  </label>
                </>
              )}
              {messagesFor(["newShares", "oldShares"])}
              <label class="field">
                <span>{uiText(language, "ratioDate")}</span>
                <input
                  type="text"
                  inputMode="numeric"
                  value={form.date}
                  onInput={(event) => change({ date: event.currentTarget.value })}
                />
                <span class="hint">{uiTextWith(language, "ratioRanges", { ranges: rangesText(env, check) })}</span>
              </label>
              {messagesFor(["effectiveDate"])}
              {preview === undefined ? null : (
                <>
                  <p class="notice">
                    {uiTextWith(language, "ratioPreview", {
                      new: preview.ratio.newShares,
                      old: preview.ratio.oldShares,
                      before: preview.position,
                      after: preview.units,
                    })}
                  </p>
                  {!preview.unusual ? null : (
                    <p class="notice notice-warn">
                      <span aria-hidden="true">⚠ </span>
                      {uiText(language, "ratioUnusual")}
                    </p>
                  )}
                  <label class="radio">
                    <input
                      type="checkbox"
                      checked={confirmed}
                      onChange={(event) => setConfirmedFor(event.currentTarget.checked ? key : undefined)}
                    />
                    {uiText(language, "ratioConfirm")}
                  </label>
                </>
              )}
              {messagesFor(["confirmation"])}
            </>
          )}
        </>
      )}
    </QuestionFrame>
  );
}
