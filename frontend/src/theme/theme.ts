export type Theme = "dark" | "light";

/** The key of the choice in the local storage. It holds only `dark` or `light`. */
export const THEME_STORAGE_KEY = "tbr.theme";

/** Light is the default: the system setting is not used, because it does not work everywhere. */
export const DEFAULT_THEME: Theme = "light";

export interface ThemeStorage {
  getItem(key: string): string | null;
  setItem(key: string, value: string): void;
}

/**
 * The saved theme, or light when nothing valid is saved. The storage may be missing or throw (a
 * private window, blocked site data), so the page works without it.
 */
export function readTheme(getStorage: () => ThemeStorage): Theme {
  try {
    const saved = getStorage().getItem(THEME_STORAGE_KEY);

    return saved === "dark" || saved === "light" ? saved : DEFAULT_THEME;
  } catch {
    return DEFAULT_THEME;
  }
}

/** Saves the choice; a storage that fails is ignored. */
export function saveTheme(getStorage: () => ThemeStorage, theme: Theme): void {
  try {
    getStorage().setItem(THEME_STORAGE_KEY, theme);
  } catch {
    // The choice then lasts until the page is reloaded.
  }
}

/** The other theme: what the toggle switches to. */
export function otherTheme(theme: Theme): Theme {
  return theme === "dark" ? "light" : "dark";
}
