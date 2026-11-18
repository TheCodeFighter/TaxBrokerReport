import { uiText, type Language } from "../i18n/ui_texts";
import { hashOf } from "../navigation/pages";
import { Disclaimer } from "./disclaimer";
import logoUrl from "../assets/logo.webp";

interface Props {
  language: Language;
}

/** The welcome page: what the application does, the way into the report screen, the warning. */
export function HomePage({ language }: Props) {
  return (
    <main class="screen welcome">
      <img class="welcome-logo" src={logoUrl} width="288" height="288" alt="" />
      <h1 tabIndex={-1}>{uiText(language, "appName")}</h1>
      <p class="welcome-line">{uiText(language, "welcomeLine")}</p>
      <div class="welcome-actions">
        <a class="button" href={hashOf("report")}>{uiText(language, "welcomeStart")}</a>
        <a class="button button-secondary" href={hashOf("manual")}>{uiText(language, "navManual")}</a>
      </div>
      <Disclaimer language={language} />
    </main>
  );
}
