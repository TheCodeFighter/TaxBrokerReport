import { useEffect, useState } from "preact/hooks";

import type { ApiClient } from "../api/client";
import { SessionEndedError } from "../api/client";
import { changeLogLevel, loadLogLevel, type LogLevel } from "../api/log_level";
import { uiText, type Language } from "../i18n/ui_texts";
import { AdvancedSection } from "./advanced_section";
import { SupportReportButton } from "./support_report_button";

interface Props {
  language: Language;
  apiFetch: ApiClient["apiFetch"];
}

/**
 * The Support page: saves the support report of the last run and, in the closed "Advanced" section,
 * sets the level of the log. Both are away from the report workflow.
 */
export function SupportPage({ language, apiFetch }: Props) {
  // The level of the log: read when the page opens, sent when the user changes it.
  const [logLevel, setLogLevel] = useState<LogLevel | undefined>();
  const [logLevelFailed, setLogLevelFailed] = useState(false);

  useEffect(() => {
    let cancelled = false;

    loadLogLevel(apiFetch).then(
      (level) => {
        if (!cancelled) {
          setLogLevel(level);
        }
      },
      // A lost session is shown by the page itself; the select stays off.
      () => undefined,
    );

    return () => {
      cancelled = true;
    };
  }, [apiFetch]);

  // A change goes to the server at once. The select shows it first; on a failure it goes back.
  const chooseLogLevel = async (level: LogLevel) => {
    const previous = logLevel;

    setLogLevel(level);
    setLogLevelFailed(false);

    try {
      const now = await changeLogLevel(apiFetch, level);

      setLogLevelFailed(now === undefined);
      setLogLevel(now ?? previous);
    } catch (caught) {
      setLogLevel(previous);

      // A lost session is shown by the page itself.
      if (!(caught instanceof SessionEndedError)) {
        setLogLevelFailed(true);
      }
    }
  };

  return (
    <main class="screen support">
      <h1 tabIndex={-1}>{uiText(language, "supportTitle")}</h1>
      <p>{uiText(language, "supportIntro")}</p>
      <SupportReportButton language={language} apiFetch={apiFetch} />
      <AdvancedSection
        language={language}
        apiFetch={apiFetch}
        level={logLevel}
        failed={logLevelFailed}
        onChange={(level) => void chooseLogLevel(level)}
      />
    </main>
  );
}
