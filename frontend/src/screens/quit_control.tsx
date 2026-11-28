import { useEffect, useRef, useState } from "preact/hooks";

import type { ApiClient } from "../api/client";
import { uiText, type Language } from "../i18n/ui_texts";
import { requestQuit } from "../session/quit";

interface Props {
  language: Language;
  apiFetch: ApiClient["apiFetch"];
  /** The server said it is stopping: the page shows its last screen. */
  onStopped: () => void;
}

function PowerIcon() {
  return (
    <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
      <path d="M12 2v10" />
      <path d="M18.4 6.6a9 9 0 1 1-12.8 0" />
    </svg>
  );
}

/**
 * The "Quit the program" button of the header. It asks once in the page (no browser dialog), with
 * the safe choice (Cancel) focused; Escape is Cancel. Only Quit sends the call.
 */
export function QuitControl({ language, apiFetch, onStopped }: Props) {
  const [asking, setAsking] = useState(false);
  const [busy, setBusy] = useState(false);
  const [failed, setFailed] = useState(false);
  const cancelButton = useRef<HTMLButtonElement>(null);

  useEffect(() => {
    if (asking) {
      cancelButton.current?.focus();
    }
  }, [asking]);

  const quit = async () => {
    setBusy(true);
    setFailed(false);

    const outcome = await requestQuit(apiFetch);

    if (outcome === "stopped") {
      onStopped();

      return;
    }

    // "ended": the client has already shown the screen of a lost server.
    setBusy(false);
    setFailed(outcome === "failed");
  };

  return (
    <>
      <button type="button" class="quit-button" aria-expanded={asking} onClick={() => setAsking(true)}>
        <PowerIcon />
        <span class="quit-divider" aria-hidden="true" />
        {uiText(language, "quitButton")}
      </button>
      {asking ? (
        <div
          class="quit-box"
          role="alert"
          onKeyDown={(event) => {
            if (event.key === "Escape" && !busy) {
              setAsking(false);
            }
          }}
        >
          <p>{uiText(language, "quitWarning")}</p>
          {failed ? <p class="quit-failed">{uiText(language, "quitFailed")}</p> : null}
          <div class="quit-actions">
            <button
              type="button"
              class="button button-secondary"
              ref={cancelButton}
              disabled={busy}
              onClick={() => setAsking(false)}
            >
              {uiText(language, "quitCancel")}
            </button>
            <button type="button" class="button" disabled={busy} onClick={() => void quit()}>
              {uiText(language, "quitConfirm")}
            </button>
          </div>
        </div>
      ) : null}
    </>
  );
}
