import { useState } from "preact/hooks";

import { uiText, uiTextWith } from "../../i18n/ui_texts";
import { linkedTexts, sourceLabel } from "../../run/diagnostics_view";
import { DEVELOPER_ANSWER, failureChoices, failureForm } from "../../run/failure_question";
import type { DecisionRequest } from "../../run/questions";
import { brokerTitle } from "../../run/result_view";
import { ChoiceGroup } from "./choice_group";
import { QuestionFrame, type QuestionEnv } from "./question_frame";

interface Props {
  env: QuestionEnv;
  request: DecisionRequest;
}

/**
 * The failure question of one form (Doh-KDVP, Doh-Div or Doh-Obr): it lists every investment and
 * broker with its reasons, then asks to leave them out or to make no XML for the form. Nothing is
 * preselected; the click on "Send answer" is the confirmation. The developer export choice, when
 * offered, is red and only leads to its own confirmation screen.
 */
export function FailureQuestion({ env, request }: Props) {
  const [choice, setChoice] = useState<string | undefined>(undefined);
  const form = failureForm(request.type);

  if (form === undefined) {
    return null;
  }

  const { language, catalogue, diagnostics, fileLabels } = env;
  const investments = request.investments ?? [];
  const brokers = request.brokers ?? [];

  const reasons = (diagnosticIds: string[] | undefined) =>
    linkedTexts(diagnosticIds ?? [], diagnostics, catalogue, language).map((text) => (
      <p key={text} class="hint">
        {text}
      </p>
    ));

  return (
    <QuestionFrame
      env={env}
      request={request}
      title={uiTextWith(language, "failureTitle", { form })}
      answer={choice}
      allowCancel
      cancelText="failureCancelWhat"
    >
      {() => (
        <>
          <p>{uiTextWith(language, "failureIntro", { form })}</p>
          {investments.length === 0 ? null : (
            <>
              <h6>{uiText(language, "failureInvestments")}</h6>
              <ul class="plain-list">
                {investments.map((investment) => (
                  <li key={investment.isin}>
                    {`${investment.name} (${investment.isin})`}
                    {reasons(investment.diagnosticIds)}
                  </li>
                ))}
              </ul>
            </>
          )}
          {brokers.length === 0 ? null : (
            <>
              <h6>{uiText(language, "failureBrokers")}</h6>
              <ul class="plain-list">
                {brokers.map((broker) => (
                  <li key={broker.broker}>
                    {brokerTitle(broker.broker, language)}
                    {reasons(broker.diagnosticIds)}
                  </li>
                ))}
              </ul>
            </>
          )}
          {request.sources.length === 0 ? null : (
            <details>
              <summary>{uiTextWith(language, "failureSources", { count: request.sources.length })}</summary>
              <ul class="plain-list">
                {request.sources.map((source) => (
                  <li key={source.sourceId}>{sourceLabel(source, fileLabels, language)}</li>
                ))}
              </ul>
            </details>
          )}
          <ChoiceGroup
            language={language}
            name={`failure-${request.id}`}
            legend={uiText(language, "failureLegend")}
            options={failureChoices(request.options)}
            value={choice}
            onChange={setChoice}
            dangerWords={[DEVELOPER_ANSWER]}
          />
        </>
      )}
    </QuestionFrame>
  );
}
