import { uiTextWith, uiText, type Language, type UiTextKey } from "../../i18n/ui_texts";
import { questionAnchor } from "../../run/questions";

/** Scrolls to an open question and moves the focus to its heading, so a keyboard user lands there. */
export function goToQuestion(requestId: string): void {
  const anchor = questionAnchor(requestId);

  document.getElementById(anchor)?.scrollIntoView({ block: "start" });
  document.getElementById(`${anchor}-title`)?.focus();
}

interface Props {
  language: Language;
  requestId: string;
  label: UiTextKey;
  /** The number in the label, when it has a `{count}` place. */
  count?: number;
  /** The main button of the page (filled); the others are outlined. */
  primary?: boolean;
}

/** A button that takes the user to one question. */
export function QuestionLink({ language, requestId, label, count, primary = false }: Props) {
  const text = count === undefined ? uiText(language, label) : uiTextWith(language, label, { count });

  return (
    <button type="button" class={primary ? "button" : "button button-secondary"} onClick={() => goToQuestion(requestId)}>
      {text}
    </button>
  );
}
