import { uiText, type Language } from "../i18n/ui_texts";

interface Props {
  language: Language;
}

/** The Manual page: only a note until the user guide is written. */
export function ManualPage({ language }: Props) {
  return (
    <main class="screen manual">
      <h1 tabIndex={-1}>{uiText(language, "manualTitle")}</h1>
      <p>{uiText(language, "manualLater")}</p>
    </main>
  );
}
