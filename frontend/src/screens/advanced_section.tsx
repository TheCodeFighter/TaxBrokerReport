import { LOG_LEVELS, type LogLevel } from "../api/log_level";
import type { Language, UiTextKey } from "../i18n/ui_texts";
import { uiText } from "../i18n/ui_texts";

interface Props {
  language: Language;
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

/** The closed "Advanced" section: the level of the log. A change is sent at once, with no button. */
export function AdvancedSection({ language, level, failed, onChange }: Props) {
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
    </details>
  );
}
