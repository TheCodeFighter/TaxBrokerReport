import { describe, expect, it } from "vitest";

import { LANGUAGE_STORAGE_KEY, readLanguage, saveLanguage } from "./language";

function storageWith(value: string | null) {
  return { getItem: () => value, setItem: () => undefined };
}

describe("readLanguage", () => {
  it("is Slovenian when nothing is saved", () => {
    expect(readLanguage(() => storageWith(null))).toBe("sl");
  });

  it("returns a saved English choice", () => {
    expect(readLanguage(() => storageWith("en"))).toBe("en");
  });

  it("is Slovenian for a saved value that is not a language", () => {
    expect(readLanguage(() => storageWith("de"))).toBe("sl");
  });

  it("is Slovenian when the storage throws", () => {
    const throwingStorage = () => {
      throw new Error("blocked");
    };

    expect(readLanguage(throwingStorage)).toBe("sl");
  });
});

describe("saveLanguage", () => {
  it("saves under tbr.language", () => {
    const saved: Record<string, string> = {};
    const storage = { getItem: () => null, setItem: (key: string, value: string) => (saved[key] = value) };

    saveLanguage(() => storage, "en");

    expect(LANGUAGE_STORAGE_KEY).toBe("tbr.language");
    expect(saved).toEqual({ "tbr.language": "en" });
  });

  it("ignores a storage that throws", () => {
    const throwingStorage = () => {
      throw new Error("blocked");
    };

    expect(() => saveLanguage(throwingStorage, "en")).not.toThrow();
  });
});
