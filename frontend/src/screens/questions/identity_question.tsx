import { useState } from "preact/hooks";

import { uiText, uiTextWith } from "../../i18n/ui_texts";
import {
  SAME_ACTION_WINDOW_DAYS,
  allOneAction,
  eachOwnAction,
  groupingAnswer,
  groupingProblem,
  rowTypeText,
  rowsByAction,
  sameDayStarts,
  type Choices,
  type GroupingProblem,
} from "../../run/identity_question";
import type { DecisionRequest, IdentityRow } from "../../run/questions";
import { brokerTitle } from "../../run/result_view";
import { QuestionFrame, type QuestionEnv } from "./question_frame";

interface Props {
  env: QuestionEnv;
  request: DecisionRequest;
}

const problemTexts = {
  incomplete: "identityIncomplete",
  gap: "identityGap",
  window: "identityWindowProblem",
} as const satisfies Record<GroupingProblem, string>;

/** The ids of the select of row `index`, so its text can describe it. */
function rowTextId(request: DecisionRequest, index: number): string {
  return `identity-${request.id}-row-${index}`;
}

/**
 * The identity question of one investment: its action rows may be one corporate action or several.
 * The user gives every row the number of its action, or uses one of the two shortcuts; the ratio
 * questions follow, one for each action. Nothing is preselected.
 */
export function IdentityQuestion({ env, request }: Props) {
  const check = request.identityCheck;
  const [choices, setChoices] = useState<Choices>(() => check?.rows.map(() => undefined) ?? []);

  if (check === undefined) {
    return null;
  }

  const { language } = env;
  const problem = groupingProblem(check, choices);
  // Nothing is said about a number that is not chosen yet: the send button waits for it.
  const shownProblem = problem === "incomplete" ? undefined : problem;
  const sameDay = problem === undefined && sameDayStarts(check, choices);
  const answer = problem === undefined ? groupingAnswer(choices) : undefined;
  const rowCount = check.rows.length;

  const choose = (index: number, value: string) =>
    setChoices(choices.map((choice, position) => (position === index ? (value === "" ? undefined : Number(value)) : choice)));

  const rowText = (row: IdentityRow) => {
    const typeKey = rowTypeText(row.type);

    return uiTextWith(language, "identityRowText", {
      broker: brokerTitle(row.broker, language),
      date: row.date,
      type: typeKey === undefined ? row.type : uiText(language, typeKey),
    });
  };

  const rowDetails = (row: IdentityRow) =>
    [
      row.transactionId === undefined ? undefined : uiTextWith(language, "identityTransaction", { id: row.transactionId }),
      row.brokerActionId === undefined ? undefined : uiTextWith(language, "identityBrokerAction", { id: row.brokerActionId }),
      row.sourceValue === undefined ? undefined : uiTextWith(language, "identitySourceValue", { value: row.sourceValue }),
    ]
      .filter((part) => part !== undefined)
      .join(", ");

  return (
    <QuestionFrame env={env} request={request} title={`${check.name} (${check.isin})`} answer={answer} allowCancel>
      {(messagesFor) => (
        <>
          <h6>{uiText(language, "identityTitle")}</h6>
          <p>{uiText(language, "identityIntro")}</p>
          <p class="hint">{uiTextWith(language, "identityWindow", { days: SAME_ACTION_WINDOW_DAYS })}</p>
          <div class="question-actions">
            <button type="button" class="button button-secondary" onClick={() => setChoices(allOneAction(rowCount))}>
              {uiText(language, "identityAllOne")}
            </button>
            <button type="button" class="button button-secondary" onClick={() => setChoices(eachOwnAction(rowCount))}>
              {uiText(language, "identityEachOwn")}
            </button>
          </div>
          <ul class="plain-list">
            {check.rows.map((row, index) => (
              <li key={index} class="identity-row">
                <span class="identity-row-text" id={rowTextId(request, index)}>
                  <strong>{rowText(row)}</strong>
                  <span>{rowDetails(row)}</span>
                </span>
                <span class="identity-row-choice">
                  <span aria-hidden="true">{uiText(language, "identityNumberLabel")}</span>
                  <select
                    class="identity-select"
                    aria-label={uiTextWith(language, "identityRowLabel", { row: index + 1 })}
                    aria-describedby={rowTextId(request, index)}
                    value={choices[index] === undefined ? "" : String(choices[index])}
                    onChange={(event) => choose(index, event.currentTarget.value)}
                  >
                    <option value="">{uiText(language, "identityChoose")}</option>
                    {check.rows.map((_, number) => (
                      <option key={number} value={String(number + 1)}>
                        {uiTextWith(language, "identityOption", { number: number + 1 })}
                      </option>
                    ))}
                  </select>
                </span>
              </li>
            ))}
          </ul>
          {answer === undefined ? null : (
            <div class="identity-summary">
              <h6>{uiText(language, "identitySummaryTitle")}</h6>
              <ul class="plain-list">
                {rowsByAction(check, choices).map((rows, index) => (
                  <li key={index}>
                    {uiTextWith(language, "identitySummaryAction", {
                      number: index + 1,
                      rows: rows
                        .map((row) =>
                          uiTextWith(language, "identitySummaryRow", {
                            broker: brokerTitle(row.broker, language),
                            date: row.date,
                          }),
                        )
                        .join(", "),
                    })}
                  </li>
                ))}
              </ul>
            </div>
          )}
          {shownProblem === undefined ? null : (
            <p class="field-error" role="alert">
              {uiTextWith(language, problemTexts[shownProblem], { days: SAME_ACTION_WINDOW_DAYS })}
            </p>
          )}
          {messagesFor(["grouping"])}
          {!sameDay ? null : (
            <p class="notice notice-warn">
              <span aria-hidden="true">⚠ </span>
              {uiText(language, "identitySameDay")}
            </p>
          )}
        </>
      )}
    </QuestionFrame>
  );
}
