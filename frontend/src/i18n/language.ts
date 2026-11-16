import type { Language } from "./ui_texts";

/** The key of the choice in the local storage. It holds only `sl` or `en`. */
export const LANGUAGE_STORAGE_KEY = "tbr.language";

export const DEFAULT_LANGUAGE: Language = "sl";

export interface LanguageStorage {
  getItem(key: string): string | null;
  setItem(key: string, value: string): void;
}

/**
 * The saved language, or Slovenian when nothing valid is saved. The storage may be missing or
 * throw (a private window, blocked site data), so the page works without it.
 */
export function readLanguage(getStorage: () => LanguageStorage): Language {
  try {
    const saved = getStorage().getItem(LANGUAGE_STORAGE_KEY);

    return saved === "sl" || saved === "en" ? saved : DEFAULT_LANGUAGE;
  } catch {
    return DEFAULT_LANGUAGE;
  }
}

/** Saves the choice; a storage that fails is ignored. */
export function saveLanguage(getStorage: () => LanguageStorage, language: Language): void {
  try {
    getStorage().setItem(LANGUAGE_STORAGE_KEY, language);
  } catch {
    // The choice then lasts until the page is reloaded.
  }
}
