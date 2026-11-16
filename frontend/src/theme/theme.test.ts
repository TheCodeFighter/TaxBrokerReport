import { describe, expect, it } from "vitest";

import { otherTheme, readTheme, saveTheme, THEME_STORAGE_KEY } from "./theme";

function storageWith(value: string | null) {
  return { getItem: () => value, setItem: () => undefined };
}

describe("readTheme", () => {
  it("is light when nothing is saved", () => {
    expect(readTheme(() => storageWith(null))).toBe("light");
  });

  it("returns a saved light choice", () => {
    expect(readTheme(() => storageWith("light"))).toBe("light");
  });

  it("returns a saved dark choice", () => {
    expect(readTheme(() => storageWith("dark"))).toBe("dark");
  });

  it("is light for a saved value that is not a theme", () => {
    expect(readTheme(() => storageWith("blue"))).toBe("light");
  });

  it("is light when the storage throws", () => {
    const throwingStorage = () => {
      throw new Error("blocked");
    };

    expect(readTheme(throwingStorage)).toBe("light");
  });
});

describe("saveTheme", () => {
  it("saves under tbr.theme", () => {
    const saved: Record<string, string> = {};
    const storage = { getItem: () => null, setItem: (key: string, value: string) => (saved[key] = value) };

    saveTheme(() => storage, "dark");

    expect(THEME_STORAGE_KEY).toBe("tbr.theme");
    expect(saved).toEqual({ "tbr.theme": "dark" });
  });

  it("ignores a storage that throws", () => {
    const throwingStorage = () => {
      throw new Error("blocked");
    };

    expect(() => saveTheme(throwingStorage, "dark")).not.toThrow();
  });
});

describe("otherTheme", () => {
  it("switches between dark and light", () => {
    expect(otherTheme("dark")).toBe("light");
    expect(otherTheme("light")).toBe("dark");
  });
});
