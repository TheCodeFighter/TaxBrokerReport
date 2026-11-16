export type Language = "sl" | "en";

export interface Texts {
  sl: string;
  en: string;
}

/**
 * Every text of the interface itself, in both languages. A missing language is a type error.
 * The diagnostic messages are not here: they come from the server (`/api/messages`).
 */
export const uiTexts = {
  appName: { sl: "TaxBrokerReport", en: "TaxBrokerReport" },
  languageLabel: { sl: "Jezik", en: "Language" },
  themeToLight: { sl: "Preklopi na svetlo temo", en: "Switch to the light theme" },
  themeToDark: { sl: "Preklopi na temno temo", en: "Switch to the dark theme" },
  checkingTitle: { sl: "Povezovanje …", en: "Connecting …" },
  checkingBody: {
    sl: "Preverjam, ali je aplikacija pripravljena.",
    en: "Checking that the application is ready.",
  },
  readyTitle: { sl: "Aplikacija je pripravljena", en: "The application is ready" },
  readyBody: {
    sl: "Prijava je uspela. Uvoz datotek in poročila pridejo v naslednjih različicah.",
    en: "You are signed in. Importing files and the reports come in later versions.",
  },
  sessionEndedTitle: { sl: "Odprite povezavo znova", en: "Open the link again" },
  sessionEndedBody: {
    sl: "Seja je končana ali aplikacija ni več zagnana. Zaženite aplikacijo in odprite novo povezavo, ki jo izpiše.",
    en: "The session has ended or the application is no longer running. Start the application and open the new link that it prints.",
  },
  linkInvalidTitle: { sl: "Povezava ne velja več", en: "The link is no longer valid" },
  linkInvalidBody: {
    sl: "Povezava deluje samo enkrat in 2 minuti. Ustavite aplikacijo, jo znova zaženite in odprite novo povezavo, ki jo izpiše.",
    en: "A link works once and for 2 minutes. Stop the application, start it again and open the new link that it prints.",
  },
  runChanged: {
    sl: "Podatki so se spremenili. Osvežite stran in poskusite znova.",
    en: "The data changed. Reload the page and try again.",
  },
  unknownMessage: { sl: "Neznano sporočilo", en: "Unknown message" },
} satisfies Record<string, Texts>;

export type UiTextKey = keyof typeof uiTexts;

export function uiText(language: Language, key: UiTextKey): string {
  return uiTexts[key][language];
}
