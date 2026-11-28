import { useEffect, useMemo, useRef, useState } from "preact/hooks";

import { createApiClient, type SessionEndReason } from "./api/client";
import { loadVersion } from "./api/version";
import { readLanguage, saveLanguage } from "./i18n/language";
import { createMessageLoader, type MessageCatalogue } from "./i18n/messages";
import { uiText, uiTextWith, type Language, type UiTextKey } from "./i18n/ui_texts";
import { hashOf, pageFromHash, pages, type Page } from "./navigation/pages";
import { AboutPage } from "./screens/about_page";
import { SupportPage } from "./screens/support_page";
import { HomePage } from "./screens/home_page";
import { ManualPage } from "./screens/manual_page";
import { QuitControl } from "./screens/quit_control";
import { RunScreen } from "./screens/run_screen";
import { startSession, type SessionState } from "./session/login";
import { closeThisTab } from "./session/quit";
import { createTabReporter, makeTabId } from "./session/tabs";
import { otherTheme, readTheme, saveTheme, type Theme } from "./theme/theme";
import logoUrl from "./assets/logo.webp";

/** `server_stopped`: the server cannot be reached. `quit`: the user stopped the program here. */
type Screen = "checking" | "server_stopped" | "quit" | SessionState;

/** The title and the text of each screen; one message and one action per screen. */
const screenTexts: Record<Exclude<Screen, "ready">, { title: UiTextKey; body: UiTextKey }> = {
  checking: { title: "checkingTitle", body: "checkingBody" },
  server_stopped: { title: "serverStoppedTitle", body: "serverStoppedBody" },
  quit: { title: "quitStoppedTitle", body: "quitStoppedBody" },
  session_ended: { title: "sessionEndedTitle", body: "sessionEndedBody" },
  link_invalid: { title: "linkInvalidTitle", body: "linkInvalidBody" },
};

const languages: Language[] = ["sl", "en"];

/** The text of each header link. */
const navTexts: Record<Page, UiTextKey> = {
  home: "navHome",
  report: "navReport",
  manual: "navManual",
  support: "navSupport",
  about: "navAbout",
};

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
  const [catalogue, setCatalogue] = useState<MessageCatalogue | undefined>();
  const [page, setPage] = useState<Page>(() => pageFromHash(window.location.hash));
  // The version of the server, shown in the footer: undefined until asked, null when not known.
  const [version, setVersion] = useState<string | null | undefined>();
  const shownPage = useRef(page);

  // One client for the page: a 401 or a lost server anywhere ends the session here. A lost server
  // after the user chose Quit keeps the screen of the quit.
  const client = useMemo(
    () =>
      createApiClient((path, init) => window.fetch(path, init), (reason: SessionEndReason) =>
        setScreen((current) =>
          current === "quit" ? current : reason === "unreachable" ? "server_stopped" : "session_ended",
        ),
      ),
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

  // The server stops when the last page is closed: tell it when this page is open and closed. A
  // page restored from the browser's back/forward cache is open again (`pageshow`).
  useEffect(() => {
    if (screen !== "ready") {
      return undefined;
    }

    const reporter = createTabReporter(client.send, makeTabId((bytes) => window.crypto.getRandomValues(bytes)));
    const onShow = () => reporter.open();
    const onHide = () => reporter.close();

    reporter.open();
    window.addEventListener("pageshow", onShow);
    window.addEventListener("pagehide", onHide);

    return () => {
      window.removeEventListener("pageshow", onShow);
      window.removeEventListener("pagehide", onHide);
    };
  }, [screen, client]);

  // The message texts load once after the login; the screens with diagnostics use them.
  useEffect(() => {
    if (screen === "ready") {
      loadCatalogue().then(setCatalogue, () => undefined);
    }
  }, [screen, loadCatalogue]);

  // The back button and a reload keep the page, because the page is the address hash.
  useEffect(() => {
    const onHashChange = () => setPage(pageFromHash(window.location.hash));

    window.addEventListener("hashchange", onHashChange);

    return () => window.removeEventListener("hashchange", onHashChange);
  }, []);

  // A new page takes the focus, so a screen reader reads it. Not on the first load.
  useEffect(() => {
    if (screen === "ready" && shownPage.current !== page) {
      document.querySelector<HTMLElement>("main:not([hidden]) h1")?.focus();
    }

    shownPage.current = page;
  }, [screen, page]);

  // The version is asked once after the login.
  useEffect(() => {
    if (screen === "ready") {
      loadVersion(client.apiFetch).then(
        (loaded) => setVersion(loaded ?? null),
        () => setVersion(null),
      );
    }
  }, [screen, client]);

  const chooseLanguage = (chosen: Language) => {
    setLanguage(chosen);
    saveLanguage(browserStorage, chosen);
  };

  const toggleTheme = () => {
    const chosen = otherTheme(theme);

    setTheme(chosen);
    saveTheme(browserStorage, chosen);
  };

  const texts = screen === "ready" ? undefined : screenTexts[screen];
  const themeLabel = uiText(language, theme === "dark" ? "themeToLight" : "themeToDark");

  return (
    <>
      <header class="header">
        <a class="brand" href={hashOf("home")}>
          <img class="logo" src={logoUrl} width="40" height="40" alt="" />
          <span class="brand-name">{uiText(language, "appName")}</span>
        </a>
        {texts === undefined ? (
          <nav class="nav" aria-label={uiText(language, "navLabel")}>
            {pages.map((target) => (
              <a key={target} href={hashOf(target)} aria-current={target === page ? "page" : undefined}>
                {uiText(language, navTexts[target])}
              </a>
            ))}
          </nav>
        ) : null}
        <div class="header-controls">
          {texts === undefined ? (
            <QuitControl language={language} apiFetch={client.apiFetch} onStopped={() => {
                setScreen("quit");
                closeThisTab(() => window.close());
              }} />
          ) : null}
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
      {texts === undefined ? (
        <>
          {page === "home" ? <HomePage language={language} /> : null}
          {page === "manual" ? <ManualPage language={language} /> : null}
          {page === "support" ? <SupportPage language={language} apiFetch={client.apiFetch} /> : null}
          {page === "about" ? <AboutPage language={language} /> : null}
          {/* The run screen stays mounted and only hidden, so the chosen files and the result stay. */}
          <main class="screen screen-wide" hidden={page !== "report"}>
            <h1 tabIndex={-1}>{uiText(language, "navReport")}</h1>
            <RunScreen language={language} apiFetch={client.apiFetch} catalogue={catalogue} />
          </main>
          {version === undefined || version === null ? null : (
            <footer class="footer">{uiTextWith(language, "versionLabel", { version })}</footer>
          )}
        </>
      ) : (
        <main class="screen">
          <h1>{uiText(language, texts.title)}</h1>
          {/* One live region for every screen, so a screen reader also reads the result. */}
          <p role="status">{uiText(language, texts.body)}</p>
        </main>
      )}
    </>
  );
}
