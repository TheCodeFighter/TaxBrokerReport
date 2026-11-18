import { useState } from "preact/hooks";

import type { ApiClient } from "../api/client";
import { SessionEndedError } from "../api/client";
import { loadLogFile } from "../api/log_file";
import { LOG_LEVELS, type LogLevel } from "../api/log_level";
import { saveFile } from "../api/support_report";
import type { Language, UiTextKey } from "../i18n/ui_texts";
import { uiText } from "../i18n/ui_texts";

interface Props {
  language: Language;
  apiFetch: ApiClient["apiFetch"];
  /** The level the server logs at; empty until the server has answered. */
  level: LogLevel | undefined;
  /** True when the last change was refused or failed. */
  failed: boolean;
  onChange: (level: LogLevel) => void;
}

const levelTexts: Record<LogLevel, UiTextKey> = {
  error: "logLevelError",
  warn: "logLevelWarn",
  info: "logLevelInfo",
  debug: "logLevelDebug",
};

/** Saves the log as one file; only when the developer asks for it. */
function LogFileButton({ language, apiFetch }: Pick<Props, "language" | "apiFetch">) {
  const [busy, setBusy] = useState(false);
  const [problem, setProblem] = useState<UiTextKey | undefined>(undefined);

  const save = async () => {
    setBusy(true);
    setProblem(undefined);

    try {
      const answer = await loadLogFile(apiFetch);

      if ("file" in answer) {
        saveFile(answer.file);
      } else {
        setProblem(answer.problem === "no_log" ? "logFileNone" : "runErrorUnexpected");
      }
    } catch (caught) {
      // A lost session is shown by the page itself; any other failure is an unexpected answer.
      if (!(caught instanceof SessionEndedError)) {
        setProblem("runErrorUnexpected");
      }
    } finally {
      setBusy(false);
    }
  };

  return (
    <div class="log-file">
      <button type="button" class="button" disabled={busy} onClick={() => void save()}>
        {uiText(language, "logFileSave")}
      </button>
      <div role="alert">{problem === undefined ? null : <p class="error">{uiText(language, problem)}</p>}</div>
    </div>
  );
}

/**
 * The closed "Advanced" section: the level of the log, sent at once with no button, and the button
 * that saves the log file.
 */
export function AdvancedSection({ language, apiFetch, level, failed, onChange }: Props) {
  return (
    <details class="advanced">
      <summary>{uiText(language, "advancedTitle")}</summary>
      <label class="field">
        <span>{uiText(language, "logLevelLabel")}</span>
        <select
          value={level}
          disabled={level === undefined}
          aria-describedby="log-level-hint"
          onChange={(event) => onChange(event.currentTarget.value as LogLevel)}
        >
          {LOG_LEVELS.map((name) => (
            <option key={name} value={name}>
              {uiText(language, levelTexts[name])}
            </option>
          ))}
        </select>
      </label>
      <p class="hint" id="log-level-hint">
        {uiText(language, "logLevelHint")}
      </p>
      {failed ? (
        <p class="error" role="alert">
          {uiText(language, "logLevelFailed")}
        </p>
      ) : null}
      <LogFileButton language={language} apiFetch={apiFetch} />
    </details>
  );
}
