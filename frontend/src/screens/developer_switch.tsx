import { useState } from "preact/hooks";

import { uiText, type Language } from "../i18n/ui_texts";

interface Props {
  language: Language;
  on: boolean;
  /** True while a run is sent: the switch waits, like the form. */
  disabled: boolean;
  onChange: (on: boolean) => void;
}

/**
 * The developer mode switch at the very bottom of the page, under a dividing line, centered. The
 * button opens a panel with the checkbox; while the mode is on the button says so in red, also
 * when the panel is closed. The mode is never saved.
 */
export function DeveloperSwitch({ language, on, disabled, onChange }: Props) {
  const [open, setOpen] = useState(false);

  return (
    <div class="developer-switch">
      <button
        type="button"
        class={on ? "button button-secondary button-on" : "button button-secondary"}
        aria-expanded={open}
        aria-controls="developer-panel"
        onClick={() => setOpen(!open)}
      >
        {uiText(language, on ? "developerModeButtonOn" : "developerModeTitle")}
      </button>
      {open ? (
        <div id="developer-panel" class="developer-panel">
          <label class="radio">
            <input
              type="checkbox"
              checked={on}
              disabled={disabled}
              onChange={(event) => onChange(event.currentTarget.checked)}
            />
            {uiText(language, "developerModeLabel")}
          </label>
          <p class={on ? "danger-notice" : "hint"}>{uiText(language, "developerModeNote")}</p>
        </div>
      ) : null}
    </div>
  );
}
