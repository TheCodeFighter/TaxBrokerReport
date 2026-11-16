import { uiText, type Language } from "../i18n/ui_texts";

interface Props {
  language: Language;
}

/** The no-liability warning, on the welcome page and on the About page. */
export function Disclaimer({ language }: Props) {
  return (
    <section class="disclaimer" aria-labelledby="disclaimer-title">
      <h2 id="disclaimer-title">{uiText(language, "disclaimerTitle")}</h2>
      <p>{uiText(language, "disclaimerText")}</p>
    </section>
  );
}
