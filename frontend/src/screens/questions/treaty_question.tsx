import { useState } from "preact/hooks";

import { uiText, uiTextWith } from "../../i18n/ui_texts";
import { NO_CLAIM_ANSWER, treatyAnswer, treatyText, type TreatyProblem } from "../../run/income_questions";
import type { DecisionRequest } from "../../run/questions";
import { QuestionFrame, type QuestionEnv } from "./question_frame";

interface Props {
  env: QuestionEnv;
  request: DecisionRequest;
}

const problemTexts = {
  empty: undefined,
  tooLong: "treatyTooLong",
  yesNo: "treatyYesNo",
  characters: "treatyCharacters",
} as const satisfies Record<TreatyProblem, string | undefined>;

/** The treaty text of a country's taxed dividends, or no claim. Nothing is preselected. */
export function TreatyQuestion({ env, request }: Props) {
  const [mode, setMode] = useState<"text" | "none" | undefined>(undefined);
  const [text, setText] = useState("");
  const check = request.treatyCheck;

  if (check === undefined) {
    return null;
  }

  const { language } = env;
  const radioName = `treaty-${request.id}`;
  const checked = treatyText(text);
  const problem = checked.ok ? undefined : problemTexts[checked.problem];
  const answer = mode === "none" ? NO_CLAIM_ANSWER : mode === "text" ? treatyAnswer(text) : undefined;
  const errorId = `treaty-error-${request.id}`;

  return (
    <QuestionFrame
      env={env}
      request={request}
      title={uiTextWith(language, "treatyTitle", { country: check.country, year: check.year })}
      answer={answer}
      allowCancel={false}
    >
      {() => (
        <>
          <p>{uiText(language, "treatyIntro")}</p>
          <p class="hint">{uiTextWith(language, "treatyIsins", { isins: check.isins.join(", ") })}</p>
          <fieldset class="field">
            <legend>{uiText(language, "treatyQuestion")}</legend>
            <label class="radio">
              <input type="radio" name={radioName} value="text" checked={mode === "text"} onChange={() => setMode("text")} />
              {uiText(language, "treatyChoiceText")}
            </label>
            <label class="radio">
              <input type="radio" name={radioName} value="none" checked={mode === "none"} onChange={() => setMode("none")} />
              {uiText(language, "treatyChoiceNone")}
            </label>
          </fieldset>
          {mode !== "text" ? null : (
            <label class="field">
              <span>{uiText(language, "treatyInput")}</span>
              <input
                type="text"
                value={text}
                aria-describedby={errorId}
                aria-invalid={problem !== undefined}
                onInput={(event) => setText(event.currentTarget.value)}
              />
            </label>
          )}
          <div id={errorId}>
            {mode !== "text" || problem === undefined ? null : (
              <p class="field-error" role="alert">
                {uiText(language, problem)}
              </p>
            )}
          </div>
        </>
      )}
    </QuestionFrame>
  );
}
