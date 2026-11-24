import { useState } from "preact/hooks";

import { uiText } from "../../i18n/ui_texts";
import type { UiTextKey } from "../../i18n/ui_texts";
import { countryAnswer, SKIP_ANSWER, typedCountry, typedCountryIsWrong } from "../../run/income_questions";
import type { DecisionRequest } from "../../run/questions";
import { QuestionFrame, type QuestionEnv } from "./question_frame";

interface Props {
  env: QuestionEnv;
  request: DecisionRequest;
}

const originTexts = new Map<string, UiTextKey>([
  ["export", "countryOriginExport"],
  ["reference", "countryOriginReference"],
  ["export_and_reference", "countryOriginBoth"],
]);

/** What the user chose: a candidate (its code), another country, a skip, or nothing yet. */
type Pick = { kind: "candidate"; country: string } | { kind: "other" } | { kind: "skip" } | undefined;

/**
 * The sources of the payer country differ: pick a candidate, type another country code or skip
 * (the country stays empty). Nothing is preselected. A typed code is checked against the country
 * list here and again on the server.
 */
export function CountryQuestion({ env, request }: Props) {
  const [picked, setPicked] = useState<Pick>(undefined);
  const [typed, setTyped] = useState("");
  const check = request.countryCheck;

  if (check === undefined) {
    return null;
  }

  const { language } = env;
  const radioName = `country-${request.id}`;
  const errorId = `country-error-${request.id}`;
  const code = typedCountry(typed);
  const typedProblem = picked?.kind === "other" && typedCountryIsWrong(typed);
  let answer;

  if (picked?.kind === "candidate") {
    answer = countryAnswer(picked.country);
  } else if (picked?.kind === "other") {
    answer = countryAnswer(code);
  } else if (picked?.kind === "skip") {
    answer = SKIP_ANSWER;
  }

  return (
    <QuestionFrame
      env={env}
      request={request}
      title={`${check.name} (${check.isin})`}
      answer={answer}
      allowCancel={false}
    >
      {() => (
        <>
          <p>{uiText(language, "countryIntro")}</p>
          <fieldset class="field">
            <legend>{uiText(language, "countryQuestion")}</legend>
            {check.candidates.map((candidate) => {
              const origin = originTexts.get(candidate.origin);

              return (
                <label key={candidate.country} class="radio">
                  <input
                    type="radio"
                    name={radioName}
                    value={candidate.country}
                    checked={picked?.kind === "candidate" && picked.country === candidate.country}
                    onChange={() => setPicked({ kind: "candidate", country: candidate.country })}
                  />
                  <span>
                    <strong>{candidate.country}</strong>
                    {origin === undefined ? null : <span class="hint">{uiText(language, origin)}</span>}
                  </span>
                </label>
              );
            })}
            <label class="radio">
              <input
                type="radio"
                name={radioName}
                value="other"
                checked={picked?.kind === "other"}
                onChange={() => setPicked({ kind: "other" })}
              />
              {uiText(language, "countryOther")}
            </label>
            <label class="radio">
              <input
                type="radio"
                name={radioName}
                value="skip"
                checked={picked?.kind === "skip"}
                onChange={() => setPicked({ kind: "skip" })}
              />
              {uiText(language, "countrySkip")}
            </label>
          </fieldset>
          {picked?.kind !== "other" ? null : (
            <label class="field">
              <span>{uiText(language, "countryOtherInput")}</span>
              <input
                type="text"
                value={typed}
                maxLength={2}
                autocapitalize="characters"
                aria-describedby={errorId}
                aria-invalid={typedProblem}
                onInput={(event) => setTyped(event.currentTarget.value.toUpperCase())}
              />
            </label>
          )}
          <div id={errorId}>
            {!typedProblem ? null : (
              <p class="field-error" role="alert">
                {uiText(language, "countryOtherInvalid")}
              </p>
            )}
          </div>
        </>
      )}
    </QuestionFrame>
  );
}
