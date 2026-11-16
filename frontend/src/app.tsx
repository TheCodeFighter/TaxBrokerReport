import { useEffect, useMemo, useState } from "preact/hooks";

import { createApiClient } from "./api/client";
import { readLanguage, saveLanguage } from "./i18n/language";
import { createMessageLoader } from "./i18n/messages";
import { uiText, type Language, type UiTextKey } from "./i18n/ui_texts";
import { startSession, type SessionState } from "./session/login";
import { otherTheme, readTheme, saveTheme, type Theme } from "./theme/theme";
import logoUrl from "./assets/logo.webp";

type Screen = "checking" | SessionState;

/** The title and the text of each screen; one message and one action per screen. */
const screenTexts: Record<Screen, { title: UiTextKey; body: UiTextKey }> = {
  checking: { title: "checkingTitle", body: "checkingBody" },
  ready: { title: "readyTitle", body: "readyBody" },
  session_ended: { title: "sessionEndedTitle", body: "sessionEndedBody" },
  link_invalid: { title: "linkInvalidTitle", body: "linkInvalidBody" },
};

const languages: Language[] = ["sl", "en"];

const browserStorage = () => window.localStorage;

/** The icons are inline SVG (no icon pack, no image file); they take the color of the text. */
function SunIcon() {
  return (
    <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" aria-hidden="true">
      <circle cx="12" cy="12" r="4" />
      <path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4" />
    </svg>
  );
}

function MoonIcon() {
  return (
    <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
      <path d="M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8z" />
    </svg>
  );
}

export function App() {
  const [language, setLanguage] = useState<Language>(() => readLanguage(browserStorage));
  const [theme, setTheme] = useState<Theme>(() => readTheme(browserStorage));
  const [screen, setScreen] = useState<Screen>("checking");

  // One client for the page: a 401 or a lost server anywhere ends the session here.
  const client = useMemo(
    () => createApiClient((path, init) => window.fetch(path, init), () => setScreen("session_ended")),
    [],
  );
  const loadCatalogue = useMemo(() => createMessageLoader(client.apiFetch), [client]);

  useEffect(() => {
    document.documentElement.lang = language;
  }, [language]);

  useEffect(() => {
    document.documentElement.dataset.theme = theme;
  }, [theme]);

  useEffect(() => {
    let cancelled = false;

    void startSession(window, client.send).then((state) => {
      if (!cancelled) {
        setScreen(state);
      }
    });

    return () => {
      cancelled = true;
    };
  }, [client]);

  // The message texts load once after the login; the screens with diagnostics use them.
  useEffect(() => {
    if (screen === "ready") {
      loadCatalogue().catch(() => undefined);
    }
  }, [screen, loadCatalogue]);

  const chooseLanguage = (chosen: Language) => {
    setLanguage(chosen);
    saveLanguage(browserStorage, chosen);
  };

  const toggleTheme = () => {
    const chosen = otherTheme(theme);

    setTheme(chosen);
    saveTheme(browserStorage, chosen);
  };

  const texts = screenTexts[screen];
  const themeLabel = uiText(language, theme === "dark" ? "themeToLight" : "themeToDark");

  return (
    <>
      <header class="header">
        <div class="brand">
          <img class="logo" src={logoUrl} width="40" height="40" alt="" />
          <span class="brand-name">{uiText(language, "appName")}</span>
        </div>
        <div class="header-controls">
          <div class="language-switch" role="group" aria-label={uiText(language, "languageLabel")}>
            {languages.map((code) => (
              <button
                type="button"
                key={code}
                lang={code}
                aria-pressed={code === language}
                onClick={() => chooseLanguage(code)}
              >
                {code.toUpperCase()}
              </button>
            ))}
          </div>
          <button
            type="button"
            class="theme-toggle"
            aria-label={themeLabel}
            title={themeLabel}
            onClick={toggleTheme}
          >
            {theme === "dark" ? <SunIcon /> : <MoonIcon />}
          </button>
        </div>
      </header>
      <main class="screen">
        <h1>{uiText(language, texts.title)}</h1>
        {/* One live region for every screen, so a screen reader also reads the result. */}
        <p role="status">{uiText(language, texts.body)}</p>
      </main>
    </>
  );
}
