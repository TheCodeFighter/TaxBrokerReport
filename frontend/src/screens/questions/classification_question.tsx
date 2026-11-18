import { useState } from "preact/hooks";

import { uiText, uiTextWith } from "../../i18n/ui_texts";
import { shownClassLabels } from "../../run/income_questions";
import type { DecisionRequest } from "../../run/questions";
import { ChoiceGroup } from "./choice_group";
import { QuestionFrame, type QuestionEnv } from "./question_frame";

interface Props {
  env: QuestionEnv;
  request: DecisionRequest;
}

/** The class of a traded instrument the export does not classify. Nothing is preselected. */
export function ClassificationQuestion({ env, request }: Props) {
  const [choice, setChoice] = useState<string | undefined>(undefined);
  const check = request.classificationCheck;

  if (check === undefined) {
    return null;
  }

  const { language } = env;
  const labels = shownClassLabels(check.classLabels);

  return (
    <QuestionFrame
      env={env}
      request={request}
      title={`${check.name} (${check.isin})`}
      answer={choice}
      allowCancel
    >
      {() => (
        <>
          <p>{uiText(language, "classificationIntro")}</p>
          <p class="hint">
            {labels.length === 0
              ? uiText(language, "classLabelsNone")
              : uiTextWith(language, "classLabels", { labels: labels.join(", ") })}
          </p>
          <ChoiceGroup
            language={language}
            name={`class-${request.id}`}
            legend={uiText(language, "classificationQuestion")}
            options={request.options ?? []}
            value={choice}
            onChange={setChoice}
          />
        </>
      )}
    </QuestionFrame>
  );
}
