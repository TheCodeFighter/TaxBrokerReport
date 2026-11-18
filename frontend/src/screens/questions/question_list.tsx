import { uiTextWith } from "../../i18n/ui_texts";
import { questionKind, type DecisionRequest } from "../../run/questions";
import { LossQuestion } from "./loss_question";
import { QuantityQuestion } from "./quantity_question";
import type { QuestionEnv } from "./question_frame";
import { RatioQuestion } from "./ratio_question";

interface Props {
  env: QuestionEnv;
  requests: DecisionRequest[];
}

/** One screen for each open question of a form; a type without a screen shows nothing here. */
function QuestionView({ env, request }: { env: QuestionEnv; request: DecisionRequest }) {
  const kind = questionKind(request.type);

  switch (kind) {
    case "loss":
      return <LossQuestion env={env} request={request} />;
    case "ratio":
      return <RatioQuestion env={env} request={request} />;
    case "quantity":
      return <QuantityQuestion env={env} request={request} />;
    case undefined:
      return null;
  }
}

/**
 * The open questions of one form, above its messages. Each question starts fresh with every new
 * result: the request IDs are numbered again for each result, so an ID can name another question.
 */
export function QuestionList({ env, requests }: Props) {
  if (requests.length === 0) {
    return null;
  }

  return (
    <div class="questions">
      <h4>{uiTextWith(env.language, "questionsTitle", { count: requests.length })}</h4>
      {requests.map((request) => (
        <QuestionView key={`${env.revision}-${request.id}`} env={env} request={request} />
      ))}
    </div>
  );
}
