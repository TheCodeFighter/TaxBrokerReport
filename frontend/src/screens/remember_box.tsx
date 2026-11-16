import { useEffect, useRef } from "preact/hooks";

import { uiText, type Language } from "../i18n/ui_texts";
import type { RememberState } from "../run/profile";

interface Props {
  language: Language;
  state: RememberState;
  /** Where the file is, as text (no user name). */
  hint: string;
  /** The notices about the saved profile and about the last save or delete. */
  notices: string[];
  onTick: () => void;
  onUntick: () => void;
  onCancel: () => void;
  onConfirm: () => void;
}

/**
 * The "Remember my details" box with its hint. Unticking while a file is saved opens a warning
 * in the page, with the safe choice (Cancel) focused; Escape is Cancel. No browser dialog.
 */
export function RememberBox({ language, state, hint, notices, onTick, onUntick, onCancel, onConfirm }: Props) {
  const cancelButton = useRef<HTMLButtonElement>(null);
  const warningOpen = state === "confirmDelete" || state === "deleting";

  useEffect(() => {
    if (state === "confirmDelete") {
      cancelButton.current?.focus();
    }
  }, [state]);

  return (
    <div class="remember">
      <label class="radio">
        <input
          type="checkbox"
          checked={state === "on"}
          disabled={state === "deleting"}
          onChange={(event) => (event.currentTarget.checked ? onTick() : onUntick())}
        />
        {uiText(language, "profileRemember")}
      </label>
      <p class="hint">{hint}</p>
      {warningOpen ? (
        <div
          class="remember-warning"
          role="alert"
          onKeyDown={(event) => {
            if (event.key === "Escape" && state === "confirmDelete") {
              onCancel();
            }
          }}
        >
          <p>{uiText(language, "profileWarning")}</p>
          <div class="remember-actions">
            <button
              type="button"
              class="button button-secondary"
              ref={cancelButton}
              disabled={state === "deleting"}
              onClick={onCancel}
            >
              {uiText(language, "profileCancel")}
            </button>
            <button type="button" class="button" disabled={state === "deleting"} onClick={onConfirm}>
              {uiText(language, "profileConfirm")}
            </button>
          </div>
        </div>
      ) : null}
      {notices.length === 0 ? null : (
        <div role="status">
          {notices.map((text) => (
            <p key={text} class="notice">
              {text}
            </p>
          ))}
        </div>
      )}
    </div>
  );
}
