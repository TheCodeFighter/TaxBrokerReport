import { uiText, type Language } from "../i18n/ui_texts";
import { Disclaimer } from "./disclaimer";

/** The project's repository. The build check allows exactly this address. */
const repositoryUrl = "https://github.com/TheCodeFighter/TaxBrokerReport";

interface Props {
  language: Language;
}

/** The About page: name, license, privacy promise, warning and third-party notices. */
export function AboutPage({ language }: Props) {
  return (
    <main class="screen about">
      <h1 tabIndex={-1}>{uiText(language, "aboutTitle")}</h1>
      <p class="about-name">{uiText(language, "appName")}</p>
      <p>{uiText(language, "aboutLicense")}</p>
      <p>{uiText(language, "aboutPrivacy")}</p>
      <p>{uiText(language, "aboutThirdParty")}</p>
      <p>
        {uiText(language, "aboutSource")}{" "}
        {/* Opens in a new tab and sends no referrer, so the local address stays on this computer. */}
        <a href={repositoryUrl} target="_blank" rel="noopener noreferrer">
          {uiText(language, "aboutSourceLink")}
        </a>
      </p>
      <Disclaimer language={language} />
    </main>
  );
}
